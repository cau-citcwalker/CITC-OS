/*
 * citcinit - CITC OS Init System (PID 1)
 * =======================================
 *
 * 이것은 CITC OS에서 커널이 가장 먼저 실행하는 프로그램입니다.
 * Linux 커널은 부팅을 마치면 /sbin/init (또는 커널 파라미터로 지정된 프로그램)을
 * PID 1로 실행합니다.
 *
 * PID 1의 책임:
 *   1. 가상 파일시스템 마운트 (/proc, /sys, /dev)
 *   2. 시스템 초기 설정 (호스트네임, 콘솔 등)
 *   3. 시스템 서비스 시작
 *   4. 고아 프로세스 회수 (좀비 프로세스 방지)
 *   5. 시스템 종료 처리
 *
 * v0.1: 파일시스템 마운트 + 쉘 실행
 * v0.2: 서비스 관리자 추가 (의존성 기반 서비스 시작)
 * v0.3: 실제 서비스 연결 (busybox syslogd, klogd)
 * v0.4: 설정 파일 기반 서비스 로드 (/etc/citc/services/)
 * v0.5: 네트워킹 지원 (DHCP, DNS)
 * v0.6: 소켓 활성화 + poll() 이벤트 루프 (Class 19)
 *
 * 빌드 방법 (WSL에서):
 *   gcc -static -o citcinit main.c
 *
 *   -static 옵션이 중요한 이유:
 *   initramfs에는 공유 라이브러리(.so)가 없을 수 있으므로,
 *   모든 라이브러리 코드를 실행파일에 포함시켜야 합니다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>   /* makedev() — 장치 번호 생성 매크로 */
#include <sys/wait.h>
#include <sys/reboot.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>

#include <poll.h>

#include "service.h"
#include "config.h"
#include "socket_activation.h"

/* ============================================================
 * 색상 출력 매크로
 * ============================================================
 * 터미널에서 색상을 사용하는 이유:
 * 부팅 메시지에서 성공/실패를 한눈에 구분하기 위해.
 * ANSI escape code를 사용합니다.
 */
#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_RESET  "\033[0m"
#define COLOR_BOLD   "\033[1m"

/* 로그 매크로 */
#define LOG_OK(fmt, ...) \
	printf(COLOR_GREEN "[  OK  ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_FAIL(fmt, ...) \
	printf(COLOR_RED   "[ FAIL ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
	printf(COLOR_BLUE  "[ INFO ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
	printf(COLOR_YELLOW "[ WARN ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)

/* ============================================================
 * 배너 출력
 * ============================================================ */
static void print_banner(void)
{
	printf("\n");
	printf(COLOR_BOLD COLOR_BLUE);
	printf("  +===================================+\n");
	printf("  |         CITC OS v0.5              |\n");
	printf("  |   Custom Init System (citcinit)   |\n");
	printf("  +===================================+\n");
	printf(COLOR_RESET);
	printf("\n");
}

/* ============================================================
 * 디렉토리 생성 헬퍼
 * ============================================================
 * mkdir()은 시스템 콜입니다. 커널에게 "이 경로에 디렉토리를 만들어줘"라고 요청합니다.
 *
 * 파라미터:
 *   path: 생성할 디렉토리 경로
 *   mode: 권한 (0755 = rwxr-xr-x)
 *         - 소유자: 읽기+쓰기+실행
 *         - 그룹:   읽기+실행
 *         - 기타:   읽기+실행
 */
static void ensure_dir(const char *path, mode_t mode)
{
	if (mkdir(path, mode) < 0 && errno != EEXIST) {
		LOG_WARN("mkdir failed: %s (%s)", path, strerror(errno));
	}
}

/* ============================================================
 * 파일시스템 마운트
 * ============================================================
 *
 * mount() 시스템 콜 설명:
 *   mount(source, target, filesystemtype, mountflags, data)
 *
 *   - source: 마운트할 장치 (가상 FS는 "none" 또는 이름)
 *   - target: 마운트 지점 (디렉토리)
 *   - filesystemtype: 파일시스템 타입 문자열
 *   - mountflags: 옵션 플래그
 *   - data: 파일시스템별 추가 옵션
 *
 * 가상 파일시스템이란?
 *   디스크에 있는 게 아니라, 커널이 동적으로 생성하는 파일시스템입니다.
 *
 *   /proc (procfs):
 *     - 실행 중인 프로세스 정보를 파일로 보여줌
 *     - /proc/1/status → PID 1의 상태
 *     - /proc/cpuinfo → CPU 정보
 *     - /proc/meminfo → 메모리 정보
 *     - ps, top 같은 명령어가 이걸 읽음
 *
 *   /sys (sysfs):
 *     - 하드웨어 장치 정보를 계층적으로 보여줌
 *     - /sys/class/net/ → 네트워크 인터페이스
 *     - /sys/block/ → 블록 디바이스 (디스크)
 *     - udev가 이걸 읽어서 /dev에 장치 파일을 생성
 *
 *   /dev (devtmpfs):
 *     - 장치 파일. 하드웨어를 파일처럼 접근할 수 있게 함
 *     - /dev/sda → 첫 번째 SATA 디스크
 *     - /dev/tty → 터미널
 *     - /dev/null → 블랙홀 (쓰면 사라짐)
 *     - /dev/random → 난수 생성기
 *
 *   /dev/pts (devpts):
 *     - 가상 터미널 (pseudo-terminal)
 *     - SSH, 터미널 에뮬레이터가 사용
 *
 *   /run (tmpfs):
 *     - 런타임 데이터 저장 (RAM에 있음, 재부팅하면 사라짐)
 *     - PID 파일, 소켓 파일 등
 */

/* 마운트할 가상 파일시스템 목록 */
struct mount_entry {
	const char *source;
	const char *target;
	const char *fstype;
	unsigned long flags;
	const char *data;
};

static const struct mount_entry early_mounts[] = {
	/* procfs: 프로세스 정보. 이게 없으면 ps, top이 동작 안 함 */
	{ "proc",    "/proc",    "proc",     0, NULL },

	/* sysfs: 하드웨어 정보. 이게 없으면 장치를 찾을 수 없음 */
	{ "sysfs",   "/sys",     "sysfs",    0, NULL },

	/* devtmpfs: 장치 파일. 커널이 자동으로 장치 노드를 생성 */
	{ "devtmpfs", "/dev",    "devtmpfs", 0, NULL },

	/* devpts: 가상 터미널. 쉘에서 필요함 */
	{ "devpts",  "/dev/pts", "devpts",   0, NULL },

	/* tmpfs: RAM 기반 임시 저장소 */
	{ "tmpfs",   "/run",     "tmpfs",    0, "mode=0755" },
	{ "tmpfs",   "/tmp",     "tmpfs",    0, "mode=1777" },
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static int mount_early_filesystems(void)
{
	int failed = 0;

	LOG_INFO("Mounting virtual filesystems...");

	for (size_t i = 0; i < ARRAY_SIZE(early_mounts); i++) {
		const struct mount_entry *m = &early_mounts[i];

		/* 마운트 포인트 디렉토리가 없으면 생성 */
		ensure_dir(m->target, 0755);

		if (mount(m->source, m->target, m->fstype,
			  m->flags, m->data) < 0) {
			/*
			 * EBUSY = 이미 마운트됨. 이건 에러가 아님.
			 * initramfs에서 이미 마운트했을 수 있음.
			 */
			if (errno == EBUSY) {
				LOG_OK("%-10s -> %s (already mounted)",
				       m->fstype, m->target);
			} else {
				LOG_FAIL("%-10s -> %s (%s)",
					 m->fstype, m->target,
					 strerror(errno));
				failed++;
			}
		} else {
			LOG_OK("%-10s -> %s", m->fstype, m->target);
		}
	}

	return failed;
}

/* ============================================================
 * /dev 기본 노드 생성
 * ============================================================
 *
 * mknod()는 특수 파일(장치 파일)을 생성하는 시스템 콜입니다.
 *
 * 장치 파일의 종류:
 *   - 캐릭터 디바이스 (S_IFCHR): 바이트 스트림 (터미널, 시리얼 포트)
 *   - 블록 디바이스 (S_IFBLK): 블록 단위 접근 (디스크, USB)
 *
 * major/minor 번호:
 *   - major: 드라이버를 식별 (예: 1 = mem, 5 = tty, 8 = sd)
 *   - minor: 같은 드라이버 내 장치를 식별 (예: sda=0, sdb=1)
 *
 * devtmpfs가 마운트되면 커널이 대부분 자동으로 만들어주지만,
 * 일부 기본 노드는 직접 만들어야 할 수 있습니다.
 */
static void create_dev_nodes(void)
{
	/* /dev/console: 시스템 콘솔. 부팅 메시지가 여기에 출력됨 */
	if (access("/dev/console", F_OK) != 0)
		mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));

	/* /dev/null: 블랙홀. 여기에 쓰면 사라짐, 읽으면 EOF */
	if (access("/dev/null", F_OK) != 0)
		mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));

	/* /dev/zero: 무한 0 생성기. 메모리 초기화에 사용 */
	if (access("/dev/zero", F_OK) != 0)
		mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));

	/* /dev/tty: 현재 프로세스의 제어 터미널 */
	if (access("/dev/tty", F_OK) != 0)
		mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));

	/*
	 * 심볼릭 링크:
	 * /dev/fd → /proc/self/fd
	 * 프로세스가 열린 파일 디스크립터에 접근할 수 있게 함.
	 * 쉘의 프로세스 대치(process substitution)에 필요.
	 */
	/* 반환값 무시 시 컴파일러 경고 방지 — 실패해도 치명적이지 않음 */
	if (symlink("/proc/self/fd", "/dev/fd") < 0)
		LOG_WARN("/dev/fd symlink failed: %s", strerror(errno));
	if (symlink("/proc/self/fd/0", "/dev/stdin") < 0)
		LOG_WARN("/dev/stdin symlink failed: %s", strerror(errno));
	if (symlink("/proc/self/fd/1", "/dev/stdout") < 0)
		LOG_WARN("/dev/stdout symlink failed: %s", strerror(errno));
	if (symlink("/proc/self/fd/2", "/dev/stderr") < 0)
		LOG_WARN("/dev/stderr symlink failed: %s", strerror(errno));
}

/* ============================================================
 * 호스트네임 설정
 * ============================================================
 * sethostname()은 시스템의 네트워크 이름을 설정합니다.
 * 쉘 프롬프트에 "user@hostname" 형태로 표시됩니다.
 */
static void set_hostname(void)
{
	const char *hostname = "citcos";

	if (sethostname(hostname, strlen(hostname)) < 0) {
		LOG_WARN("hostname set failed: %s", strerror(errno));
	} else {
		LOG_OK("Hostname: %s", hostname);
	}
}

/* ============================================================
 * 콘솔 설정 (기본)
 * ============================================================
 * 부팅 초기에 호출. /dev/console만 사용.
 * /dev가 아직 devtmpfs로 마운트되기 전이라
 * /dev/ttyS0가 없을 수 있으므로, 듀얼 출력은 나중에 설정.
 */
static void setup_console(void)
{
	int fd;

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
	}
}

/* ============================================================
 * 듀얼 출력 설정 (화면 + 시리얼)
 * ============================================================
 * devtmpfs 마운트 후에 호출해야 함!
 * /dev/ttyS0가 존재해야 시리얼 포트를 열 수 있음.
 *
 * 방식:
 *   pipe + fork된 tee 자식 프로세스
 *   → 모든 printf() 출력이 화면과 시리얼 양쪽에 동시 출력.
 *
 *   [citcinit]  stdout/stderr → pipe → [tee child] → /dev/console
 *                                                   → /dev/ttyS0
 */
static void setup_dual_output(void)
{
	int serial_fd = open("/dev/ttyS0", O_WRONLY | O_NOCTTY);
	if (serial_fd < 0)
		return;  /* 시리얼 없음 → 기존 콘솔만 유지 */

	int console_wr = open("/dev/console", O_WRONLY);
	if (console_wr < 0) {
		close(serial_fd);
		return;
	}

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		close(serial_fd);
		close(console_wr);
		return;
	}

	/* 버퍼된 데이터를 먼저 flush */
	fflush(stdout);
	fflush(stderr);

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		close(serial_fd);
		close(console_wr);
		return;
	}

	if (pid == 0) {
		/*
		 * Tee 자식 프로세스:
		 * pipe에서 읽어서 화면(console)과 시리얼(ttyS0) 양쪽에 출력.
		 * pipe가 닫히면 (부모 종료 시) EOF로 자연스럽게 종료.
		 */
		close(pipefd[1]);
		close(STDIN_FILENO);

		char buf[512];
		ssize_t n;
		while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
			(void)!write(console_wr, buf, n);
			(void)!write(serial_fd, buf, n);
		}

		close(pipefd[0]);
		close(console_wr);
		close(serial_fd);
		_exit(0);
	}

	/* 부모: stdout/stderr를 pipe로 전환 */
	close(pipefd[0]);
	close(serial_fd);
	close(console_wr);

	dup2(pipefd[1], STDOUT_FILENO);
	dup2(pipefd[1], STDERR_FILENO);
	if (pipefd[1] > STDERR_FILENO)
		close(pipefd[1]);

	/* pipe 통과 시 line-buffered로 설정 */
	setvbuf(stdout, NULL, _IOLBF, 0);
}

/* ============================================================
 * 고아 프로세스 회수 (Zombie Reaper)
 * ============================================================
 *
 * 이것은 PID 1의 가장 중요한 숨겨진 책임입니다.
 *
 * 배경 지식 - 프로세스 생명주기:
 *   1. 부모 프로세스가 fork()로 자식을 생성
 *   2. 자식이 작업을 수행
 *   3. 자식이 exit()으로 종료
 *   4. 부모가 wait()으로 자식의 종료 상태를 회수
 *
 * 문제: 만약 부모가 wait() 하기 전에 먼저 죽으면?
 *   → 자식은 "고아(orphan)"가 됨
 *   → 커널이 고아를 PID 1의 자식으로 입양시킴
 *   → PID 1이 wait()으로 회수해야 함
 *
 * 이걸 안 하면?
 *   → 좀비 프로세스가 쌓임 (프로세스 테이블 낭비)
 *   → 결국 새 프로세스를 생성할 수 없게 됨 → 시스템 마비
 *
 * SIGCHLD 시그널:
 *   자식 프로세스가 종료되면 커널이 부모에게 SIGCHLD를 보냄.
 *   PID 1은 이 시그널을 받으면 wait()을 호출해야 함.
 */
static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int sig)
{
	(void)sig;
	got_sigchld = 1;
	sa_signal_notify();  /* poll()을 깨우기 위해 self-pipe에 write */
}

static void reap_zombies(void)
{
	int status;
	pid_t pid;

	/*
	 * waitpid(-1, ..., WNOHANG):
	 *   -1: 아무 자식이나 기다림
	 *   WNOHANG: 기다리는 자식이 없으면 즉시 반환 (블로킹하지 않음)
	 *
	 * while 루프인 이유:
	 *   한 번에 여러 자식이 죽을 수 있으므로,
	 *   더 이상 회수할 자식이 없을 때까지 반복.
	 */
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (WIFEXITED(status)) {
			LOG_INFO("Child %d exited (code: %d)",
				 pid, WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			LOG_INFO("Child %d killed by signal %d",
				 pid, WTERMSIG(status));
		}

		/*
		 * 서비스 관리자에 알림.
		 * 이 PID가 등록된 서비스이면 상태 업데이트 + 자동 재시작.
		 * 서비스가 아니면 (쉘 등) 아무 일도 안 함.
		 */
		svc_notify_exit(pid, status);
	}
}

/* ============================================================
 * 쉘 실행
 * ============================================================
 *
 * fork() + execv() 패턴:
 *   이것은 Unix/Linux에서 새 프로그램을 실행하는 기본 패턴입니다.
 *
 *   fork():
 *     - 현재 프로세스를 복제하여 자식 프로세스를 생성
 *     - 부모에게는 자식의 PID를, 자식에게는 0을 반환
 *     - 이 시점에서 부모와 자식은 동일한 코드를 실행하고 있음
 *
 *   execv():
 *     - 현재 프로세스의 메모리를 새 프로그램으로 교체
 *     - 성공하면 돌아오지 않음 (새 프로그램이 실행되니까)
 *     - 실패하면 -1 반환
 *
 *   왜 fork() 후 exec()인가?
 *     → PID 1이 직접 exec()하면 PID 1이 쉘로 바뀌어버림
 *     → PID 1은 계속 살아있어야 하므로, 자식에서 쉘을 실행
 */
static pid_t spawn_shell(void)
{
	pid_t pid;

	/* 실행할 쉘 목록 (첫 번째 발견되는 것을 사용) */
	const char *shells[] = {
		"/bin/citcsh",    /* CITC OS 커스텀 쉘 */
		"/bin/sh",
		"/bin/bash",
		"/bin/ash",       /* busybox의 ash 쉘 */
		NULL,
	};

	pid = fork();

	if (pid < 0) {
		/* fork() 실패 - 이건 심각한 문제 */
		LOG_FAIL("fork() failed: %s", strerror(errno));
		return -1;
	}

	if (pid == 0) {
		/*
		 * 여기는 자식 프로세스.
		 * fork() 반환값이 0이면 자식입니다.
		 */

		/* 세션 리더가 됨 (새 세션 생성) */
		setsid();

		/* 사용 가능한 쉘을 찾아서 실행 */
		for (int i = 0; shells[i] != NULL; i++) {
			if (access(shells[i], X_OK) == 0) {
				LOG_INFO("Starting shell: %s", shells[i]);

				char *argv[] = { (char *)shells[i], NULL };
				char *envp[] = {
					"HOME=/root",
					"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
					"TERM=linux",
					"SHELL=/bin/sh",
					NULL,
				};

				execve(shells[i], argv, envp);

				/* execve가 반환되었으면 실패한 것 */
				LOG_FAIL("execve(%s) failed: %s",
					 shells[i], strerror(errno));
			}
		}

		/* 어떤 쉘도 실행 못 했으면 종료 */
		LOG_FAIL("No executable shell found!");
		_exit(1);
	}

	/*
	 * 여기는 부모 프로세스 (PID 1).
	 * fork() 반환값이 자식의 PID.
	 */
	return pid;
}

/* ============================================================
 * 시리얼 콘솔 쉘 실행
 * ============================================================
 * /dev/ttyS0에 별도의 쉘을 띄워서 WSL 터미널에서도
 * 명령어를 입력할 수 있게 합니다.
 *
 * 메인 쉘은 /dev/console (QEMU 화면)에 연결되고,
 * 이 쉘은 /dev/ttyS0 (시리얼 = WSL 터미널)에 연결됩니다.
 */
static pid_t spawn_serial_shell(void)
{
	int serial_fd;

	serial_fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY);
	if (serial_fd < 0)
		return -1;  /* 시리얼 포트 없음 */

	pid_t pid = fork();
	if (pid < 0) {
		close(serial_fd);
		return -1;
	}

	if (pid == 0) {
		/* 자식: 시리얼 쉘 */
		setsid();

		/* stdin/stdout/stderr를 /dev/ttyS0에 연결 */
		dup2(serial_fd, STDIN_FILENO);
		dup2(serial_fd, STDOUT_FILENO);
		dup2(serial_fd, STDERR_FILENO);
		if (serial_fd > STDERR_FILENO)
			close(serial_fd);

		const char *shells[] = {
			"/bin/sh", "/bin/ash", NULL,
		};

		for (int i = 0; shells[i] != NULL; i++) {
			if (access(shells[i], X_OK) == 0) {
				char *argv[] = { (char *)shells[i], NULL };
				char *envp[] = {
					"HOME=/root",
					"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
					"TERM=vt100",
					"SHELL=/bin/sh",
					NULL,
				};
				execve(shells[i], argv, envp);
			}
		}
		_exit(1);
	}

	close(serial_fd);
	return pid;
}

/* ============================================================
 * 시스템 종료 처리
 * ============================================================
 *
 * PID 1이 종료 시그널을 받으면:
 *   1. 모든 프로세스에 SIGTERM 전송 (정상 종료 요청)
 *   2. 잠시 대기 (프로세스들이 종료할 시간을 줌)
 *   3. 남은 프로세스에 SIGKILL 전송 (강제 종료)
 *   4. 파일시스템 동기화
 *   5. reboot() 시스템 콜로 재부팅 또는 전원 끄기
 */
static volatile sig_atomic_t shutdown_requested = 0;
static volatile unsigned int shutdown_cmd = LINUX_REBOOT_CMD_POWER_OFF;

static void sigterm_handler(int sig)
{
	(void)sig;
	shutdown_requested = 1;
	shutdown_cmd = LINUX_REBOOT_CMD_POWER_OFF;
	sa_signal_notify();
}

static void sigint_handler(int sig)
{
	(void)sig;
	shutdown_requested = 1;
	shutdown_cmd = LINUX_REBOOT_CMD_RESTART;
	sa_signal_notify();
}

/*
 * SIGUSR1: halt (시스템 정지)
 *
 * halt는 poweroff와 다릅니다:
 *   - poweroff: CPU 정지 + 전원 끄기 (ACPI power off)
 *   - halt: CPU 정지만. 전원은 켜져 있을 수 있음.
 *
 * 실제로 현대 시스템에서는 거의 같은 결과지만,
 * 전통적인 Unix에서는 구분했습니다.
 * (옛날 서버는 halt 후 수동으로 전원 스위치를 꺼야 했음)
 */
static void sigusr1_handler(int sig)
{
	(void)sig;
	shutdown_requested = 1;
	shutdown_cmd = LINUX_REBOOT_CMD_HALT;
	sa_signal_notify();
}

static void do_shutdown(void)
{
	const char *action;

	if (shutdown_cmd == LINUX_REBOOT_CMD_RESTART)
		action = "restarting";
	else if (shutdown_cmd == LINUX_REBOOT_CMD_HALT)
		action = "halting";
	else
		action = "shutting down";

	printf("\n");
	LOG_INFO("System %s...", action);

	/* 1. 서비스 관리자를 통해 서비스 먼저 정지 */
	svc_stop_all();

	/* 2. 나머지 모든 프로세스에 SIGTERM (정상 종료 요청) */
	LOG_INFO("Sending SIGTERM to all processes...");
	kill(-1, SIGTERM);

	/* 2. 3초 대기 */
	sleep(3);

	/* 3. 남은 프로세스에 SIGKILL (강제 종료) */
	LOG_INFO("Sending SIGKILL to remaining processes...");
	kill(-1, SIGKILL);

	/* 4. 파일시스템 동기화 (디스크에 데이터 기록) */
	LOG_INFO("Syncing filesystems...");
	sync();

	/* 5. 파일시스템 언마운트 */
	umount2("/tmp", MNT_DETACH);
	umount2("/run", MNT_DETACH);
	umount2("/dev/pts", MNT_DETACH);
	umount2("/dev", MNT_DETACH);
	umount2("/sys", MNT_DETACH);
	umount2("/proc", MNT_DETACH);

	/* 6. 재부팅 또는 전원 끄기 */
	LOG_OK("System %s!", action);
	reboot(shutdown_cmd);

	/* reboot()이 실패하면 여기에 도달 (이러면 안 됨) */
	LOG_FAIL("reboot() failed: %s", strerror(errno));
	for (;;)
		sleep(1);
}

/* ============================================================
 * 시그널 핸들러 등록
 * ============================================================
 *
 * sigaction()은 signal()보다 안전한 시그널 핸들러 등록 방법입니다.
 *
 * 시그널이란?
 *   프로세스에 보내는 소프트웨어 인터럽트.
 *   예: SIGTERM(종료 요청), SIGCHLD(자식 종료), SIGINT(Ctrl+C)
 *
 * SA_RESTART:
 *   시그널 핸들러가 실행된 후 중단된 시스템 콜을 자동으로 재시작.
 *   이게 없으면 read(), wait() 등이 EINTR로 실패할 수 있음.
 *
 * SA_NOCLDSTOP:
 *   자식이 일시정지(SIGSTOP)될 때는 SIGCHLD를 받지 않음.
 *   자식이 종료될 때만 받음.
 */
static void setup_signals(void)
{
	struct sigaction sa;

	/* SIGCHLD: 자식 프로세스 종료 → 좀비 회수 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	/* SIGTERM: 종료 요청 → 시스템 종료 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);

	/* SIGINT (Ctrl+C): 재부팅 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

	/* SIGUSR1: halt (시스템 정지) — shutdown -h 명령이 보냄 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigusr1_handler;
	sa.sa_flags = 0;
	sigaction(SIGUSR1, &sa, NULL);
}

/* ============================================================
 * 메인 함수
 * ============================================================
 *
 * PID 1의 메인 루프:
 *   1. 초기화 (마운트, 콘솔, 시그널)
 *   2. 쉘 실행
 *   3. 무한 루프에서 이벤트 처리 (좀비 회수, 종료 요청)
 *
 * 왜 무한 루프인가?
 *   PID 1은 절대 종료되면 안 됩니다.
 *   종료되면 커널이 panic합니다.
 *   그래서 계속 돌면서 시그널을 처리합니다.
 */
int main(int argc, char *argv[])
{
	pid_t shell_pid;

	(void)argc;
	(void)argv;

	/*
	 * PID 확인. 반드시 PID 1이어야 함.
	 * 테스트용으로 PID 1이 아닐 때도 허용하되, 경고 출력.
	 */
	if (getpid() != 1) {
		LOG_WARN("Not PID 1 (PID=%d). Running in test mode.",
			 getpid());
	}

	/* === 1단계: 콘솔 설정 (기본: /dev/console만) === */
	setup_console();

	/* === 2단계: 배너 출력 === */
	print_banner();

	LOG_INFO("citcinit starting (PID=%d)", getpid());

	/* === 3단계: 시그널 핸들러 등록 === */
	setup_signals();
	LOG_OK("Signal handlers registered");

	/* === 4단계: 가상 파일시스템 마운트 === */
	mount_early_filesystems();

	/* === 5단계: 장치 노드 생성 === */
	create_dev_nodes();
	LOG_OK("Device nodes created");

	/* === 5.5단계: 듀얼 출력 활성화 (화면 + 시리얼) === */
	/*
	 * devtmpfs 마운트 후에야 /dev/ttyS0가 존재함.
	 * 여기서 pipe+tee 방식으로 양쪽 출력을 설정.
	 * 이전 단계의 로그는 화면에만 보이지만,
	 * 이후 모든 로그는 화면 + 시리얼 양쪽에 출력됨.
	 */
	setup_dual_output();
	LOG_OK("Dual output enabled (console + serial)");

	/* === 6단계: 호스트네임 설정 === */
	set_hostname();

	/* === 7단계: 서비스 관리자 초기화 및 설정 파일 로드 === */
	/*
	 * v0.4: 설정 파일 기반 서비스 로드!
	 *
	 * 이전 버전(v0.3)에서는 서비스를 main.c에 하드코딩했지만,
	 * 이제 /etc/citc/services/ 디렉토리의 .conf 파일에서 읽습니다.
	 *
	 * 장점:
	 *   - 서비스 추가/수정 시 재컴파일 불필요
	 *   - 사용자가 직접 .conf 파일을 작성하여 서비스 추가 가능
	 *   - 패키지 매니저가 패키지 설치 시 .conf 파일을 함께 설치
	 *
	 * 작동 방식:
	 *   1. opendir("/etc/citc/services")로 디렉토리 열기
	 *   2. *.conf 파일을 찾아서 key=value 파싱
	 *   3. svc_register() + svc_add_arg() + svc_add_dependency() 호출
	 */
	svc_manager_init();
	config_load_services(SVC_CONFIG_DIR);

	/* === 7.5단계: 소켓 활성화 초기화 (Class 19) === */
	/*
	 * 서비스를 시작하기 전에 소켓을 먼저 생성!
	 *
	 * 왜 서비스보다 먼저?
	 *   소켓 활성화의 핵심: 소켓이 먼저 준비되어야
	 *   다른 서비스가 연결을 시도할 수 있음.
	 *
	 *   예: compositor의 소켓(/tmp/citc-display-0)을 먼저 생성
	 *       → citcshell이 연결 시도 → compositor가 자동 시작
	 *
	 * self-pipe도 여기서 생성:
	 *   메인 루프가 poll() 기반이므로, 시그널을 감지하려면
	 *   self-pipe가 필요. (시그널 핸들러에서 pipe에 write →
	 *   poll()이 깨어남)
	 */
	sa_create_signal_pipe();
	sa_init();

	/* 등록된 서비스를 의존성 순서대로 시작 */
	printf("\n");
	svc_start_all();
	svc_print_status();

	/* === 8단계: 쉘 실행 === */
	printf("\n");
	LOG_INFO("=== System initialization complete ===");
	printf("\n");

	shell_pid = spawn_shell();
	if (shell_pid < 0) {
		LOG_FAIL("Shell launch failed! Trying emergency shell...");
		/* 비상: execv로 직접 쉘 실행 (PID 1이 쉘이 됨) */
		char *argv_emergency[] = { "/bin/sh", NULL };
		execv("/bin/sh", argv_emergency);
		/* 이것도 실패하면... 할 수 있는 게 없음 */
		LOG_FAIL("Emergency shell also failed! System halted.");
		for (;;)
			sleep(1);
	}

	/* 시리얼 콘솔에도 쉘 실행 (WSL 터미널에서 명령어 입력 가능) */
	if (spawn_serial_shell() > 0)
		LOG_OK("Serial shell started on /dev/ttyS0");

	/* === 9단계: 메인 루프 (PID 1은 여기서 영원히 돌아감) === */
	/*
	 * v0.5 변경: pause() → poll() 기반 이벤트 루프 (Class 19)
	 *
	 * 이전 버전: pause() — 시그널만 처리 가능
	 * 새 버전:   poll()  — 시그널 + 소켓 이벤트 동시 처리
	 *
	 * poll()은 여러 fd를 동시에 감시:
	 *   - self-pipe: 시그널 핸들러가 write → poll() 깨어남
	 *   - 소켓 활성화 fd: 클라이언트 연결 → 서비스 시작
	 *
	 * 이것이 현대 init 시스템(systemd, s6)의 메인 루프 구조.
	 * 단순한 pause() 루프에서 이벤트 기반 루프로의 진화.
	 */
	LOG_INFO("Entering event loop (shell PID=%d)", shell_pid);

	for (;;) {
		struct pollfd fds[SVC_MAX_SERVICES + 1];
		int nfds;

		/* 종료 요청 확인 */
		if (shutdown_requested) {
			sa_cleanup();
			do_shutdown();
			/* do_shutdown은 절대 반환하지 않음 */
		}

		/* 좀비 프로세스 회수 */
		if (got_sigchld) {
			got_sigchld = 0;
			reap_zombies();
		}

		/*
		 * poll() fd 배열 구성:
		 *   소켓 활성화 listen fd들 + self-pipe read fd
		 *
		 * 매 루프마다 재구성하는 이유:
		 *   서비스가 시작되면 해당 소켓의 감시를 중단해야 하므로.
		 *   (STOPPED → RUNNING으로 변하면 목록에서 빠짐)
		 */
		nfds = sa_build_poll_fds(fds, SVC_MAX_SERVICES + 1);

		/*
		 * poll(fds, nfds, -1):
		 *   fds    = 감시할 fd 배열
		 *   nfds   = 배열 크기
		 *   -1     = 타임아웃 없음 (무한 대기)
		 *
		 * 반환:
		 *   > 0: 이벤트가 있는 fd 수
		 *   = 0: 타임아웃 (여기서는 불가)
		 *   < 0: 에러 (EINTR = 시그널에 의해 중단)
		 */
		if (poll(fds, nfds, -1) > 0) {
			/* 소켓 활성화 이벤트 + self-pipe 이벤트 처리 */
			sa_handle_events(fds, nfds);
		}
		/* poll()이 EINTR로 중단되어도 괜찮음 → 루프 상단에서 처리 */
	}

	/* 여기에 도달하면 안 됨 */
	return 0;
}
