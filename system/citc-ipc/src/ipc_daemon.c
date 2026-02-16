/*
 * ipc_daemon.c - CITC IPC 메시지 버스 데몬
 * ==========================================
 *
 * D-Bus의 핵심 개념을 교육적으로 구현한 IPC 데몬.
 *
 * 이 데몬이 하는 일:
 *   1. Unix domain socket에서 클라이언트 연결을 받음
 *   2. 클라이언트가 서비스 이름을 등록 (예: "display", "shell")
 *   3. 클라이언트 간 메시지를 라우팅 (이름 기반 전달)
 *   4. 브로드캐스트 메시지를 모든 클라이언트에 전달
 *
 * 아키텍처:
 *
 *   +-------------------+
 *   |   citc-ipc daemon |
 *   |  /run/citc-ipc    |
 *   +--------+----------+
 *            |
 *    +-------+-------+--------+
 *    |       |       |        |
 *   [compositor] [shell] [pkgmgr]
 *    "display"   "shell"  "pkgmgr"
 *
 * poll() 기반 이벤트 루프:
 *   dbus-daemon과 동일한 구조.
 *   listen socket + 모든 클라이언트 fd를 poll()로 감시.
 *   새 연결 → accept, 메시지 도착 → 읽고 라우팅, 연결 종료 → 정리.
 *
 * 빌드:
 *   gcc -static -Wall -Werror -o citc-ipc ipc_daemon.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ipc_proto.h"

/* ============================================================
 * 상수
 * ============================================================ */
#define MAX_CLIENTS   32   /* 최대 동시 연결 클라이언트 수 */

/* ============================================================
 * 클라이언트 구조체
 * ============================================================
 *
 * 연결된 각 클라이언트의 정보.
 * name이 비어있으면 아직 REGISTER하지 않은 익명 클라이언트.
 */
struct ipc_client {
	int fd;                       /* 소켓 fd (-1이면 빈 슬롯) */
	uint32_t id;                  /* 클라이언트 고유 ID */
	char name[IPC_NAME_MAX];     /* 등록된 서비스 이름 */
};

/* ============================================================
 * 전역 상태
 * ============================================================ */
static struct ipc_client clients[MAX_CLIENTS];
static int listen_fd = -1;
static int running = 1;
static uint32_t next_client_id = 1;

/* ============================================================
 * 유틸리티: 완전한 쓰기/읽기
 * ============================================================
 *
 * write()와 read()는 요청한 바이트보다 적게 처리할 수 있음.
 * 특히 논블로킹 소켓에서 버퍼가 가득 찼을 때 발생.
 * 이 헬퍼는 전체 바이트가 처리될 때까지 반복.
 */
static ssize_t write_all(int fd, const void *buf, size_t len)
{
	size_t written = 0;

	while (written < len) {
		ssize_t n = write(fd, (const char *)buf + written,
				  len - written);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		written += n;
	}
	return (ssize_t)written;
}

static ssize_t read_all(int fd, void *buf, size_t len)
{
	size_t total = 0;

	while (total < len) {
		ssize_t n = read(fd, (char *)buf + total,
				 len - total);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (n == 0)
			return -1; /* 연결 종료 */
		total += n;
	}
	return (ssize_t)total;
}

/* ============================================================
 * 유틸리티: 메시지 전송
 * ============================================================ */
static int send_msg(int fd, uint32_t type, uint32_t serial,
		    const void *payload, uint32_t payload_len)
{
	struct ipc_header hdr;

	hdr.type = type;
	hdr.length = payload_len;
	hdr.serial = serial;

	/* 헤더 전송 */
	if (write_all(fd, &hdr, sizeof(hdr)) < 0)
		return -1;

	/* 페이로드 전송 */
	if (payload_len > 0 && payload) {
		if (write_all(fd, payload, payload_len) < 0)
			return -1;
	}

	return 0;
}

/* ============================================================
 * 클라이언트 관리
 * ============================================================ */

/* 빈 슬롯 찾기 */
static struct ipc_client *find_free_slot(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd < 0)
			return &clients[i];
	}
	return NULL;
}

/* 이름으로 클라이언트 찾기 */
static struct ipc_client *find_by_name(const char *name)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0 &&
		    strcmp(clients[i].name, name) == 0)
			return &clients[i];
	}
	return NULL;
}

/* 클라이언트 연결 해제 */
static void disconnect_client(struct ipc_client *c)
{
	if (c->fd < 0)
		return;

	printf("[IPC] Client disconnected: id=%u name='%s'\n",
	       c->id, c->name[0] ? c->name : "(anonymous)");

	close(c->fd);
	c->fd = -1;
	c->name[0] = '\0';
	c->id = 0;
}

/* ============================================================
 * 새 클라이언트 수락
 * ============================================================ */
static void accept_client(void)
{
	int fd = accept(listen_fd, NULL, NULL);

	if (fd < 0)
		return;

	struct ipc_client *c = find_free_slot();

	if (!c) {
		printf("[IPC] Max clients reached, rejecting\n");
		close(fd);
		return;
	}

	/* 논블로킹 설정 */
	fcntl(fd, F_SETFL, O_NONBLOCK);

	c->fd = fd;
	c->id = next_client_id++;
	c->name[0] = '\0';

	printf("[IPC] New client: id=%u fd=%d\n", c->id, fd);

	/* WELCOME 메시지 전송 */
	struct ipc_welcome welcome;
	welcome.client_id = c->id;
	send_msg(fd, IPC_MSG_WELCOME, 0, &welcome, sizeof(welcome));
}

/* ============================================================
 * 메시지 처리
 * ============================================================
 *
 * 클라이언트로부터 메시지를 읽고 처리.
 *
 * 프로토콜:
 *   1. 헤더(12B) 읽기 → type, length, serial
 *   2. 페이로드(length B) 읽기
 *   3. type에 따라 처리
 */
static void handle_client_message(struct ipc_client *c)
{
	struct ipc_header hdr;
	ssize_t n;

	/* 헤더 읽기 (부분 읽기 대응) */
	n = read_all(c->fd, &hdr, sizeof(hdr));
	if (n < 0) {
		disconnect_client(c);
		return;
	}

	/* 페이로드 크기 검증 */
	if (hdr.length > sizeof(struct ipc_send)) {
		printf("[IPC] Oversized payload from client %u\n", c->id);
		disconnect_client(c);
		return;
	}

	/* 페이로드 읽기 */
	uint8_t payload[sizeof(struct ipc_send)];

	if (hdr.length > 0) {
		if (read_all(c->fd, payload, hdr.length) < 0) {
			disconnect_client(c);
			return;
		}
	}

	/* 메시지 타입별 처리 */
	switch (hdr.type) {
	case IPC_MSG_REGISTER: {
		/*
		 * 이름 등록
		 *
		 * D-Bus에서: RequestName("org.freedesktop.NetworkManager")
		 * 여기서:    REGISTER("network")
		 *
		 * 이름 충돌 시 거부.
		 * D-Bus는 큐잉, 교체 등 복잡한 정책이 있지만
		 * 여기서는 단순히 거부.
		 */
		struct ipc_register *reg = (struct ipc_register *)payload;

		reg->name[IPC_NAME_MAX - 1] = '\0';  /* 안전 종료 */

		if (find_by_name(reg->name)) {
			printf("[IPC] Name '%s' already taken\n", reg->name);
			struct ipc_error err = {
				.code = 1,
			};
			snprintf(err.message, sizeof(err.message),
				 "Name '%.63s' already registered", reg->name);
			send_msg(c->fd, IPC_MSG_ERROR, hdr.serial,
				 &err, sizeof(err));
			break;
		}

		snprintf(c->name, sizeof(c->name), "%.63s", reg->name);
		printf("[IPC] Client %u registered as '%s'\n",
		       c->id, c->name);
		break;
	}

	case IPC_MSG_SEND: {
		/*
		 * 메시지 라우팅
		 *
		 * D-Bus에서: method_call destination=org.foo.Bar member=DoSomething
		 * 여기서:    SEND destination="display" method="get_resolution"
		 *
		 * 대상 서비스 이름으로 클라이언트를 찾아서 전달.
		 * 대상이 없으면 에러 반환.
		 */
		struct ipc_send *msg = (struct ipc_send *)payload;

		msg->destination[IPC_NAME_MAX - 1] = '\0';
		msg->method[IPC_NAME_MAX - 1] = '\0';

		struct ipc_client *target = find_by_name(msg->destination);

		if (!target) {
			struct ipc_error err = {
				.code = 2,
			};
			snprintf(err.message, sizeof(err.message),
				 "Service '%.63s' not found", msg->destination);
			send_msg(c->fd, IPC_MSG_ERROR, hdr.serial,
				 &err, sizeof(err));
			break;
		}

		/* DELIVER 메시지 구성 */
		struct ipc_deliver deliver;

		snprintf(deliver.sender, sizeof(deliver.sender),
			 "%.63s", c->name[0] ? c->name : "(anonymous)");
		snprintf(deliver.method, sizeof(deliver.method),
			 "%.63s", msg->method);
		deliver.data_len = msg->data_len;
		if (msg->data_len > 0 && msg->data_len <= IPC_DATA_MAX)
			memcpy(deliver.data, msg->data, msg->data_len);

		send_msg(target->fd, IPC_MSG_DELIVER, hdr.serial,
			 &deliver, sizeof(deliver));

		printf("[IPC] Route: '%s' -> '%s' method='%s'\n",
		       c->name, msg->destination, msg->method);
		break;
	}

	case IPC_MSG_BROADCAST: {
		/*
		 * 브로드캐스트 (시그널)
		 *
		 * D-Bus에서: signal path=/org/foo interface=org.foo member=Changed
		 * 여기서:    BROADCAST signal="package-installed"
		 *
		 * 발신자를 제외한 모든 연결된 클라이언트에게 전달.
		 * pub/sub 패턴의 단순화된 구현.
		 */
		struct ipc_broadcast *bc = (struct ipc_broadcast *)payload;

		/* 발신자 이름 설정 */
		snprintf(bc->sender, sizeof(bc->sender),
			 "%s", c->name[0] ? c->name : "(anonymous)");

		printf("[IPC] Broadcast from '%s': signal='%s'\n",
		       bc->sender, bc->signal_name);

		/* 모든 클라이언트에 전달 (발신자 제외) */
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0 && clients[i].fd != c->fd) {
				send_msg(clients[i].fd, IPC_MSG_SIGNAL,
					 hdr.serial, bc, sizeof(*bc));
			}
		}
		break;
	}

	default:
		printf("[IPC] Unknown message type %u from client %u\n",
		       hdr.type, c->id);
		break;
	}
}

/* ============================================================
 * 시그널 핸들러
 * ============================================================ */
static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* ============================================================
 * 서버 초기화
 * ============================================================
 *
 * LISTEN_FDS 프로토콜 지원:
 *   소켓 활성화로 시작된 경우 fd 3을 사용.
 *   아니면 직접 소켓 생성.
 */
static int server_init(void)
{
	/*
	 * 소켓 활성화 감지 (Class 19 연동)
	 */
	char *lfds = getenv("LISTEN_FDS");

	if (lfds && atoi(lfds) > 0) {
		listen_fd = 3;
		fcntl(listen_fd, F_SETFL, O_NONBLOCK);
		printf("[IPC] Socket activation (fd=3)\n");
		return 0;
	}

	/* 직접 소켓 생성 */
	struct sockaddr_un addr;

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return -1;
	}

	unlink(CITC_IPC_SOCKET);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CITC_IPC_SOCKET,
		sizeof(addr.sun_path) - 1);

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 8) < 0) {
		perror("listen");
		close(listen_fd);
		return -1;
	}

	fcntl(listen_fd, F_SETFL, O_NONBLOCK);
	printf("[IPC] Listening on %s\n", CITC_IPC_SOCKET);
	return 0;
}

/* ============================================================
 * 메인 이벤트 루프
 * ============================================================
 *
 * poll() 기반:
 *   fds[0]     = listen socket (새 연결 감지)
 *   fds[1..N]  = 클라이언트 소켓 (메시지 감지)
 *
 * dbus-daemon의 메인 루프와 동일한 구조.
 */
int main(void)
{
	struct pollfd fds[MAX_CLIENTS + 1];
	int nfds;

	printf("=== CITC IPC Daemon ===\n");

	/* 클라이언트 슬롯 초기화 */
	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	/* 시그널 핸들러 */
	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGPIPE, SIG_IGN);  /* 연결 끊긴 클라이언트에 write 시 */

	/* 서버 초기화 */
	if (server_init() < 0) {
		fprintf(stderr, "[IPC] Server init failed\n");
		return 1;
	}

	printf("[IPC] Ready. Waiting for clients...\n");

	/* 이벤트 루프 */
	while (running) {
		nfds = 0;

		/* listen socket */
		fds[nfds].fd = listen_fd;
		fds[nfds].events = POLLIN;
		fds[nfds].revents = 0;
		nfds++;

		/* 클라이언트 소켓 */
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				fds[nfds].fd = clients[i].fd;
				fds[nfds].events = POLLIN;
				fds[nfds].revents = 0;
				nfds++;
			}
		}

		/* poll: 이벤트 대기 */
		int ret = poll(fds, nfds, 1000); /* 1초 타임아웃 */

		if (ret < 0) {
			if (errno == EINTR)
				continue;  /* 시그널에 의한 중단 → 재시도 */
			perror("poll");
			break;
		}

		if (ret == 0)
			continue;  /* 타임아웃 → 다시 루프 */

		/* 이벤트 처리 */
		for (int i = 0; i < nfds; i++) {
			if (!(fds[i].revents & POLLIN))
				continue;

			if (fds[i].fd == listen_fd) {
				/* 새 클라이언트 연결 */
				accept_client();
			} else {
				/* 클라이언트 메시지 */
				for (int j = 0; j < MAX_CLIENTS; j++) {
					if (clients[j].fd == fds[i].fd) {
						handle_client_message(&clients[j]);
						break;
					}
				}
			}
		}
	}

	/* 정리 */
	printf("[IPC] Shutting down...\n");
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0)
			disconnect_client(&clients[i]);
	}
	close(listen_fd);
	unlink(CITC_IPC_SOCKET);

	return 0;
}
