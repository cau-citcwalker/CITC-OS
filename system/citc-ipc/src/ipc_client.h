/*
 * ipc_client.h - CITC IPC 클라이언트 라이브러리 (header-only)
 * =============================================================
 *
 * IPC 데몬에 연결하여 메시지를 보내고 받는 클라이언트 라이브러리.
 * cdp_client.h와 동일한 "header-only" 패턴.
 *
 * header-only 라이브러리란?
 *   .c 파일 없이 .h 파일만으로 구성된 라이브러리.
 *   #include만 하면 바로 사용 가능 — 링크 설정 불필요.
 *   모든 함수를 static으로 선언하여 링커 충돌 방지.
 *
 *   장점: 빌드 단순화, 배포 편리
 *   단점: 여러 .c에서 include하면 코드 중복 (바이너리 크기 증가)
 *   적합: 소규모 유틸리티 라이브러리
 *
 * 사용법:
 *
 *   #include "ipc_client.h"
 *
 *   // 연결
 *   struct ipc_conn *ipc = ipc_connect();
 *   if (!ipc) { error... }
 *
 *   // 이름 등록
 *   ipc_register(ipc, "shell");
 *
 *   // 메시지 전송
 *   ipc_send(ipc, "display", "get_resolution", NULL, 0);
 *
 *   // 브로드캐스트
 *   ipc_broadcast(ipc, "status-changed", NULL, 0);
 *
 *   // 수신 처리 (poll 기반)
 *   ipc_dispatch(ipc);
 *
 *   // 연결 종료
 *   ipc_disconnect(ipc);
 */

#ifndef CITC_IPC_CLIENT_H
#define CITC_IPC_CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ipc_proto.h"

/* ============================================================
 * 클라이언트 연결 구조체
 * ============================================================ */
struct ipc_conn {
	int fd;                       /* 서버와의 소켓 fd */
	uint32_t client_id;           /* 서버가 할당한 ID */
	uint32_t next_serial;         /* 다음 메시지 일련번호 */
	char name[IPC_NAME_MAX];     /* 등록한 이름 */
};

/* ============================================================
 * 연결
 * ============================================================ */
static struct ipc_conn *ipc_connect(void)
{
	struct sockaddr_un addr;
	struct ipc_conn *conn;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CITC_IPC_SOCKET,
		sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}

	conn = calloc(1, sizeof(*conn));
	if (!conn) {
		close(fd);
		return NULL;
	}

	conn->fd = fd;
	conn->next_serial = 1;

	/*
	 * WELCOME 메시지 수신 대기.
	 * 서버가 연결 직후 WELCOME을 보내므로 바로 읽을 수 있음.
	 *
	 * poll()로 타임아웃 설정: 서버가 응답하지 않으면
	 * 무한 대기하지 않고 2초 후 진행 (ID 없이 사용).
	 */
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
		struct ipc_header hdr;

		if (read(fd, &hdr, sizeof(hdr)) == sizeof(hdr) &&
		    hdr.type == IPC_MSG_WELCOME &&
		    hdr.length == sizeof(struct ipc_welcome)) {
			struct ipc_welcome welcome;

			if (read(fd, &welcome, sizeof(welcome)) ==
			    sizeof(welcome))
				conn->client_id = welcome.client_id;
		}
	}

	/* 논블로킹으로 전환 (이후 dispatch에서 사용) */
	fcntl(fd, F_SETFL, O_NONBLOCK);

	return conn;
}

/* ============================================================
 * 연결 해제
 * ============================================================ */
static void ipc_disconnect(struct ipc_conn *conn)
{
	if (!conn)
		return;

	if (conn->fd >= 0)
		close(conn->fd);
	free(conn);
}

/* ============================================================
 * 내부: 메시지 전송
 * ============================================================ */
static int ipc_send_raw(struct ipc_conn *conn, uint32_t type,
			const void *payload, uint32_t len)
{
	struct ipc_header hdr;

	hdr.type = type;
	hdr.length = len;
	hdr.serial = conn->next_serial++;

	if (write(conn->fd, &hdr, sizeof(hdr)) != sizeof(hdr))
		return -1;
	if (len > 0 && payload) {
		if (write(conn->fd, payload, len) != (ssize_t)len)
			return -1;
	}
	return 0;
}

/* ============================================================
 * 이름 등록
 * ============================================================ */
static int ipc_register(struct ipc_conn *conn, const char *name)
{
	struct ipc_register reg;

	if (!conn || !name)
		return -1;

	memset(&reg, 0, sizeof(reg));
	snprintf(reg.name, sizeof(reg.name), "%s", name);
	snprintf(conn->name, sizeof(conn->name), "%s", name);

	return ipc_send_raw(conn, IPC_MSG_REGISTER, &reg, sizeof(reg));
}

/* ============================================================
 * 특정 서비스에 메시지 전송
 * ============================================================ */
static int ipc_send(struct ipc_conn *conn, const char *dest,
		    const char *method, const void *data, uint32_t data_len)
{
	struct ipc_send msg;

	if (!conn || !dest || !method)
		return -1;

	memset(&msg, 0, sizeof(msg));
	snprintf(msg.destination, sizeof(msg.destination), "%s", dest);
	snprintf(msg.method, sizeof(msg.method), "%s", method);
	msg.data_len = data_len;
	if (data && data_len > 0 && data_len <= IPC_DATA_MAX)
		memcpy(msg.data, data, data_len);

	return ipc_send_raw(conn, IPC_MSG_SEND, &msg, sizeof(msg));
}

/* ============================================================
 * 브로드캐스트
 * ============================================================ */
static int ipc_broadcast(struct ipc_conn *conn, const char *signal_name,
			 const void *data, uint32_t data_len)
{
	struct ipc_broadcast bc;

	if (!conn || !signal_name)
		return -1;

	memset(&bc, 0, sizeof(bc));
	snprintf(bc.signal_name, sizeof(bc.signal_name),
		 "%s", signal_name);
	bc.data_len = data_len;
	if (data && data_len > 0 && data_len <= IPC_DATA_MAX)
		memcpy(bc.data, data, data_len);

	return ipc_send_raw(conn, IPC_MSG_BROADCAST, &bc, sizeof(bc));
}

/* ============================================================
 * 수신 메시지 처리 (논블로킹)
 * ============================================================
 *
 * 콜백 없는 단순 버전:
 * 수신된 메시지의 타입과 내용을 반환.
 * 메시지가 없으면 0 반환.
 *
 * 반환: > 0 메시지 타입, 0 없음, < 0 에러
 */
static int ipc_dispatch(struct ipc_conn *conn, struct ipc_header *out_hdr,
			void *out_payload, uint32_t max_payload)
{
	struct ipc_header hdr;
	ssize_t n;

	if (!conn)
		return -1;

	n = read(conn->fd, &hdr, sizeof(hdr));
	if (n <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;  /* 메시지 없음 */
		return -1;  /* 에러 또는 연결 종료 */
	}
	if (n != sizeof(hdr))
		return -1;

	/* 페이로드 읽기 */
	if (hdr.length > 0 && hdr.length <= max_payload) {
		n = read(conn->fd, out_payload, hdr.length);
		if (n != (ssize_t)hdr.length)
			return -1;
	}

	if (out_hdr)
		*out_hdr = hdr;

	return (int)hdr.type;
}

/* ============================================================
 * 소켓 fd 반환 (poll에서 사용)
 * ============================================================ */
static int ipc_get_fd(struct ipc_conn *conn)
{
	return conn ? conn->fd : -1;
}

#endif /* CITC_IPC_CLIENT_H */
