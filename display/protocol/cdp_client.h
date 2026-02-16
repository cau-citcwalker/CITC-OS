/*
 * CITC Display Protocol — 클라이언트 라이브러리
 * =============================================
 *
 * 이 헤더를 include하면 CDP 컴포지터에 연결하여
 * 윈도우를 만들고 그릴 수 있습니다.
 *
 * Wayland 대응:
 *   이 파일 전체              ↔  libwayland-client
 *   cdp_connect()             ↔  wl_display_connect()
 *   cdp_create_surface()      ↔  wl_compositor_create_surface() + xdg_toplevel
 *   cdp_attach_buffer()       ↔  wl_surface_attach(wl_buffer from wl_shm_pool)
 *   cdp_commit()              ↔  wl_surface_commit()
 *   cdp_request_frame()       ↔  wl_surface.frame() → wl_callback
 *   cdp_dispatch()            ↔  wl_display_dispatch()
 *
 * 사용법:
 *   #include "cdp_client.h"
 *
 *   struct cdp_conn *conn = cdp_connect();
 *   struct cdp_window *win = cdp_create_surface(conn, 300, 200, "My App");
 *
 *   // 직접 픽셀 그리기 (XRGB8888)
 *   uint32_t *px = win->pixels;
 *   px[y * win->width + x] = 0x00FF0000;  // 빨강
 *
 *   cdp_commit(win);
 *
 *   while (cdp_dispatch(conn) >= 0) {
 *       // 이벤트 콜백에서 처리
 *   }
 *
 * header-only 라이브러리:
 *   모든 함수가 static으로 선언되어 있어서
 *   .c 파일 하나에 #include하면 바로 사용 가능.
 *   별도 링킹 불필요! (교육용 프로젝트에 적합)
 */

#ifndef CDP_CLIENT_H
#define CDP_CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>    /* syscall(SYS_memfd_create) */

#include "cdp_proto.h"

/* ============================================================
 * memfd_create 래퍼
 * ============================================================
 *
 * memfd_create()란?
 *   이름 없는(anonymous) 공유메모리 파일을 만드는 Linux 시스템콜.
 *   /tmp에 파일을 만드는 것과 비슷하지만:
 *     - 파일시스템에 이름이 없음 (다른 프로세스가 경로로 접근 불가)
 *     - fd가 닫히면 자동 정리 (파일 삭제 걱정 없음)
 *     - 메모리에만 존재 (디스크 I/O 없음)
 *
 * 왜 직접 syscall()을 사용하는가?
 *   musl-libc의 일부 버전에서는 memfd_create() 래퍼가 없을 수 있음.
 *   syscall()을 직접 호출하면 어떤 libc에서도 작동!
 *   Linux 3.17+ 커널이면 OK (우리는 6.8 사용).
 */
#ifndef SYS_memfd_create
#define SYS_memfd_create 319  /* x86_64 */
#endif

static int cdp_memfd_create(const char *name)
{
	return (int)syscall(SYS_memfd_create, name, 0);
}

/* ============================================================
 * 데이터 구조
 * ============================================================ */

#define CDP_MAX_WINDOWS  4

/*
 * CDP 윈도우 (= Wayland surface + buffer)
 *
 * 클라이언트가 만든 하나의 윈도우.
 * pixels 포인터를 통해 직접 그릴 수 있음.
 */
struct cdp_window {
	uint32_t surface_id;      /* 서버가 부여한 surface ID */
	int shm_fd;               /* memfd (공유메모리 fd) */
	uint32_t *pixels;         /* mmap된 픽셀 버퍼 (XRGB8888) */
	uint32_t width, height;   /* 크기 (픽셀) */
	uint32_t stride;          /* 한 줄 바이트 수 */
	uint32_t shm_size;        /* 전체 메모리 크기 */
	int active;               /* 1=활성, 0=해제됨 */
};

/*
 * CDP 연결 (= Wayland display)
 *
 * 컴포지터와의 소켓 연결.
 * 이벤트 콜백을 등록하여 입력을 받음.
 */
struct cdp_conn {
	int sock_fd;              /* 서버 소켓 */
	uint32_t screen_width;    /* 화면 너비 */
	uint32_t screen_height;   /* 화면 높이 */

	struct cdp_window windows[CDP_MAX_WINDOWS];
	int num_windows;

	/* 이벤트 콜백 — 앱이 설정 */
	void (*on_key)(uint32_t keycode, uint32_t state, char ch);
	void (*on_pointer_motion)(uint32_t surface_id, int x, int y);
	void (*on_pointer_button)(uint32_t surface_id, uint32_t button,
				  uint32_t state);
	void (*on_pointer_enter)(uint32_t surface_id, int x, int y);
	void (*on_pointer_leave)(uint32_t surface_id);
	void (*on_frame)(uint32_t surface_id);
	void (*on_focus_in)(uint32_t surface_id);
	void (*on_focus_out)(uint32_t surface_id);
};

/* ============================================================
 * 연결 / 해제
 * ============================================================ */

/*
 * 컴포지터에 연결
 *
 * Wayland 대응: wl_display_connect(NULL)
 *   → $WAYLAND_DISPLAY 또는 "wayland-0" 소켓에 연결
 *
 * 과정:
 *   1. Unix domain socket 생성
 *   2. /tmp/citc-display-0에 connect()
 *   3. 서버의 WELCOME 메시지 수신 (화면 크기 등)
 *
 * 반환: 연결 구조체 (실패 시 NULL)
 */
static struct cdp_conn *cdp_connect(void)
{
	struct cdp_conn *conn;
	struct sockaddr_un addr;
	int sock;

	conn = (struct cdp_conn *)calloc(1, sizeof(*conn));
	if (!conn) {
		fprintf(stderr, "CDP: 메모리 할당 실패\n");
		return NULL;
	}
	conn->sock_fd = -1;

	/* Unix domain socket 생성 */
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		fprintf(stderr, "CDP: 소켓 생성 실패: %s\n", strerror(errno));
		free(conn);
		return NULL;
	}

	/* 서버에 연결 */
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CDP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

	/*
	 * connect() 재시도 (서버가 아직 준비 안 됐을 수 있음)
	 *
	 * 실제 Wayland 클라이언트도 서버가 소켓을 만들 때까지
	 * 대기하거나 에러를 반환합니다.
	 */
	int retries = 5;

	while (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (--retries <= 0) {
			fprintf(stderr, "CDP: 연결 실패: %s\n",
				strerror(errno));
			fprintf(stderr, "     compositor가 실행 중인지 확인하세요\n");
			close(sock);
			free(conn);
			return NULL;
		}
		fprintf(stderr, "CDP: 연결 재시도... (%d)\n", retries);
		usleep(500000);  /* 0.5초 대기 */
	}

	conn->sock_fd = sock;

	/* WELCOME 메시지 수신 */
	uint32_t type, size;
	struct cdp_welcome welcome;

	if (cdp_recv_msg(sock, &type, &welcome, sizeof(welcome), &size) < 0 ||
	    type != CDP_EVT_WELCOME) {
		fprintf(stderr, "CDP: WELCOME 메시지 수신 실패\n");
		close(sock);
		free(conn);
		return NULL;
	}

	conn->screen_width = welcome.screen_width;
	conn->screen_height = welcome.screen_height;

	printf("CDP: 연결 성공! (화면: %ux%u, 프로토콜 v%u)\n",
	       welcome.screen_width, welcome.screen_height,
	       welcome.version);

	return conn;
}

/*
 * 연결 해제
 * Wayland: wl_display_disconnect()
 */
static void cdp_disconnect(struct cdp_conn *conn)
{
	if (!conn)
		return;

	for (int i = 0; i < conn->num_windows; i++) {
		struct cdp_window *w = &conn->windows[i];

		if (w->active) {
			if (w->pixels)
				munmap(w->pixels, w->shm_size);
			if (w->shm_fd >= 0)
				close(w->shm_fd);
		}
	}

	if (conn->sock_fd >= 0)
		close(conn->sock_fd);

	free(conn);
}

/* ============================================================
 * Surface(윈도우) 생성
 * ============================================================ */

/*
 * 새 surface 생성 + 공유메모리 버퍼 설정
 *
 * 이 함수 하나로 Wayland의 여러 단계를 합침:
 *   1. wl_compositor_create_surface()  → surface 생성
 *   2. xdg_surface + xdg_toplevel     → 윈도우 역할 부여
 *   3. memfd_create + wl_shm_pool     → 공유메모리 생성
 *   4. wl_shm_pool_create_buffer      → 버퍼 생성
 *   5. wl_surface_attach              → 버퍼 연결
 *   6. xdg_toplevel_set_title         → 제목 설정
 *
 * 반환: 윈도우 포인터 (실패 시 NULL)
 */
static struct cdp_window *cdp_create_surface(struct cdp_conn *conn,
					     int width, int height,
					     const char *title)
{
	if (!conn || conn->num_windows >= CDP_MAX_WINDOWS)
		return NULL;

	struct cdp_window *win = &conn->windows[conn->num_windows];

	memset(win, 0, sizeof(*win));
	win->width = (uint32_t)width;
	win->height = (uint32_t)height;
	win->stride = (uint32_t)width * 4;  /* XRGB8888: 4 bytes per pixel */
	win->shm_size = win->stride * win->height;
	win->shm_fd = -1;

	/* 1. CREATE_SURFACE 요청 */
	struct cdp_create_surface req;

	req.x = 100 + conn->num_windows * 50;   /* 기본 위치 (겹치지 않게) */
	req.y = 100 + conn->num_windows * 30;
	req.width = width;
	req.height = height;

	if (cdp_send_msg(conn->sock_fd, CDP_REQ_CREATE_SURFACE,
			 &req, sizeof(req)) < 0) {
		fprintf(stderr, "CDP: CREATE_SURFACE 전송 실패\n");
		return NULL;
	}

	/* 서버 응답 대기 (SURFACE_ID) */
	uint32_t type, size;
	struct cdp_surface_id resp;

	if (cdp_recv_msg(conn->sock_fd, &type, &resp,
			 sizeof(resp), &size) < 0 ||
	    type != CDP_EVT_SURFACE_ID) {
		fprintf(stderr, "CDP: SURFACE_ID 수신 실패\n");
		return NULL;
	}

	win->surface_id = resp.surface_id;

	/*
	 * 2. 공유메모리 생성 (memfd_create)
	 *
	 * memfd_create → ftruncate → mmap 의 3단계:
	 *   memfd_create: 익명 메모리 파일 생성 (fd 반환)
	 *   ftruncate:    크기 설정
	 *   mmap:         프로세스 주소 공간에 매핑
	 *
	 * 이후 이 fd를 서버에 SCM_RIGHTS로 전달하면
	 * 서버도 같은 메모리를 mmap할 수 있음!
	 */
	win->shm_fd = cdp_memfd_create("cdp-buffer");
	if (win->shm_fd < 0) {
		fprintf(stderr, "CDP: memfd_create 실패: %s\n",
			strerror(errno));
		return NULL;
	}

	if (ftruncate(win->shm_fd, (off_t)win->shm_size) < 0) {
		fprintf(stderr, "CDP: ftruncate 실패: %s\n",
			strerror(errno));
		close(win->shm_fd);
		return NULL;
	}

	win->pixels = (uint32_t *)mmap(NULL, win->shm_size,
				       PROT_READ | PROT_WRITE,
				       MAP_SHARED, win->shm_fd, 0);
	if (win->pixels == MAP_FAILED) {
		fprintf(stderr, "CDP: mmap 실패: %s\n", strerror(errno));
		close(win->shm_fd);
		win->pixels = NULL;
		return NULL;
	}

	/* 검은색으로 초기화 */
	memset(win->pixels, 0, win->shm_size);

	/* 3. ATTACH_BUFFER: 버퍼 정보 + fd 전달 */
	struct cdp_attach_buffer abuf;

	abuf.surface_id = win->surface_id;
	abuf.width = win->width;
	abuf.height = win->height;
	abuf.stride = win->stride;
	abuf.format = 0;  /* XRGB8888 */

	if (cdp_send_msg(conn->sock_fd, CDP_REQ_ATTACH_BUFFER,
			 &abuf, sizeof(abuf)) < 0) {
		fprintf(stderr, "CDP: ATTACH_BUFFER 전송 실패\n");
		goto fail;
	}

	/* SCM_RIGHTS로 fd 전달! (핵심!) */
	if (cdp_send_fd(conn->sock_fd, win->shm_fd) < 0) {
		fprintf(stderr, "CDP: fd 전달 실패\n");
		goto fail;
	}

	/* 4. SET_TITLE */
	if (title && title[0]) {
		struct cdp_set_title st;

		st.surface_id = win->surface_id;
		memset(st.title, 0, sizeof(st.title));
		strncpy(st.title, title, sizeof(st.title) - 1);

		cdp_send_msg(conn->sock_fd, CDP_REQ_SET_TITLE,
			     &st, sizeof(st));
	}

	win->active = 1;
	conn->num_windows++;

	printf("CDP: surface %u 생성 완료 (%dx%d, shm=%u bytes)\n",
	       win->surface_id, width, height, win->shm_size);

	return win;

fail:
	munmap(win->pixels, win->shm_size);
	close(win->shm_fd);
	win->pixels = NULL;
	win->shm_fd = -1;
	return NULL;
}

/* ============================================================
 * Surface 조작
 * ============================================================ */

/*
 * 화면 갱신 요청
 * Wayland: wl_surface.commit()
 *
 * 클라이언트가 pixels에 그리기를 완료한 후 호출.
 * 서버가 이 메시지를 받으면 다음 프레임에 새 내용을 표시.
 */
static void cdp_commit_to(struct cdp_conn *conn, struct cdp_window *win)
{
	if (!conn || !win || !win->active)
		return;

	struct cdp_commit req;

	req.surface_id = win->surface_id;
	cdp_send_msg(conn->sock_fd, CDP_REQ_COMMIT, &req, sizeof(req));
}

/*
 * 프레임 콜백 요청
 * Wayland: wl_surface.frame() → wl_callback
 *
 * "다음 화면 갱신이 끝나면 알려줘"
 * → conn->on_frame 콜백이 호출됨
 */
__attribute__((unused))
static void cdp_request_frame(struct cdp_conn *conn, struct cdp_window *win)
{
	if (!conn || !win || !win->active)
		return;

	struct cdp_frame_req req;

	req.surface_id = win->surface_id;
	cdp_send_msg(conn->sock_fd, CDP_REQ_FRAME, &req, sizeof(req));
}

/*
 * 패널 surface 설정
 * Wayland: wlr-layer-shell (zwlr_layer_surface_v1)
 *
 * 일반 surface를 패널(태스크바)로 전환.
 * 컴포지터가 이 surface를:
 *   - 화면 가장자리에 고정
 *   - 항상 최상위에 표시
 *   - 타이틀바/테두리 제거
 *   - 드래그 불가로 처리
 */
__attribute__((unused))
static void cdp_set_panel(struct cdp_conn *conn, struct cdp_window *win,
			  uint32_t edge, uint32_t height)
{
	if (!conn || !win || !win->active)
		return;

	struct cdp_set_panel req;

	req.surface_id = win->surface_id;
	req.edge = edge;
	req.height = height;

	cdp_send_msg(conn->sock_fd, CDP_REQ_SET_PANEL,
		     &req, sizeof(req));
}

/*
 * Surface 삭제
 * Wayland: wl_surface.destroy()
 */
__attribute__((unused))
static void cdp_destroy_surface(struct cdp_conn *conn, struct cdp_window *win)
{
	if (!conn || !win || !win->active)
		return;

	struct cdp_destroy_surface req;

	req.surface_id = win->surface_id;
	cdp_send_msg(conn->sock_fd, CDP_REQ_DESTROY_SURFACE,
		     &req, sizeof(req));

	if (win->pixels) {
		munmap(win->pixels, win->shm_size);
		win->pixels = NULL;
	}
	if (win->shm_fd >= 0) {
		close(win->shm_fd);
		win->shm_fd = -1;
	}
	win->active = 0;
}

/* ============================================================
 * 이벤트 디스패치
 * ============================================================
 *
 * Wayland 대응: wl_display_dispatch()
 *
 * 서버에서 오는 이벤트를 하나 읽고 적절한 콜백을 호출.
 * 블로킹 함수 — 이벤트가 올 때까지 대기.
 *
 * 반환: 0 (성공), -1 (연결 끊김)
 */
static int cdp_dispatch(struct cdp_conn *conn)
{
	if (!conn)
		return -1;

	uint32_t type, size;
	uint8_t payload[CDP_MSG_MAX_PAYLOAD];

	if (cdp_recv_msg(conn->sock_fd, &type, payload,
			 sizeof(payload), &size) < 0)
		return -1;

	switch (type) {
	case CDP_EVT_FRAME_DONE: {
		struct cdp_frame_done *evt = (struct cdp_frame_done *)payload;

		if (conn->on_frame)
			conn->on_frame(evt->surface_id);
		break;
	}
	case CDP_EVT_KEY: {
		struct cdp_key *evt = (struct cdp_key *)payload;

		if (conn->on_key)
			conn->on_key(evt->keycode, evt->state,
				     (char)evt->character);
		break;
	}
	case CDP_EVT_POINTER_MOTION: {
		struct cdp_pointer_motion *evt =
			(struct cdp_pointer_motion *)payload;
		if (conn->on_pointer_motion)
			conn->on_pointer_motion(evt->surface_id,
						evt->x, evt->y);
		break;
	}
	case CDP_EVT_POINTER_BUTTON: {
		struct cdp_pointer_button *evt =
			(struct cdp_pointer_button *)payload;
		if (conn->on_pointer_button)
			conn->on_pointer_button(evt->surface_id,
						evt->button, evt->state);
		break;
	}
	case CDP_EVT_POINTER_ENTER: {
		struct cdp_pointer_enter *evt =
			(struct cdp_pointer_enter *)payload;
		if (conn->on_pointer_enter)
			conn->on_pointer_enter(evt->surface_id,
					       evt->x, evt->y);
		break;
	}
	case CDP_EVT_POINTER_LEAVE: {
		struct cdp_pointer_leave *evt =
			(struct cdp_pointer_leave *)payload;
		if (conn->on_pointer_leave)
			conn->on_pointer_leave(evt->surface_id);
		break;
	}
	case CDP_EVT_FOCUS_IN: {
		struct cdp_focus_in *evt = (struct cdp_focus_in *)payload;

		if (conn->on_focus_in)
			conn->on_focus_in(evt->surface_id);
		break;
	}
	case CDP_EVT_FOCUS_OUT: {
		struct cdp_focus_out *evt = (struct cdp_focus_out *)payload;

		if (conn->on_focus_out)
			conn->on_focus_out(evt->surface_id);
		break;
	}
	default:
		/* 알 수 없는 이벤트 무시 (프로토콜 버전 호환) */
		break;
	}

	return 0;
}

#endif /* CDP_CLIENT_H */
