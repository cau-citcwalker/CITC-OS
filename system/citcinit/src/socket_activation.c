/*
 * socket_activation.c - CITC OS 소켓 활성화 구현
 * ================================================
 *
 * 소켓 활성화 시스템의 핵심 구현 파일.
 *
 * 이 파일에서 배우는 것들:
 *   - Unix domain socket 생성 (socket + bind + listen)
 *   - poll() 기반 이벤트 루프
 *   - self-pipe 트릭 (시그널 → poll() 깨우기)
 *   - fd 전달 패턴 (LISTEN_FDS 프로토콜)
 *
 * 핵심 함수들:
 *   socket(AF_UNIX, SOCK_STREAM, 0) — Unix domain socket 생성
 *   bind()                           — 소켓에 경로 바인딩
 *   listen()                         — 연결 대기 모드로 전환
 *   poll()                           — 여러 fd를 동시에 감시
 *   pipe()                           — self-pipe 생성
 *
 * 전체 흐름:
 *   1. sa_init()              → 모든 소켓 활성화 서비스의 listen 소켓 생성
 *   2. sa_create_signal_pipe() → self-pipe 생성
 *   3. sa_build_poll_fds()    → poll() 배열 구성
 *   4. poll()                  → 이벤트 대기
 *   5. sa_handle_events()     → 소켓 활동 감지 → 서비스 시작
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "service.h"
#include "socket_activation.h"

/* 로그 매크로 */
#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_RESET  "\033[0m"

#define LOG_OK(fmt, ...) \
	printf(COLOR_GREEN "[  OK  ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_FAIL(fmt, ...) \
	printf(COLOR_RED   "[ FAIL ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
	printf(COLOR_BLUE  "[ INFO ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
	printf(COLOR_YELLOW "[ WARN ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)

/* ============================================================
 * Self-pipe
 * ============================================================
 *
 * self-pipe 트릭:
 *   시그널 핸들러는 async-signal-safe 함수만 호출할 수 있음.
 *   printf(), malloc() 등은 호출 불가! (교착 상태 위험)
 *
 *   하지만 write()는 async-signal-safe.
 *   pipe를 만들어서 시그널 핸들러에서 write(1바이트) →
 *   메인 루프의 poll()이 pipe의 POLLIN을 감지하여 깨어남.
 *
 *   이렇게 하면 시그널을 "fd 이벤트"로 변환할 수 있음.
 *   (현대 Linux에서는 signalfd()가 있지만, 이것이 더 이식성 있음)
 *
 *   signal_pipe[0] = 읽기 끝 (poll에서 감시)
 *   signal_pipe[1] = 쓰기 끝 (시그널 핸들러에서 write)
 */
static int signal_pipe[2] = { -1, -1 };

/* ============================================================
 * 소켓 활성화 초기화
 * ============================================================
 *
 * 등록된 모든 서비스를 순회하면서
 * socket_path가 설정된 서비스의 Unix domain socket을 생성.
 *
 * Unix domain socket이란?
 *   네트워크가 아닌 같은 시스템 내의 프로세스 간 통신에 사용.
 *   파일 경로를 주소로 사용 (예: /tmp/citc-display-0).
 *   TCP 소켓과 API가 동일하지만 네트워크를 거치지 않아 빠름.
 *
 *   socket(AF_UNIX, SOCK_STREAM, 0) → Unix stream socket 생성
 *   AF_UNIX (= AF_LOCAL): "같은 머신 내 통신"
 *   SOCK_STREAM: TCP처럼 연결 지향, 순서 보장
 */
int sa_init(void)
{
	int created = 0;

	/*
	 * 외부에서 서비스 배열에 접근할 수 없으므로
	 * svc_find_by_listen_fd()를 사용하여 탐색.
	 *
	 * 하지만 초기화 시에는 모든 서비스를 순회해야 함.
	 * → svc_find_by_listen_fd()는 이미 listen_fd가 설정된 것만 찾으므로
	 *   여기서는 직접 서비스 배열 접근이 필요.
	 *
	 * 해결: service.h에 선언된 함수를 통해 처리.
	 * 실제로는 svc_set_socket()으로 설정된 서비스가 있는지
	 * 모든 서비스를 순회 → listen_fd를 확인.
	 *
	 * 외부에서 접근할 수 없는 static 배열이므로,
	 * 이 함수는 각 서비스의 socket_path를 확인하기 위해
	 * service.c의 내부 구조에 접근해야 합니다.
	 *
	 * 하지만 캡슐화를 위해 svc_find_by_listen_fd 등
	 * service.h에 있는 API만 사용합니다.
	 *
	 * 대안: 여기서 for루프로 알려진 서비스 이름을 직접 시도하는 대신,
	 *       service.c에 sa용 헬퍼를 추가.
	 *
	 * 가장 깔끔한 방법: service.c에
	 * "소켓 활성화 서비스 순회" 함수를 제공.
	 * 하지만 여기서는 단순화를 위해 service.c에
	 * sa_setup_listen_sockets()를 export합니다.
	 *
	 * → 실제 소켓 생성 로직을 여기에, 서비스 순회는 service.c에서 호출.
	 *
	 * 최종 결정: sa_init()에서 sa_create_listen_socket()을 export하고,
	 * service.c의 svc_start_all() 직전에 소켓을 생성하도록 합니다.
	 * 하지만 이건 너무 복잡해지므로...
	 *
	 * 간단한 방법: sa_init()가 main.c에서 호출되고,
	 * 내부적으로 svc_find_by_listen_fd(-1)로는 못 찾으므로
	 * 별도의 콜백/이터레이터 패턴을 사용.
	 *
	 * 실용적 해결: socket_activation.c가 service.c의 internal에
	 * 접근할 수 있게 합니다 (같은 프로젝트 내이므로).
	 */

	/*
	 * 실용적 접근: extern으로 services 배열에 접근.
	 * 이상적이지는 않지만, 같은 init 프로세스 내의 모듈이므로 괜찮음.
	 * (systemd도 내부적으로 이렇게 함)
	 */
	extern service_t services[];
	extern int num_services;

	for (int i = 0; i < num_services; i++) {
		service_t *svc = &services[i];
		struct sockaddr_un addr;
		int fd;

		/* 소켓 활성화 서비스가 아니면 건너뛰기 */
		if (!svc->socket_activated || svc->socket_path[0] == '\0')
			continue;

		/* 이전 소켓 파일 정리 (비정상 종료 시 남아있을 수 있음) */
		unlink(svc->socket_path);

		/* Unix domain socket 생성 */
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			LOG_FAIL("Socket create failed for '%s': %s",
				 svc->name, strerror(errno));
			continue;
		}

		/* 소켓 주소 설정 */
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, svc->socket_path,
			sizeof(addr.sun_path) - 1);

		/* bind: 소켓에 파일 경로 바인딩 */
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			LOG_FAIL("Socket bind failed for '%s' (%s): %s",
				 svc->name, svc->socket_path, strerror(errno));
			close(fd);
			continue;
		}

		/*
		 * listen(backlog=8):
		 *   연결 대기 큐의 최대 크기.
		 *   서비스가 아직 시작되지 않았을 때 들어오는 연결은
		 *   커널이 이 큐에 보관. 서비스가 시작되면 accept()로 꺼냄.
		 *
		 *   이것이 소켓 활성화의 핵심:
		 *   소켓은 이미 listen 상태이므로 클라이언트는
		 *   서비스가 시작될 때까지 연결이 큐잉됨 (타임아웃 전까지).
		 */
		if (listen(fd, 8) < 0) {
			LOG_FAIL("Socket listen failed for '%s': %s",
				 svc->name, strerror(errno));
			close(fd);
			unlink(svc->socket_path);
			continue;
		}

		/* 논블로킹 설정 (poll에서 사용) */
		if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
			LOG_FAIL("fcntl O_NONBLOCK failed for '%s': %s",
				 svc->name, strerror(errno));
			close(fd);
			unlink(svc->socket_path);
			continue;
		}

		svc->listen_fd = fd;
		created++;

		LOG_OK("Socket ready: %s (fd=%d, service='%s')",
		       svc->socket_path, fd, svc->name);
	}

	if (created > 0)
		LOG_INFO("Socket activation: %d socket(s) listening", created);

	return created;
}

/* ============================================================
 * Self-pipe 생성
 * ============================================================ */
int sa_create_signal_pipe(void)
{
	if (pipe(signal_pipe) < 0) {
		LOG_FAIL("Self-pipe creation failed: %s", strerror(errno));
		return -1;
	}

	/*
	 * 양쪽 모두 논블로킹으로 설정.
	 *
	 * 쓰기 쪽 (signal_pipe[1]):
	 *   시그널 핸들러에서 write() → 블로킹되면 안 됨!
	 *   논블로킹이면 파이프가 가득 차도 즉시 반환.
	 *
	 * 읽기 쪽 (signal_pipe[0]):
	 *   poll()에서 감시하므로 논블로킹이어야 함.
	 */
	fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK);

	return 0;
}

/* ============================================================
 * 시그널 알림 (시그널 핸들러에서 호출)
 * ============================================================
 *
 * 중요: async-signal-safe 함수만 사용!
 *   OK:  write(), _exit(), getpid()
 *   NO:  printf(), malloc(), syslog()
 */
void sa_signal_notify(void)
{
	int save_errno = errno;  /* errno 보존 (중요!) */
	char byte = 1;

	if (signal_pipe[1] >= 0)
		(void)!write(signal_pipe[1], &byte, 1);

	errno = save_errno;  /* errno 복원 */
}

/* ============================================================
 * poll() fd 배열 구성
 * ============================================================
 *
 * poll()에 전달할 fd 배열을 구성합니다:
 *   [0..N-1] = 소켓 활성화 서비스의 listen fd들
 *   [N]      = self-pipe 읽기 끝 (시그널 알림용)
 *
 * poll()이란?
 *   여러 fd를 동시에 감시하는 시스템 콜.
 *   "이 fd들 중 하나라도 데이터가 있으면 알려줘"
 *
 *   select()의 후속:
 *     select(): FD_SET 비트맵 사용, fd 1024 제한
 *     poll():   배열 사용, 제한 없음
 *     epoll():  커널이 관리, 대규모에 효율적 (Linux 전용)
 *
 *   우리는 감시할 fd가 수십 개 이하이므로 poll()이 적당.
 */
int sa_build_poll_fds(struct pollfd *fds, int max_fds)
{
	extern service_t services[];
	extern int num_services;
	int nfds = 0;

	/* 소켓 활성화 서비스의 listen fd 추가 */
	for (int i = 0; i < num_services && nfds < max_fds - 1; i++) {
		if (services[i].socket_activated &&
		    services[i].listen_fd >= 0 &&
		    services[i].state == SVC_STOPPED) {
			/*
			 * STOPPED 상태인 서비스만 감시.
			 * 이미 RUNNING이면 소켓은 서비스가 직접 사용 중.
			 *
			 * 하지만! 소켓 활성화에서는 소켓을 서비스에 "전달"하므로
			 * listen_fd는 항상 init이 들고 있음.
			 * 실제로는 서비스 시작 후에도 감시를 계속해야 할 수 있지만,
			 * 여기서는 단순화: 서비스가 STOPPED일 때만 활성화 트리거.
			 */
			fds[nfds].fd = services[i].listen_fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}
	}

	/* self-pipe 읽기 끝 추가 */
	if (signal_pipe[0] >= 0 && nfds < max_fds) {
		fds[nfds].fd = signal_pipe[0];
		fds[nfds].events = POLLIN;
		fds[nfds].revents = 0;
		nfds++;
	}

	return nfds;
}

/* ============================================================
 * poll() 이벤트 처리
 * ============================================================
 *
 * poll()이 반환된 후 호출하여 각 fd의 이벤트를 처리.
 *
 * 처리 종류:
 *   1. 리스닝 소켓에 POLLIN → 클라이언트 연결 요청 있음
 *      → 해당 서비스 시작 (fd 전달)
 *   2. self-pipe에 POLLIN → 시그널 수신됨
 *      → pipe 비우기 (실제 시그널 처리는 main에서)
 */
int sa_handle_events(struct pollfd *fds, int nfds)
{
	int handled = 0;

	for (int i = 0; i < nfds; i++) {
		if (!(fds[i].revents & POLLIN))
			continue;

		/* self-pipe 이벤트: pipe 비우기 */
		if (fds[i].fd == signal_pipe[0]) {
			char buf[64];

			/*
			 * pipe에 쌓인 모든 바이트를 읽어서 비우기.
			 * 시그널이 여러 번 왔을 수 있으므로 루프.
			 * 논블로킹이므로 데이터가 없으면 바로 반환.
			 */
			while (read(signal_pipe[0], buf, sizeof(buf)) > 0)
				; /* 비우기 */
			handled++;
			continue;
		}

		/* 소켓 이벤트: 서비스 시작 */
		service_t *svc = svc_find_by_listen_fd(fds[i].fd);

		if (!svc)
			continue;

		if (svc->state == SVC_STOPPED) {
			LOG_INFO("Socket activation: connection on %s",
				 svc->socket_path);
			LOG_INFO("Starting service '%s' on demand...",
				 svc->name);

			/*
			 * 서비스 시작!
			 * svc_start()가 fork → execve 시
			 * listen_fd를 fd 3으로 dup2하여 전달.
			 */
			svc_start(svc->name);
			handled++;
		}
	}

	return handled;
}

/* ============================================================
 * 정리
 * ============================================================ */
void sa_cleanup(void)
{
	extern service_t services[];
	extern int num_services;

	/* 리스닝 소켓 닫기 */
	for (int i = 0; i < num_services; i++) {
		if (services[i].socket_activated &&
		    services[i].listen_fd >= 0) {
			close(services[i].listen_fd);
			unlink(services[i].socket_path);
			services[i].listen_fd = -1;
		}
	}

	/* self-pipe 닫기 */
	if (signal_pipe[0] >= 0) {
		close(signal_pipe[0]);
		signal_pipe[0] = -1;
	}
	if (signal_pipe[1] >= 0) {
		close(signal_pipe[1]);
		signal_pipe[1] = -1;
	}
}
