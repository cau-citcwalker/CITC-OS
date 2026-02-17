/*
 * CITC OS Compositor - 윈도우 관리자 + 입력 시스템
 * =================================================
 *
 * 이전 drmdraw는 DRM/KMS로 화면에 그리기만 했습니다.
 * 이번에는 "대화형" 그래픽 시스템을 만듭니다:
 *   - 마우스 커서가 움직이고
 *   - 윈도우를 클릭하고 드래그하고
 *   - 키보드로 글자를 입력합니다
 *
 * 이것이 Wayland 컴포지터의 핵심 기반입니다!
 *
 * 컴포지터(Compositor)란?
 *   여러 앱의 윈도우를 하나의 화면에 합성(composite)하는 프로그램.
 *   Windows의 DWM, macOS의 Quartz Compositor와 같은 역할.
 *
 *   합성 과정:
 *     1. 배경 그리기
 *     2. 윈도우들을 뒤에서 앞으로 그리기 (painter's algorithm)
 *     3. 마우스 커서 그리기 (항상 최상위)
 *     4. 버퍼 스왑 (화면에 표시)
 *
 * 새로 배우는 것:
 *   1. Linux 입력 시스템 (evdev) — 키보드/마우스 읽기
 *   2. 이벤트 루프 (poll) — 여러 입력을 동시에 대기
 *   3. 윈도우 관리 — 포커스, 드래그, Z-order
 *   4. 합성 렌더링 — 여러 레이어를 하나로 합치기
 *
 * 빌드:
 *   gcc -static -Wall -o compositor compositor.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>

/*
 * DRM 헤더 (drmdraw에서 배움)
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>

#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED         1
#define DRM_MODE_DISCONNECTED      2
#define DRM_MODE_UNKNOWNCONNECTION 3
#endif

/*
 * Linux 입력 시스템 헤더
 *
 * <linux/input.h>에 정의된 것들:
 *   struct input_event — 입력 이벤트 구조체
 *   EV_KEY, EV_REL, EV_ABS — 이벤트 타입
 *   KEY_A ~ KEY_Z, KEY_ESC — 키 코드
 *   REL_X, REL_Y — 마우스 상대 이동
 *   BTN_LEFT, BTN_RIGHT — 마우스 버튼
 *
 * <linux/input-event-codes.h>는 input.h가 자동 포함.
 */
#include <linux/input.h>

/*
 * CITC Display Protocol (CDP) — Class 12에서 추가
 *
 * 이전까지 컴포지터의 윈도우는 모두 내부에서 하드코딩했습니다.
 * 이제 외부 프로세스가 소켓으로 연결하여 윈도우를 만들 수 있습니다!
 *
 * 이것이 Wayland의 핵심 아이디어:
 *   앱(클라이언트) → Unix 소켓 연결 → surface 요청
 *   → 공유메모리에 직접 그리기 → 컴포지터가 합성
 *
 * 필요한 추가 헤더:
 *   sys/socket.h — 소켓 API (socket, bind, listen, accept, sendmsg, recvmsg)
 *   sys/un.h     — Unix domain socket 주소 구조체 (sockaddr_un)
 */
#include <sys/socket.h>
#include <sys/un.h>

#include "../../protocol/cdp_proto.h"

/* 8x8 비트맵 폰트 (이전 수업에서 공유) */
#include "../../fbdraw/src/font8x8.h"

/* PSF2 폰트 (Class 61 추가) — 8x16 고품질 폰트 */
#include "../../font/psf2.h"

/* 전역 PSF2 폰트 (로드 실패 시 font8x8 fallback) */
static struct psf2_font g_psf2;

#define PSF2_FONT_PATH "/usr/share/fonts/ter-116n.psf"

/* ============================================================
 * 상수 정의
 * ============================================================ */

#define MAX_WINDOWS    8     /* 최대 윈도우 개수 */
#define MAX_INPUT_FDS  4     /* 최대 입력 장치 수 */
#define TITLEBAR_H     24    /* 타이틀바 높이 (픽셀) */
#define CLOSE_BTN_W    20    /* 닫기 버튼 너비 */
#define WIN_TEXT_MAX   256   /* 윈도우 텍스트 버퍼 크기 */
#define CURSOR_SIZE    12    /* 커서 크기 */
#define RESIZE_EDGE    4     /* 리사이즈 감지 엣지 두께 (Class 59) */
#define RESIZE_CORNER  8     /* 리사이즈 코너 크기 (Class 59) */
#define MIN_WIN_W      100   /* 최소 윈도우 너비 (Class 59) */
#define MIN_WIN_H      60    /* 최소 윈도우 높이 (Class 59) */
#define BTN_W          20    /* 타이틀바 버튼 너비 (Class 59) */

/* CDP (CITC Display Protocol) 상수 — Class 12 추가 */
#define MAX_CDP_CLIENTS    4     /* 최대 동시 연결 클라이언트 */
#define MAX_CDP_SURFACES   4     /* 최대 CDP surface 수 */

/* ============================================================
 * DRM 데이터 구조 (drmdraw에서 가져옴)
 * ============================================================ */

struct drm_buf {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t size;
	uint32_t handle;
	uint32_t fb_id;
	uint8_t *map;
};

static struct {
	int fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	struct drm_mode_modeinfo mode;
	uint32_t saved_crtc_fb;
	struct drm_buf bufs[2];
	int front;
} drm;

/* ============================================================
 * 윈도우 구조체
 * ============================================================
 *
 * 윈도우 = 화면 위의 사각형 영역.
 * 실제 OS에서 윈도우는:
 *   - 위치 (x, y)
 *   - 크기 (width, height)
 *   - 제목 (titlebar)
 *   - 내용 (client area)
 *   - 상태 (포커스, 최소화, 최대화 등)
 *
 * Z-order:
 *   윈도우가 겹칠 때 어떤 것이 위에 보이는지.
 *   windows[] 배열에서 뒤쪽 인덱스가 위에 그려짐.
 *   클릭하면 해당 윈도우를 배열 맨 뒤로 이동 → 최상위.
 */
struct window {
	int x, y;                       /* 위치 (좌상단) */
	int w, h;                       /* 크기 (타이틀바 포함) */
	char title[64];                 /* 제목 */
	char text[WIN_TEXT_MAX];        /* 내용 텍스트 */
	int text_len;                   /* 텍스트 길이 */
	int visible;                    /* 보이는지 */
	uint8_t color_r, color_g, color_b;  /* 타이틀바 색상 */

	/* CDP 연결 정보 — surface가 연결된 윈도우 (Class 12 추가) */
	int cdp_surface_idx;                /* CDP surface 인덱스 (-1=내부 윈도우) */

	/*
	 * 패널 윈도우 플래그 (Class 17 추가)
	 *
	 * 패널 = 태스크바 같은 특수 윈도우.
	 * Wayland의 wlr-layer-shell에 해당.
	 *   - 화면 가장자리에 고정
	 *   - 타이틀바/테두리 없음
	 *   - 드래그 불가
	 *   - 항상 일반 윈도우 위에 렌더링
	 */
	int is_panel;

	/*
	 * 최소화/최대화 상태 (Class 59 추가)
	 *
	 * minimized: 태스크바에만 표시, 화면에서 숨김
	 * maximized: 화면 전체 차지 (패널 영역 제외)
	 * saved_*: 최대화 전 원래 위치/크기 저장 (복원용)
	 */
	int minimized;
	int maximized;
	int saved_x, saved_y, saved_w, saved_h;
};

/* ============================================================
 * CDP Surface (외부 앱의 윈도우) — Class 12 추가
 * ============================================================
 *
 * Wayland에서의 대응:
 *   struct cdp_surface ↔ wl_surface + wl_buffer
 *
 * 내부 윈도우와의 차이:
 *   내부 윈도우: 텍스트를 draw_char()로 직접 그림
 *   CDP surface: 클라이언트가 공유메모리에 픽셀을 그리고,
 *                컴포지터가 그 메모리를 DRM 버퍼에 복사(blit)
 *
 * 공유메모리 흐름:
 *   클라이언트                  서버(컴포지터)
 *   memfd_create() → fd
 *   ftruncate(fd, size)
 *   mmap(fd) → pixels
 *   [pixels에 그리기]
 *   sendmsg(SCM_RIGHTS, fd) → → recvmsg() → fd 수신
 *                               mmap(fd) → shm_map
 *                               [shm_map에서 읽기 → DRM 버퍼에 복사]
 */
struct cdp_surface {
	int active;                   /* 사용 중인지 */
	int window_idx;               /* comp.windows[] 인덱스 */
	int client_idx;               /* cdp.clients[] 인덱스 */

	/* 공유메모리 버퍼 (Wayland: wl_buffer backed by wl_shm_pool) */
	int shm_fd;                   /* memfd 파일 디스크립터 */
	uint8_t *shm_map;            /* mmap된 포인터 (서버 측) */
	uint32_t shm_size;           /* 매핑 전체 크기 */
	uint32_t buf_width;          /* 버퍼 너비 (픽셀) */
	uint32_t buf_height;         /* 버퍼 높이 (픽셀) */
	uint32_t buf_stride;         /* 한 줄 바이트 수 */

	uint32_t format;             /* 0=XRGB8888, 1=ARGB8888 (Class 60) */
	int committed;                /* commit 되었는지 (렌더링 가능) */
	int frame_requested;          /* 프레임 콜백 요청됨 */
};

/*
 * CDP 클라이언트 (연결된 외부 앱)
 *
 * Wayland: wl_client
 * 하나의 소켓 연결 = 하나의 클라이언트 = 하나의 앱
 */
struct cdp_client {
	int fd;                       /* 소켓 fd (-1 = 빈 슬롯) */
};

/* ============================================================
 * 입력 상태
 * ============================================================
 *
 * evdev(event device)란?
 *   Linux의 통합 입력 인터페이스.
 *   모든 입력 장치(키보드, 마우스, 터치스크린, 게임패드)가
 *   /dev/input/eventX 파일로 노출됩니다.
 *
 *   각 파일에서 struct input_event를 read()하면 이벤트를 받을 수 있음.
 *
 *   struct input_event {
 *       struct timeval time;  // 타임스탬프
 *       __u16 type;           // EV_KEY, EV_REL, EV_ABS, EV_SYN
 *       __u16 code;           // KEY_A, REL_X, BTN_LEFT 등
 *       __s32 value;          // 0=해제, 1=누름, 2=반복 (키)
 *                             // 상대 이동량 (마우스 REL)
 *   };
 *
 *   이벤트 타입:
 *     EV_KEY: 키보드 키 또는 마우스 버튼
 *     EV_REL: 마우스 상대 이동 (dx, dy)
 *     EV_ABS: 터치스크린 절대 좌표
 *     EV_SYN: 이벤트 묶음 끝 (동기화 마커)
 *
 *   동기화(SYN)란?
 *     마우스가 대각선으로 움직이면 REL_X와 REL_Y 이벤트가 따로 옴.
 *     EV_SYN은 "여기까지가 한 묶음"이라는 표시.
 *     예: REL_X(+5), REL_Y(-3), SYN → 마우스가 (5, -3) 이동
 */

/* 입력 장치 종류 */
enum input_type {
	INPUT_KEYBOARD,
	INPUT_MOUSE,
};

struct input_dev {
	int fd;                  /* 파일 디스크립터 */
	enum input_type type;    /* 키보드 or 마우스 */
	int is_abs;              /* 1=절대좌표(태블릿), 0=상대좌표(마우스) */
	int abs_max_x, abs_max_y; /* 절대좌표 장치의 최대값 */
	char name[64];           /* 장치 이름 */
};

/* ============================================================
 * 전역 컴포지터 상태
 * ============================================================ */

static struct {
	/* 윈도우 */
	struct window windows[MAX_WINDOWS];
	int num_windows;
	int focused;            /* 포커스된 윈도우 인덱스 (-1=없음) */

	/* 입력 */
	struct input_dev inputs[MAX_INPUT_FDS];
	int num_inputs;

	/* 마우스 상태 */
	int mouse_x, mouse_y;   /* 커서 위치 */
	int mouse_btn_left;      /* 왼쪽 버튼 눌림 상태 */

	/* 드래그 상태 */
	int dragging;            /* 드래그 중인 윈도우 인덱스 (-1=없음) */
	int drag_off_x, drag_off_y;  /* 드래그 시작 시 커서-윈도우 오프셋 */

	/* 실행 상태 */
	int running;
	int need_redraw;         /* 화면 갱신 필요 플래그 */

	/* 데미지 트래킹 (Class 58)
	 *
	 * 매 프레임 전체를 다시 그리는 대신,
	 * 변경된 영역(데미지)만 다시 그림.
	 *
	 * damage_full = 1: 전체 화면 리드로 (윈도우 생성/삭제 등)
	 * damage_full = 0: damage_rects[]에 기록된 영역만 리드로
	 * damage_count = 0 && damage_full = 0: 리드로 불필요 (idle)
	 */
	struct { int x, y, w, h; } damage_rects[32];
	int damage_count;
	int damage_full;         /* 전체 화면 리드로 필요 */

	/* 이전 커서 위치 (커서 데미지용) */
	int prev_mouse_x, prev_mouse_y;

	/* 배경 캐시 — 그래디언트를 매번 다시 계산하지 않음 */
	uint32_t *bg_cache;
	int bg_cache_valid;

	/* 리사이즈 상태 (Class 59) */
	int resizing;            /* 리사이즈 중인 윈도우 인덱스 (-1=없음) */
	int resize_edge;         /* 0=none, 1=right, 2=bottom, 3=corner */
	int resize_start_x, resize_start_y;  /* 리사이즈 시작 시 마우스 위치 */
	int resize_orig_w, resize_orig_h;    /* 리사이즈 시작 시 원래 크기 */
} comp;

/* ============================================================
 * CDP 서버 상태 — Class 12 추가
 * ============================================================
 *
 * Wayland 컴포지터의 서버 측 상태에 해당.
 * listen_fd로 새 연결을 받고,
 * 각 클라이언트는 자기만의 surface를 가짐.
 */
static struct {
	int listen_fd;                                /* 리슨 소켓 */
	struct cdp_client clients[MAX_CDP_CLIENTS];   /* 연결된 클라이언트 */
	struct cdp_surface surfaces[MAX_CDP_SURFACES]; /* CDP surfaces */
	uint32_t next_surface_id;                     /* 다음 surface ID */

	/* 클립보드 (Class 62) */
	char clipboard_buf[CDP_CLIPBOARD_MAX];
	uint32_t clipboard_len;
} cdp;

/* ============================================================
 * 데미지 트래킹 — Class 58 추가
 * ============================================================
 *
 * 데미지 트래킹이란?
 *   화면에서 변경된 영역만 다시 그리는 최적화 기법.
 *   PulseAudio가 무음일 때 CPU를 안 쓰듯,
 *   컴포지터도 변경 없으면 렌더링을 건너뛸 수 있음.
 *
 *   Wayland: wl_surface.damage_buffer() → 컴포지터가 영역 추적
 *   X11: XDamage 확장
 *
 *   효과:
 *     마우스만 이동: 12x12 영역 2개만 리드로 (이전+새 위치)
 *     완전 idle: 렌더링 0회 (CPU 사용률 최소)
 *     윈도우 변경: 해당 윈도우 영역만 리드로
 */
static void damage_reset(void)
{
	comp.damage_count = 0;
	comp.damage_full = 0;
}

static void damage_add(int x, int y, int w, int h)
{
	if (comp.damage_full)
		return; /* 이미 전체 리드로 예정 */

	if (comp.damage_count >= 32) {
		/* 데미지 rect가 너무 많으면 전체 리드로 */
		comp.damage_full = 1;
		return;
	}

	comp.damage_rects[comp.damage_count].x = x;
	comp.damage_rects[comp.damage_count].y = y;
	comp.damage_rects[comp.damage_count].w = w;
	comp.damage_rects[comp.damage_count].h = h;
	comp.damage_count++;
}

static void damage_add_full(void)
{
	comp.damage_full = 1;
}

static void damage_add_window(int win_idx)
{
	if (win_idx < 0 || win_idx >= comp.num_windows)
		return;
	struct window *w = &comp.windows[win_idx];
	/* 윈도우 전체 영역 (테두리 2px 포함) */
	damage_add(w->x - 2, w->y - 2, w->w + 4, w->h + 4);
}

static int damage_has_any(void)
{
	return comp.damage_full || comp.damage_count > 0;
}

/* ============================================================
 * DRM 함수들 (drmdraw.c에서 가져옴, 주석 간소화)
 * ============================================================
 *
 * 이 함수들은 Class 10에서 상세히 배웠습니다.
 * 여기서는 간단한 주석만 붙입니다.
 */

/* Dumb 버퍼 생성 (4단계: CREATE → ADDFB → MAP → mmap) */
static int buf_create(struct drm_buf *buf, uint32_t width, uint32_t height)
{
	struct drm_mode_create_dumb create = {0};
	struct drm_mode_map_dumb map_req = {0};
	struct drm_mode_fb_cmd fb_cmd = {0};

	buf->width = width;
	buf->height = height;

	create.width = width;
	create.height = height;
	create.bpp = 32;
	if (ioctl(drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("CREATE_DUMB");
		return -1;
	}
	buf->handle = create.handle;
	buf->pitch = create.pitch;
	buf->size = create.size;

	fb_cmd.width = width;
	fb_cmd.height = height;
	fb_cmd.pitch = buf->pitch;
	fb_cmd.bpp = 32;
	fb_cmd.depth = 24;
	fb_cmd.handle = buf->handle;
	if (ioctl(drm.fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) < 0) {
		perror("ADDFB");
		goto err_destroy;
	}
	buf->fb_id = fb_cmd.fb_id;

	map_req.handle = buf->handle;
	if (ioctl(drm.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
		perror("MAP_DUMB");
		goto err_rmfb;
	}

	buf->map = mmap(NULL, buf->size, PROT_READ | PROT_WRITE,
			MAP_SHARED, drm.fd, map_req.offset);
	if (buf->map == MAP_FAILED) {
		perror("mmap");
		goto err_rmfb;
	}
	memset(buf->map, 0, buf->size);
	return 0;

err_rmfb:
	ioctl(drm.fd, DRM_IOCTL_MODE_RMFB, &buf->fb_id);
err_destroy:
	{
		struct drm_mode_destroy_dumb d = { .handle = buf->handle };
		ioctl(drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	return -1;
}

static void buf_destroy(struct drm_buf *buf)
{
	if (buf->map && buf->map != MAP_FAILED)
		munmap(buf->map, buf->size);
	if (buf->fb_id)
		ioctl(drm.fd, DRM_IOCTL_MODE_RMFB, &buf->fb_id);
	if (buf->handle) {
		struct drm_mode_destroy_dumb d = { .handle = buf->handle };
		ioctl(drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	memset(buf, 0, sizeof(*buf));
}

/*
 * DRM 초기화 (Class 10에서 배운 것과 동일)
 * 모든 배열 포인터 필수 규칙 적용됨.
 */
static int drm_init(void)
{
	struct drm_mode_card_res res = {0};
	struct drm_mode_get_connector conn = {0};
	struct drm_mode_get_encoder enc = {0};
	struct drm_mode_crtc saved_crtc = {0};
	uint32_t *conn_ids = NULL, *crtc_ids = NULL;
	uint32_t *enc_ids_res = NULL, *fb_ids = NULL;
	struct drm_mode_modeinfo *modes = NULL;
	uint32_t *enc_ids = NULL;
	uint32_t *props = NULL;
	uint64_t *prop_values = NULL;
	int found = 0;

	drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm.fd < 0) {
		perror("/dev/dri/card0");
		return -1;
	}

	/* 1st pass: 리소스 카운트 */
	if (ioctl(drm.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("GETRESOURCES 1st");
		goto err;
	}

	if (res.count_connectors == 0 || res.count_crtcs == 0) {
		printf("디스플레이 없음\n");
		goto err;
	}

	/* 모든 배열 할당 (빠뜨리면 EFAULT!) */
	conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
	crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
	enc_ids_res = calloc(res.count_encoders ? res.count_encoders : 1,
			     sizeof(uint32_t));
	fb_ids = calloc(res.count_fbs ? res.count_fbs : 1, sizeof(uint32_t));

	res.connector_id_ptr = (uint64_t)(unsigned long)conn_ids;
	res.crtc_id_ptr = (uint64_t)(unsigned long)crtc_ids;
	res.encoder_id_ptr = (uint64_t)(unsigned long)enc_ids_res;
	res.fb_id_ptr = (uint64_t)(unsigned long)fb_ids;

	/* 2nd pass: 데이터 채우기 */
	if (ioctl(drm.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("GETRESOURCES 2nd");
		goto err;
	}

	/* 연결된 커넥터 찾기 */
	for (uint32_t i = 0; i < res.count_connectors && !found; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_ids[i];

		if (ioctl(drm.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		if (conn.connection == DRM_MODE_DISCONNECTED)
			continue;
		if (conn.count_modes == 0)
			continue;

		/* 모든 배열 할당 (props 포함!) */
		modes = calloc(conn.count_modes,
			       sizeof(struct drm_mode_modeinfo));
		enc_ids = calloc(conn.count_encoders ? conn.count_encoders : 1,
				 sizeof(uint32_t));
		props = calloc(conn.count_props ? conn.count_props : 1,
			       sizeof(uint32_t));
		prop_values = calloc(conn.count_props ? conn.count_props : 1,
				     sizeof(uint64_t));

		conn.modes_ptr = (uint64_t)(unsigned long)modes;
		conn.encoders_ptr = (uint64_t)(unsigned long)enc_ids;
		conn.props_ptr = (uint64_t)(unsigned long)props;
		conn.prop_values_ptr = (uint64_t)(unsigned long)prop_values;

		if (ioctl(drm.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
			free(modes); modes = NULL;
			free(enc_ids); enc_ids = NULL;
			free(props); props = NULL;
			free(prop_values); prop_values = NULL;
			continue;
		}

		/* preferred 모드 선택 */
		drm.mode = modes[0];
		for (uint32_t j = 0; j < conn.count_modes; j++) {
			if (modes[j].type & DRM_MODE_TYPE_PREFERRED)
				drm.mode = modes[j];
		}

		drm.conn_id = conn.connector_id;

		/* 인코더 → CRTC 매핑 */
		if (conn.encoder_id) {
			memset(&enc, 0, sizeof(enc));
			enc.encoder_id = conn.encoder_id;
			if (ioctl(drm.fd, DRM_IOCTL_MODE_GETENCODER,
				  &enc) == 0)
				drm.crtc_id = enc.crtc_id;
		}
		if (!drm.crtc_id && res.count_crtcs > 0)
			drm.crtc_id = crtc_ids[0];

		found = 1;
	}

	if (!found) {
		printf("디스플레이를 찾을 수 없음\n");
		goto err;
	}

	printf("[DRM] %ux%u @%uHz\n",
	       drm.mode.hdisplay, drm.mode.vdisplay, drm.mode.vrefresh);

	/* 현재 CRTC 저장 */
	saved_crtc.crtc_id = drm.crtc_id;
	if (ioctl(drm.fd, DRM_IOCTL_MODE_GETCRTC, &saved_crtc) == 0)
		drm.saved_crtc_fb = saved_crtc.fb_id;

	/* 더블 버퍼 생성 */
	if (buf_create(&drm.bufs[0], drm.mode.hdisplay,
		       drm.mode.vdisplay) < 0)
		goto err;
	if (buf_create(&drm.bufs[1], drm.mode.hdisplay,
		       drm.mode.vdisplay) < 0) {
		buf_destroy(&drm.bufs[0]);
		goto err;
	}

	/* 첫 화면 표시 */
	{
		struct drm_mode_crtc crtc = {0};
		crtc.crtc_id = drm.crtc_id;
		crtc.fb_id = drm.bufs[0].fb_id;
		crtc.set_connectors_ptr =
			(uint64_t)(unsigned long)&drm.conn_id;
		crtc.count_connectors = 1;
		crtc.mode = drm.mode;
		crtc.mode_valid = 1;
		if (ioctl(drm.fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
			perror("SETCRTC");
			buf_destroy(&drm.bufs[0]);
			buf_destroy(&drm.bufs[1]);
			goto err;
		}
	}
	drm.front = 0;

	free(conn_ids); free(crtc_ids);
	free(enc_ids_res); free(fb_ids);
	free(modes); free(enc_ids);
	free(props); free(prop_values);
	return 0;

err:
	free(conn_ids); free(crtc_ids);
	free(enc_ids_res); free(fb_ids);
	free(modes); free(enc_ids);
	free(props); free(prop_values);
	if (drm.fd >= 0) close(drm.fd);
	drm.fd = -1;
	return -1;
}

static void drm_cleanup(void)
{
	if (drm.saved_crtc_fb) {
		struct drm_mode_crtc crtc = {0};
		crtc.crtc_id = drm.crtc_id;
		crtc.fb_id = drm.saved_crtc_fb;
		crtc.set_connectors_ptr =
			(uint64_t)(unsigned long)&drm.conn_id;
		crtc.count_connectors = 1;
		crtc.mode = drm.mode;
		crtc.mode_valid = 1;
		ioctl(drm.fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	}
	buf_destroy(&drm.bufs[0]);
	buf_destroy(&drm.bufs[1]);
	if (drm.fd >= 0) close(drm.fd);
}

/* Back 버퍼 가져오기 */
static struct drm_buf *back_buf(void)
{
	return &drm.bufs[drm.front ^ 1];
}

/* 버퍼 스왑 (더블 버퍼링) */
static void drm_swap(void)
{
	struct drm_mode_crtc crtc = {0};
	int back = drm.front ^ 1;

	crtc.crtc_id = drm.crtc_id;
	crtc.fb_id = drm.bufs[back].fb_id;
	crtc.set_connectors_ptr = (uint64_t)(unsigned long)&drm.conn_id;
	crtc.count_connectors = 1;
	crtc.mode = drm.mode;
	crtc.mode_valid = 1;

	if (ioctl(drm.fd, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0)
		drm.front = back;
}

/* ============================================================
 * 그리기 함수들 (drmdraw.c에서 가져옴)
 * ============================================================ */

static void draw_pixel(struct drm_buf *buf,
		       int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 ||
	    (uint32_t)x >= buf->width || (uint32_t)y >= buf->height)
		return;
	uint32_t *p = (uint32_t *)(buf->map + y * buf->pitch + x * 4);
	*p = color;
}

static void draw_rect(struct drm_buf *buf,
		      int x, int y, int w, int h, uint32_t color)
{
	for (int row = y; row < y + h; row++) {
		if (row < 0 || (uint32_t)row >= buf->height)
			continue;
		uint32_t *line = (uint32_t *)(buf->map + row * buf->pitch);
		for (int col = x; col < x + w; col++) {
			if (col >= 0 && (uint32_t)col < buf->width)
				line[col] = color;
		}
	}
}

static void draw_char(struct drm_buf *buf,
		      int x, int y, char c, uint32_t color, int scale)
{
	/* PSF2 폰트 사용 (Class 61) */
	if (g_psf2.loaded && scale == 1) {
		uint32_t *fb = (uint32_t *)buf->map;
		int stride = (int)(buf->pitch / 4);

		psf2_draw_char(fb, stride, x, y, c, color, &g_psf2);
		return;
	}

	/* font8x8 fallback */
	unsigned char ch = (unsigned char)c;
	if (ch > 127) return;

	for (int row = 0; row < 8; row++) {
		uint8_t bits = font8x8_basic[ch][row];
		for (int col = 0; col < 8; col++) {
			if (bits & (1 << col)) {
				for (int sy = 0; sy < scale; sy++)
					for (int sx = 0; sx < scale; sx++)
						draw_pixel(buf,
							   x + col * scale + sx,
							   y + row * scale + sy,
							   color);
			}
		}
	}
}

/*
 * 폰트 너비/높이 조회 (PSF2 또는 font8x8)
 */
static int font_width(void)  { return g_psf2.loaded ? (int)g_psf2.width : 8; }
static int font_height(void) { return g_psf2.loaded ? (int)g_psf2.height : 8; }

static void draw_string(struct drm_buf *buf,
			int x, int y, const char *str,
			uint32_t color, int scale)
{
	int cw = font_width() * scale;

	while (*str) {
		draw_char(buf, x, y, *str, color, scale);
		x += cw;
		str++;
	}
}

/* 색상 헬퍼 (XRGB8888) */
static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ============================================================
 * 입력 시스템 (evdev)
 * ============================================================
 *
 * Linux 입력 스택:
 *
 *   하드웨어 (USB/PS2 키보드, 마우스)
 *       ↓
 *   커널 드라이버 (hid-generic, atkbd, psmouse)
 *       ↓
 *   evdev 서브시스템 (/dev/input/eventX)
 *       ↓
 *   유저스페이스 (이 프로그램이 read()로 이벤트 읽기)
 *
 * 장치 식별 방법:
 *   EVIOCGBIT ioctl로 장치가 지원하는 이벤트 타입을 조회.
 *   - EV_REL (상대 이동) 지원 → 일반 마우스
 *   - EV_ABS (절대 좌표) 지원 → 터치스크린/태블릿
 *   - EV_KEY + KEY_A 지원 → 키보드
 *
 * QEMU에서의 입력:
 *   QEMU는 기본적으로 USB 태블릿 장치를 제공합니다.
 *   태블릿은 EV_ABS (절대 좌표)를 사용 — EV_REL이 아님!
 *   이것은 마우스 통합(integration)을 위한 것:
 *   호스트 마우스 위치가 그대로 게스트 절대 좌표로 전달됨.
 *
 *   EV_ABS 장치는 좌표 범위가 있음 (예: 0~32767).
 *   이것을 화면 해상도에 맞게 스케일링해야 합니다.
 */

/* 비트 테스트 매크로 — 특정 비트가 설정되어 있는지 확인 */
#define BITS_PER_LONG  (sizeof(unsigned long) * 8)
#define NLONGS(x)      (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, array) \
	((array)[(bit) / BITS_PER_LONG] & (1UL << ((bit) % BITS_PER_LONG)))

/*
 * input_scan - 입력 장치 탐색
 *
 * /dev/input/ 디렉토리를 스캔하여 event* 파일을 찾고,
 * 각 장치가 키보드인지 마우스인지 판별합니다.
 */
static void input_scan(void)
{
	DIR *dir;
	struct dirent *ent;
	char path[280];

	dir = opendir("/dev/input");
	if (!dir) {
		printf("[INPUT] /dev/input 열기 실패\n");
		return;
	}

	while ((ent = readdir(dir)) != NULL && comp.num_inputs < MAX_INPUT_FDS) {
		/* "event"로 시작하는 파일만 처리 */
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		/*
		 * EVIOCGBIT(0, ...) — 지원하는 이벤트 타입 조회
		 *
		 * 비트맵으로 반환됨:
		 *   비트 EV_KEY 설정 → 키/버튼 이벤트 지원
		 *   비트 EV_REL 설정 → 상대 이동 이벤트 지원
		 *   비트 EV_ABS 설정 → 절대 좌표 이벤트 지원
		 */
		unsigned long evbits[NLONGS(EV_MAX)] = {0};
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
			close(fd);
			continue;
		}

		/*
		 * 장치 판별 순서가 중요!
		 *
		 * Virtio Tablet처럼 EV_REL과 EV_ABS를 동시에 지원하는
		 * 장치가 있음. EV_REL은 스크롤 휠 전용이고, 실제 마우스
		 * 이동은 EV_ABS(절대 좌표)로 옴.
		 *
		 * 따라서 EV_ABS(+ABS_X)를 먼저 체크해야 올바르게 분류됨:
		 *   1. EV_ABS + ABS_X 있음 → 절대좌표 마우스 (태블릿)
		 *   2. EV_REL 있음 → 상대좌표 마우스 (일반 마우스)
		 *   3. EV_KEY + KEY_A → 키보드
		 */
		struct input_dev *dev = &comp.inputs[comp.num_inputs];
		dev->fd = fd;

		/* 장치 이름 조회 (디버그용) */
		char name[64] = "Unknown";
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		snprintf(dev->name, sizeof(dev->name), "%s", name);

		/* ABS_X 지원 여부 확인 (절대좌표 장치 판별에 사용) */
		int has_abs_x = 0;
		if (TEST_BIT(EV_ABS, evbits)) {
			unsigned long absbits[NLONGS(ABS_MAX)] = {0};
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
			has_abs_x = TEST_BIT(ABS_X, absbits) != 0;
		}

		if (TEST_BIT(EV_ABS, evbits) && has_abs_x) {
			/*
			 * EV_ABS + ABS_X → 절대 좌표 마우스 (태블릿/터치스크린)
			 *
			 * QEMU Virtio Tablet, USB Tablet 등.
			 * 이 장치는 EV_REL도 지원할 수 있지만(스크롤 휠),
			 * 마우스 이동은 절대 좌표(ABS_X/ABS_Y)로 전달됨.
			 *
			 * 절대 좌표 장치의 범위를 조회:
			 *   EVIOCGABS(ABS_X) → struct input_absinfo
			 *   .minimum=0, .maximum=32767 (일반적)
			 *
			 * 화면 좌표 = (abs_value * screen_width) / abs_max
			 */
			struct input_absinfo abs_x = {0}, abs_y = {0};
			ioctl(fd, EVIOCGABS(ABS_X), &abs_x);
			ioctl(fd, EVIOCGABS(ABS_Y), &abs_y);

			dev->type = INPUT_MOUSE;
			dev->is_abs = 1;
			dev->abs_max_x = abs_x.maximum > 0 ? abs_x.maximum : 32767;
			dev->abs_max_y = abs_y.maximum > 0 ? abs_y.maximum : 32767;

			printf("[INPUT] 마우스(절대): %s (%s)\n",
			       path, dev->name);
			printf("        ABS 범위: X=0~%d, Y=0~%d\n",
			       dev->abs_max_x, dev->abs_max_y);
			comp.num_inputs++;
		} else if (TEST_BIT(EV_REL, evbits)) {
			/*
			 * EV_REL 지원 (EV_ABS 없음) → 상대 좌표 마우스
			 * PS/2 마우스, USB 마우스 등
			 */
			dev->type = INPUT_MOUSE;
			dev->is_abs = 0;
			printf("[INPUT] 마우스(상대): %s (%s)\n",
			       path, dev->name);
			comp.num_inputs++;
		} else if (TEST_BIT(EV_KEY, evbits)) {
			/*
			 * EV_KEY만으로는 키보드인지 확실하지 않음.
			 * 키보드는 일반적으로 알파벳 키를 지원함.
			 * KEY_A(30) 지원 여부로 확인.
			 */
			unsigned long keybits[NLONGS(KEY_MAX)] = {0};
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);

			if (TEST_BIT(KEY_A, keybits)) {
				dev->type = INPUT_KEYBOARD;
				printf("[INPUT] 키보드: %s (%s)\n",
				       path, dev->name);
				comp.num_inputs++;
			} else {
				close(fd);
			}
		} else {
			close(fd);
		}
	}

	closedir(dir);
	printf("[INPUT] 장치 %d개 발견\n\n", comp.num_inputs);
}

/*
 * 키코드 → ASCII 변환 (간이 매핑)
 *
 * 실제 OS에서는 XKB(X Keyboard Extension)나 libxkbcommon을 사용.
 * 여기서는 간단한 테이블로 영문+숫자만 처리.
 *
 * Linux 키코드는 물리 키 위치를 나타냄 (레이아웃 무관).
 * KEY_Q = 16, KEY_W = 17, ... (QWERTY 기준 위치)
 */
static const char keymap_lower[128] = {
	[KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4',
	[KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8',
	[KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = '-', [KEY_EQUAL] = '=',
	[KEY_TAB] = '\t', [KEY_GRAVE] = '`',
	[KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r',
	[KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i',
	[KEY_O] = 'o', [KEY_P] = 'p',
	[KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f',
	[KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k',
	[KEY_L] = 'l',
	[KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v',
	[KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm',
	[KEY_SPACE] = ' ', [KEY_DOT] = '.', [KEY_COMMA] = ',',
	[KEY_SLASH] = '/', [KEY_SEMICOLON] = ';',
	[KEY_APOSTROPHE] = '\'',
	[KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']',
	[KEY_BACKSLASH] = '\\',
};

/* Shift 누른 상태의 문자 매핑 (US 키보드 레이아웃) */
static const char keymap_upper[128] = {
	[KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$',
	[KEY_5] = '%', [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*',
	[KEY_9] = '(', [KEY_0] = ')', [KEY_MINUS] = '_', [KEY_EQUAL] = '+',
	[KEY_GRAVE] = '~',
	[KEY_Q] = 'Q', [KEY_W] = 'W', [KEY_E] = 'E', [KEY_R] = 'R',
	[KEY_T] = 'T', [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I',
	[KEY_O] = 'O', [KEY_P] = 'P',
	[KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F',
	[KEY_G] = 'G', [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K',
	[KEY_L] = 'L',
	[KEY_Z] = 'Z', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V',
	[KEY_B] = 'B', [KEY_N] = 'N', [KEY_M] = 'M',
	[KEY_SPACE] = ' ', [KEY_DOT] = '>', [KEY_COMMA] = '<',
	[KEY_SLASH] = '?', [KEY_SEMICOLON] = ':',
	[KEY_APOSTROPHE] = '"',
	[KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}',
	[KEY_BACKSLASH] = '|',
};

/*
 * Shift 키 상태 추적
 *
 * evdev는 물리 키 이벤트만 전달하므로,
 * Shift 누름/해제를 직접 추적해야 함.
 * 실제 OS는 XKB에서 modifier 상태를 관리.
 */
static int shift_held;
static int ctrl_held;

static char keycode_to_char(unsigned int code)
{
	if (code < 128) {
		/*
		 * Ctrl + 알파벳 → 제어 문자 (ASCII 1-26)
		 * Ctrl+A = 0x01, Ctrl+C = 0x03, Ctrl+D = 0x04, ...
		 * 터미널에서 필수적인 기능!
		 *   Ctrl+C → SIGINT (프로세스 중단)
		 *   Ctrl+D → EOF (쉘 종료)
		 *   Ctrl+L → 화면 지우기
		 */
		if (ctrl_held) {
			char base = keymap_lower[code];

			if (base >= 'a' && base <= 'z')
				return base - 'a' + 1;
			return 0;
		}
		if (shift_held && keymap_upper[code])
			return keymap_upper[code];
		return keymap_lower[code];
	}
	return 0;
}

/* ============================================================
 * 윈도우 관리
 * ============================================================ */

/*
 * 윈도우 생성
 *
 * 지금은 내장 윈도우만 생성합니다 (하드코딩).
 * 나중에 Wayland를 추가하면 외부 앱이 윈도우를 요청합니다.
 */
static int window_create(int x, int y, int w, int h,
			 const char *title,
			 uint8_t r, uint8_t g, uint8_t b)
{
	int idx = -1;

	/*
	 * 먼저 닫힌 윈도우 슬롯 재사용 시도.
	 * visible=0이고 CDP surface도 없는 슬롯 = 완전히 정리된 빈 슬롯.
	 */
	for (int i = 0; i < comp.num_windows; i++) {
		if (!comp.windows[i].visible &&
		    comp.windows[i].cdp_surface_idx < 0) {
			idx = i;
			break;
		}
	}

	/* 빈 슬롯이 없으면 새 슬롯 할당 */
	if (idx < 0) {
		if (comp.num_windows >= MAX_WINDOWS)
			return -1;
		idx = comp.num_windows++;
	}

	struct window *win = &comp.windows[idx];
	win->x = x;
	win->y = y;
	win->w = w;
	win->h = h;
	snprintf(win->title, sizeof(win->title), "%s", title);
	win->text[0] = '\0';
	win->text_len = 0;
	win->visible = 1;
	win->color_r = r;
	win->color_g = g;
	win->color_b = b;
	win->cdp_surface_idx = -1;  /* 내부 윈도우 (CDP 아님) */
	win->is_panel = 0;

	return idx;
}

/*
 * 좌표에 있는 윈도우 찾기 (위에서 아래로)
 *
 * Z-order: 배열 뒤쪽이 위에 있으므로 뒤에서부터 탐색.
 * 가장 위에 있는 (사용자에게 보이는) 윈도우를 반환.
 */
static int window_at_point(int px, int py)
{
	/*
	 * Class 17 변경: 패널을 먼저 검색.
	 * 패널은 항상 최상위에 렌더링되므로
	 * 히트 테스트에서도 먼저 찾아야 올바릅니다.
	 */
	for (int i = 0; i < comp.num_windows; i++) {
		struct window *w = &comp.windows[i];

		if (!w->visible || !w->is_panel)
			continue;
		if (px >= w->x && px < w->x + w->w &&
		    py >= w->y && py < w->y + w->h)
			return i;
	}

	/* 일반 윈도우: 뒤→앞 (Z-order 최상위부터) */
	for (int i = comp.num_windows - 1; i >= 0; i--) {
		struct window *w = &comp.windows[i];

		if (!w->visible || w->is_panel)
			continue;
		if (px >= w->x && px < w->x + w->w &&
		    py >= w->y && py < w->y + w->h)
			return i;
	}
	return -1;
}

/*
 * 윈도우 포커스 변경
 *
 * 클릭된 윈도우를 배열 맨 뒤로 이동 → Z-order 최상위.
 * 배열에서 해당 윈도우를 빼고, 나머지를 앞으로 당기고,
 * 맨 뒤에 삽입합니다.
 *
 * 예: [A, B, C, D] 에서 B를 클릭
 *     → [A, C, D, B] (B가 최상위)
 */
static void window_focus(int idx)
{
	if (idx < 0 || idx >= comp.num_windows)
		return;

	comp.focused = comp.num_windows - 1; /* 맨 뒤가 포커스 */

	if (idx == comp.num_windows - 1)
		return; /* 이미 최상위 */

	/* 윈도우를 배열 맨 뒤로 이동 */
	struct window tmp = comp.windows[idx];
	for (int i = idx; i < comp.num_windows - 1; i++)
		comp.windows[i] = comp.windows[i + 1];
	comp.windows[comp.num_windows - 1] = tmp;
}

/*
 * 타이틀바 영역인지 확인
 * 타이틀바 = 윈도우 상단 TITLEBAR_H 픽셀
 */
static int is_titlebar(struct window *w, int px, int py)
{
	return (px >= w->x && px < w->x + w->w &&
		py >= w->y && py < w->y + TITLEBAR_H);
}

/*
 * 닫기 버튼 영역인지 확인
 * 닫기 버튼 = 타이틀바 우측 CLOSE_BTN_W 영역
 */
static int is_close_btn(struct window *w, int px, int py)
{
	return (is_titlebar(w, px, py) &&
		px >= w->x + w->w - CLOSE_BTN_W);
}

/*
 * 최소화 버튼 영역: [X] 왼쪽 두 번째 버튼
 * 타이틀바에서 우측으로 [—][□][X] 순서.
 */
static int is_minimize_btn(struct window *w, int px, int py)
{
	return (is_titlebar(w, px, py) &&
		px >= w->x + w->w - CLOSE_BTN_W * 3 &&
		px <  w->x + w->w - CLOSE_BTN_W * 2);
}

/*
 * 최대화 버튼 영역: [X] 왼쪽 첫 번째 버튼
 */
static int is_maximize_btn(struct window *w, int px, int py)
{
	return (is_titlebar(w, px, py) &&
		px >= w->x + w->w - CLOSE_BTN_W * 2 &&
		px <  w->x + w->w - CLOSE_BTN_W);
}

/*
 * 리사이즈 엣지 감지 (Class 59)
 *
 * 윈도우 가장자리에서 마우스를 잡아 크기 변경:
 *   - 우하단 코너: RESIZE_CORNER x RESIZE_CORNER
 *   - 우측 엣지: RESIZE_EDGE px
 *   - 하단 엣지: RESIZE_EDGE px
 *
 * 반환: 0=없음, 1=우측, 2=하단, 3=코너(우하단)
 */
static int resize_edge_at(struct window *w, int px, int py)
{
	if (w->is_panel || w->maximized)
		return 0; /* 패널/최대화 윈도우는 리사이즈 불가 */

	int right = w->x + w->w;
	int bottom = w->y + w->h;

	/* 우하단 코너 */
	if (px >= right - RESIZE_CORNER && px < right + 2 &&
	    py >= bottom - RESIZE_CORNER && py < bottom + 2)
		return 3;

	/* 우측 엣지 */
	if (px >= right - RESIZE_EDGE && px < right + 2 &&
	    py >= w->y + TITLEBAR_H && py < bottom)
		return 1;

	/* 하단 엣지 */
	if (py >= bottom - RESIZE_EDGE && py < bottom + 2 &&
	    px >= w->x && px < right)
		return 2;

	return 0;
}

/*
 * CDP configure 이벤트 전송 (Class 59)
 *
 * 윈도우 크기 변경 시 클라이언트에 알림.
 * 클라이언트는 새 크기로 버퍼를 재할당해야 함.
 */
static void cdp_send_configure(int win_idx, int width, int height)
{
	struct window *w = &comp.windows[win_idx];

	if (w->cdp_surface_idx < 0)
		return;

	int sidx = w->cdp_surface_idx;

	if (sidx >= MAX_CDP_SURFACES || !cdp.surfaces[sidx].active)
		return;

	int cfd = cdp.clients[cdp.surfaces[sidx].client_idx].fd;

	if (cfd < 0)
		return;

	struct cdp_configure evt;
	evt.surface_id = (uint32_t)(sidx + 1);
	evt.width = width;
	evt.height = height;

	fcntl(cfd, F_SETFL, 0);
	cdp_send_msg(cfd, CDP_EVT_CONFIGURE, &evt, sizeof(evt));
	fcntl(cfd, F_SETFL, O_NONBLOCK);
}

/* ============================================================
 * CDP 서버 기능 — Class 12 추가
 * ============================================================
 *
 * 이 섹션은 컴포지터를 "디스플레이 서버"로 만듭니다.
 *
 * Wayland 컴포지터의 핵심 역할:
 *   1. 소켓에서 클라이언트 연결 받기 (accept)
 *   2. 클라이언트의 요청 처리 (surface 생성, 버퍼 연결 등)
 *   3. 공유메모리에서 픽셀 읽어서 화면에 합성
 *   4. 입력 이벤트를 포커스된 클라이언트에 전달
 *
 * 지금까지 컴포지터는 "그리기 + 입력" 프로그램이었습니다.
 * 이제 "서버" 역할이 추가됩니다!
 */

/*
 * CDP 서버 초기화
 *
 * Unix domain socket 서버 생성 과정:
 *   1. socket(AF_UNIX, SOCK_STREAM) → 소켓 fd 생성
 *   2. bind(path) → 파일시스템 경로에 바인딩
 *   3. listen() → 연결 대기 시작
 *
 * Wayland 대응:
 *   wl_display_create() → wl_display_add_socket()
 *   → $XDG_RUNTIME_DIR/wayland-0 생성
 *
 * 우리는 /tmp/citc-display-0에 생성합니다.
 */
static int cdp_server_init(void)
{
	struct sockaddr_un addr;

	/*
	 * 소켓 활성화 감지 (Class 19)
	 *
	 * LISTEN_FDS 환경변수가 설정되어 있으면,
	 * init(citcinit)이 이미 소켓을 만들어서 fd 3으로 전달한 것.
	 * 이 경우 socket()+bind()+listen()을 건너뛰고
	 * 전달받은 fd를 바로 사용.
	 *
	 * systemd에서 sd_listen_fds()가 하는 것과 동일한 로직.
	 * LISTEN_PID도 확인하여 자신에게 전달된 것인지 검증.
	 */
	char *listen_fds_env = getenv("LISTEN_FDS");
	char *listen_pid_env = getenv("LISTEN_PID");

	int listen_fds_n = listen_fds_env ? atoi(listen_fds_env) : 0;

	if (listen_fds_n > 0 && listen_fds_n <= 10) {
		/* PID 확인 (선택적이지만 안전을 위해) */
		if (listen_pid_env && atoi(listen_pid_env) != getpid()) {
			printf("CDP: LISTEN_PID mismatch (expected %d, got %s)\n",
			       getpid(), listen_pid_env);
			/* PID 불일치해도 fd가 유효할 수 있으므로 계속 시도 */
		}

		/*
		 * fd 3 = init이 전달한 리스닝 소켓.
		 * 이미 bind + listen 상태이므로 바로 사용 가능.
		 */
		cdp.listen_fd = 3;
		fcntl(cdp.listen_fd, F_SETFL, O_NONBLOCK);

		printf("CDP: Socket activation (fd=%d)\n", cdp.listen_fd);
		goto clients_init;
	}

	/* 소켓 활성화가 아니면 직접 소켓 생성 (기존 방식) */
	cdp.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (cdp.listen_fd < 0) {
		printf("CDP: socket create failed: %s\n", strerror(errno));
		return -1;
	}

	/* 이전 소켓 파일 정리 (비정상 종료 시 남아있을 수 있음) */
	unlink(CDP_SOCKET_PATH);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CDP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (bind(cdp.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("CDP: bind failed: %s\n", strerror(errno));
		close(cdp.listen_fd);
		cdp.listen_fd = -1;
		return -1;
	}

	/*
	 * listen(backlog=4):
	 *   최대 4개의 "대기 중인" 연결 허용.
	 *   accept() 전에 쌓일 수 있는 연결 수.
	 */
	if (listen(cdp.listen_fd, 4) < 0) {
		printf("CDP: listen failed: %s\n", strerror(errno));
		close(cdp.listen_fd);
		cdp.listen_fd = -1;
		return -1;
	}

	/* 논블로킹으로 설정 (poll에서 사용) */
	fcntl(cdp.listen_fd, F_SETFL, O_NONBLOCK);

clients_init:

	/* 클라이언트 슬롯 초기화 */
	for (int i = 0; i < MAX_CDP_CLIENTS; i++)
		cdp.clients[i].fd = -1;

	/* surface 슬롯 초기화 */
	for (int i = 0; i < MAX_CDP_SURFACES; i++) {
		cdp.surfaces[i].active = 0;
		cdp.surfaces[i].shm_fd = -1;
	}

	cdp.next_surface_id = 1;  /* ID 0은 사용하지 않음 */

	printf("CDP: 서버 시작 (%s)\n", CDP_SOCKET_PATH);
	return 0;
}

/*
 * CDP 서버 정리
 */
static void cdp_server_cleanup(void)
{
	/* 모든 surface 정리 */
	for (int i = 0; i < MAX_CDP_SURFACES; i++) {
		if (cdp.surfaces[i].active) {
			if (cdp.surfaces[i].shm_map)
				munmap(cdp.surfaces[i].shm_map,
				       cdp.surfaces[i].shm_size);
			if (cdp.surfaces[i].shm_fd >= 0)
				close(cdp.surfaces[i].shm_fd);
		}
	}

	/* 모든 클라이언트 소켓 닫기 */
	for (int i = 0; i < MAX_CDP_CLIENTS; i++) {
		if (cdp.clients[i].fd >= 0)
			close(cdp.clients[i].fd);
	}

	/* 리슨 소켓 닫기 */
	if (cdp.listen_fd >= 0) {
		close(cdp.listen_fd);
		unlink(CDP_SOCKET_PATH);
	}
}

/*
 * 새 클라이언트 수락
 *
 * accept()는 연결 대기 중인 클라이언트를 수락하고
 * 새 소켓 fd를 반환합니다.
 *
 * listen_fd: 모든 연결 요청을 받는 "문지기"
 * accept_fd: 특정 클라이언트와의 1:1 연결
 *
 * Wayland: wl_client_create()
 */
static void cdp_accept_client(void)
{
	int client_fd = accept(cdp.listen_fd, NULL, NULL);

	if (client_fd < 0)
		return;

	/* 빈 슬롯 찾기 */
	int slot = -1;

	for (int i = 0; i < MAX_CDP_CLIENTS; i++) {
		if (cdp.clients[i].fd < 0) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		printf("CDP: 클라이언트 슬롯 없음 (최대 %d)\n",
		       MAX_CDP_CLIENTS);
		close(client_fd);
		return;
	}

	/* 논블로킹 설정 */
	fcntl(client_fd, F_SETFL, O_NONBLOCK);

	cdp.clients[slot].fd = client_fd;

	/* WELCOME 메시지 전송 */
	struct cdp_welcome welcome;

	welcome.screen_width = drm.mode.hdisplay;
	welcome.screen_height = drm.mode.vdisplay;
	welcome.version = CDP_VERSION;

	/*
	 * 논블로킹 소켓으로 blocking write를 하기 위해
	 * 잠시 블로킹 모드로 전환
	 */
	fcntl(client_fd, F_SETFL, 0);
	cdp_send_msg(client_fd, CDP_EVT_WELCOME, &welcome, sizeof(welcome));
	fcntl(client_fd, F_SETFL, O_NONBLOCK);

	printf("CDP: 클라이언트 %d 연결됨 (fd=%d)\n", slot, client_fd);
}

/*
 * surface_id에서 인덱스로 변환 (id = index + 1)
 */
static int cdp_surface_index(uint32_t surface_id)
{
	int idx = (int)surface_id - 1;

	if (idx >= 0 && idx < MAX_CDP_SURFACES && cdp.surfaces[idx].active)
		return idx;
	return -1;
}

/*
 * 클라이언트 메시지 처리
 *
 * 각 메시지 타입에 따라 적절한 동작 수행.
 * Wayland 컴포지터에서 wl_resource_dispatch()에 해당.
 */
/*
 * 클라이언트 완전 정리 — surface, 공유메모리, 소켓, 슬롯 해제.
 *
 * 호출 시점:
 *   1. 클라이언트 disconnect 감지 (read 반환 0/-1)
 *   2. X 버튼으로 윈도우 닫기
 *
 * 이 함수 이후 client_idx 슬롯은 재사용 가능.
 */
static void cdp_disconnect_client(int client_idx)
{
	int client_fd = cdp.clients[client_idx].fd;

	printf("CDP: 클라이언트 %d 정리\n", client_idx);

	/* 이 클라이언트의 모든 surface 정리 */
	for (int i = 0; i < MAX_CDP_SURFACES; i++) {
		if (!cdp.surfaces[i].active ||
		    cdp.surfaces[i].client_idx != client_idx)
			continue;

		/* 윈도우 숨기기 + 슬롯 재사용 가능하도록 정리 */
		int wi = cdp.surfaces[i].window_idx;

		if (wi >= 0 && wi < comp.num_windows) {
			comp.windows[wi].visible = 0;
			comp.windows[wi].cdp_surface_idx = -1;
		}

		/* 공유메모리 해제 */
		if (cdp.surfaces[i].shm_map)
			munmap(cdp.surfaces[i].shm_map,
			       cdp.surfaces[i].shm_size);
		if (cdp.surfaces[i].shm_fd >= 0)
			close(cdp.surfaces[i].shm_fd);

		cdp.surfaces[i].active = 0;
		cdp.surfaces[i].shm_fd = -1;
		cdp.surfaces[i].shm_map = NULL;
	}

	close(client_fd);
	cdp.clients[client_idx].fd = -1;
	damage_add_full();
	comp.need_redraw = 1;
}

static void cdp_handle_client_msg(int client_idx)
{
	int client_fd = cdp.clients[client_idx].fd;
	uint32_t type, size;
	uint8_t payload[CDP_MSG_MAX_PAYLOAD];

	/*
	 * 블로킹 모드로 전환하여 메시지를 완전히 읽기.
	 * 이미 poll()에서 POLLIN을 확인했으므로 바로 읽힘.
	 */
	fcntl(client_fd, F_SETFL, 0);
	int ret = cdp_recv_msg(client_fd, &type, payload,
			       sizeof(payload), &size);
	fcntl(client_fd, F_SETFL, O_NONBLOCK);

	if (ret < 0) {
		cdp_disconnect_client(client_idx);
		return;
	}

	switch (type) {
	case CDP_REQ_CREATE_SURFACE: {
		/*
		 * Surface 생성 — Wayland: wl_compositor.create_surface
		 *
		 * 1. 빈 surface 슬롯 찾기
		 * 2. 새 윈도우 생성 (window_create)
		 * 3. surface ↔ 윈도우 연결
		 * 4. SURFACE_ID 응답 전송
		 */
		struct cdp_create_surface *req =
			(struct cdp_create_surface *)payload;

		/* 빈 surface 슬롯 */
		int slot = -1;

		for (int i = 0; i < MAX_CDP_SURFACES; i++) {
			if (!cdp.surfaces[i].active) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			printf("CDP: surface 슬롯 없음\n");
			break;
		}

		/* 윈도우 크기에 타이틀바 높이 추가 */
		int win_w = req->width;
		int win_h = req->height + TITLEBAR_H;
		int win_idx = window_create(req->x, req->y,
					    win_w, win_h,
					    "CDP Client",
					    80, 160, 220);
		if (win_idx < 0) {
			printf("CDP: 윈도우 생성 실패\n");
			break;
		}

		/* surface 초기화 */
		struct cdp_surface *surf = &cdp.surfaces[slot];

		memset(surf, 0, sizeof(*surf));
		surf->active = 1;
		surf->window_idx = win_idx;
		surf->client_idx = client_idx;
		surf->shm_fd = -1;
		surf->committed = 0;
		surf->frame_requested = 0;

		/* 윈도우에 surface 인덱스 연결 */
		comp.windows[win_idx].cdp_surface_idx = slot;

		uint32_t surface_id = (uint32_t)(slot + 1);

		/* 응답 전송 */
		struct cdp_surface_id resp;

		resp.surface_id = surface_id;
		fcntl(client_fd, F_SETFL, 0);
		cdp_send_msg(client_fd, CDP_EVT_SURFACE_ID,
			     &resp, sizeof(resp));
		fcntl(client_fd, F_SETFL, O_NONBLOCK);

		printf("CDP: surface %u 생성 (client=%d, window=%d, %dx%d)\n",
		       surface_id, client_idx, win_idx,
		       req->width, req->height);
		damage_add_full();
		comp.need_redraw = 1;
		break;
	}

	case CDP_REQ_ATTACH_BUFFER: {
		/*
		 * 공유메모리 버퍼 연결
		 * Wayland: wl_surface.attach(wl_buffer from wl_shm_pool)
		 *
		 * 이 메시지 바로 다음에 SCM_RIGHTS로 fd가 옴!
		 * → cdp_recv_fd()로 별도 수신
		 */
		struct cdp_attach_buffer *req =
			(struct cdp_attach_buffer *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx < 0)
			break;

		struct cdp_surface *surf = &cdp.surfaces[sidx];

		/* 이전 버퍼 해제 */
		if (surf->shm_map) {
			munmap(surf->shm_map, surf->shm_size);
			surf->shm_map = NULL;
		}
		if (surf->shm_fd >= 0) {
			close(surf->shm_fd);
			surf->shm_fd = -1;
		}

		/* fd 수신 (SCM_RIGHTS) — 핵심!! */
		fcntl(client_fd, F_SETFL, 0);
		int shm_fd = cdp_recv_fd(client_fd);

		fcntl(client_fd, F_SETFL, O_NONBLOCK);

		if (shm_fd < 0) {
			printf("CDP: fd 수신 실패\n");
			break;
		}

		/* 서버 측에서 같은 메모리를 mmap */
		uint32_t shm_size = req->stride * req->height;
		uint8_t *shm_map = mmap(NULL, shm_size,
					PROT_READ,  /* 서버는 읽기만! */
					MAP_SHARED, shm_fd, 0);

		if (shm_map == MAP_FAILED) {
			printf("CDP: mmap 실패: %s\n", strerror(errno));
			close(shm_fd);
			break;
		}

		surf->shm_fd = shm_fd;
		surf->shm_map = shm_map;
		surf->shm_size = shm_size;
		surf->buf_width = req->width;
		surf->buf_height = req->height;
		surf->buf_stride = req->stride;
		surf->format = req->format;  /* 0=XRGB, 1=ARGB (Class 60) */

		printf("CDP: surface %u 버퍼 연결 (%ux%u, fmt=%u, %u bytes)\n",
		       req->surface_id, req->width, req->height, req->format,
		       shm_size);
		break;
	}

	case CDP_REQ_COMMIT: {
		/*
		 * 화면 갱신 요청
		 * Wayland: wl_surface.commit()
		 *
		 * 클라이언트가 "그리기 완료!"를 알림.
		 * committed 플래그를 설정하여 다음 렌더링에 반영.
		 */
		struct cdp_commit *req = (struct cdp_commit *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx < 0)
			break;

		cdp.surfaces[sidx].committed = 1;
		damage_add_window(cdp.surfaces[sidx].window_idx);
		comp.need_redraw = 1;
		break;
	}

	case CDP_REQ_FRAME: {
		/*
		 * 프레임 콜백 요청
		 * Wayland: wl_surface.frame() → wl_callback
		 *
		 * 다음 render_frame() 후에
		 * CDP_EVT_FRAME_DONE을 보내줄 것을 약속.
		 */
		struct cdp_frame_req *req = (struct cdp_frame_req *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx < 0)
			break;

		cdp.surfaces[sidx].frame_requested = 1;
		break;
	}

	case CDP_REQ_SET_TITLE: {
		struct cdp_set_title *req = (struct cdp_set_title *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx < 0)
			break;

		struct cdp_surface *surf = &cdp.surfaces[sidx];
		int wi = surf->window_idx;

		if (wi >= 0 && wi < comp.num_windows) {
			memcpy(comp.windows[wi].title, req->title,
			       sizeof(comp.windows[wi].title) - 1);
			comp.windows[wi].title[sizeof(comp.windows[wi].title) - 1] = '\0';
			damage_add_window(wi);
			comp.need_redraw = 1;
		}
		break;
	}

	case CDP_REQ_SET_PANEL: {
		/*
		 * 패널 surface 설정 (Class 17 추가)
		 * Wayland: wlr-layer-shell (zwlr_layer_surface_v1)
		 *
		 * 일반 윈도우를 패널(태스크바)로 전환:
		 *   1. is_panel 플래그 설정
		 *   2. 화면 하단에 고정 (y = screen_height - panel_height)
		 *   3. 전체 너비로 확장 (w = screen_width)
		 *   4. 타이틀바 높이를 제거 (패널은 타이틀바 없음)
		 *
		 * 패널은 render_frame()에서 일반 윈도우 뒤에 그려져
		 * 항상 최상위에 표시됩니다.
		 */
		struct cdp_set_panel *req = (struct cdp_set_panel *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx < 0)
			break;

		struct cdp_surface *surf = &cdp.surfaces[sidx];
		int wi = surf->window_idx;

		if (wi >= 0 && wi < comp.num_windows) {
			struct window *w = &comp.windows[wi];

			w->is_panel = 1;

			/*
			 * 패널 크기/위치 재조정:
			 *   - 전체 너비
			 *   - 요청한 높이 (타이틀바 제거)
			 *   - 화면 하단에 고정
			 */
			w->w = (int)drm.mode.hdisplay;
			w->h = (int)req->height;
			w->x = 0;
			w->y = (int)drm.mode.vdisplay - w->h;

			printf("CDP: surface %u → panel (edge=%u, %dx%d at y=%d)\n",
			       req->surface_id, req->edge,
			       w->w, w->h, w->y);
			damage_add_full();
			comp.need_redraw = 1;
		}
		break;
	}

	case CDP_REQ_DESTROY_SURFACE: {
		struct cdp_destroy_surface *req =
			(struct cdp_destroy_surface *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx < 0)
			break;

		struct cdp_surface *surf = &cdp.surfaces[sidx];
		int wi = surf->window_idx;

		if (wi >= 0 && wi < comp.num_windows)
			comp.windows[wi].visible = 0;

		if (surf->shm_map)
			munmap(surf->shm_map, surf->shm_size);
		if (surf->shm_fd >= 0)
			close(surf->shm_fd);

		surf->active = 0;
		surf->shm_fd = -1;
		surf->shm_map = NULL;

		printf("CDP: surface %u 삭제됨\n", req->surface_id);
		damage_add_full();
		comp.need_redraw = 1;
		break;
	}

	case CDP_REQ_DAMAGE: {
		struct cdp_damage *req = (struct cdp_damage *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx >= 0) {
			int wi = cdp.surfaces[sidx].window_idx;

			if (wi >= 0 && wi < comp.num_windows) {
				struct window *w = &comp.windows[wi];
				/* 클라이언트가 보고한 영역을 화면 좌표로 변환 */
				damage_add(w->x + (int)req->x,
					   w->y + TITLEBAR_H + (int)req->y,
					   (int)req->w, (int)req->h);
			}
		}
		comp.need_redraw = 1;
		break;
	}

	case CDP_REQ_SET_MODE: {
		/* 해상도 변경 — 현재는 로그만 (DRM 모드셋은 부팅 시 고정) */
		struct cdp_set_mode *req = (struct cdp_set_mode *)payload;

		printf("CDP: SET_MODE 요청 %ux%u@%uHz (현재 미지원)\n",
		       req->width, req->height, req->refresh);
		break;
	}

	case CDP_REQ_LIST_WINDOWS: {
		/* 열린 윈도우 목록 전송 */
		struct cdp_window_list resp;

		memset(&resp, 0, sizeof(resp));
		int cnt = 0;

		for (int i = 0; i < comp.num_windows && cnt < CDP_MAX_WINLIST; i++) {
			struct window *w = &comp.windows[i];

			if (w->is_panel)
				continue; /* 패널 제외 */

			resp.entries[cnt].surface_id =
				(w->cdp_surface_idx >= 0) ?
				(uint32_t)(w->cdp_surface_idx + 1) : 0;
			strncpy(resp.entries[cnt].title, w->title,
				sizeof(resp.entries[cnt].title) - 1);
			resp.entries[cnt].minimized = w->minimized;
			cnt++;
		}
		resp.count = (uint32_t)cnt;

		uint32_t msg_size = sizeof(uint32_t) +
				    (uint32_t)cnt * sizeof(struct cdp_window_entry);

		cdp_send_msg(client_fd, CDP_EVT_WINDOW_LIST,
			     &resp, msg_size);
		break;
	}

	case CDP_REQ_RAISE_SURFACE: {
		struct cdp_raise_surface *req =
			(struct cdp_raise_surface *)payload;
		int sidx = cdp_surface_index(req->surface_id);

		if (sidx >= 0) {
			int wi = cdp.surfaces[sidx].window_idx;

			if (wi >= 0 && wi < comp.num_windows) {
				struct window *w = &comp.windows[wi];

				/* 최소화 해제 */
				if (w->minimized)
					w->minimized = 0;
				w->visible = 1;

				/* Z-order: 맨 위로 올리기 */
				if (wi != comp.num_windows - 1) {
					struct window tmp = *w;
					/* 윈도우를 배열 끝으로 이동 */
					for (int j = wi; j < comp.num_windows - 1; j++)
						comp.windows[j] = comp.windows[j + 1];
					comp.windows[comp.num_windows - 1] = tmp;
					/* cdp_surface의 window_idx 갱신 */
					for (int j = 0; j < MAX_CDP_SURFACES; j++) {
						if (cdp.surfaces[j].active &&
						    cdp.surfaces[j].window_idx > wi)
							cdp.surfaces[j].window_idx--;
					}
					if (tmp.cdp_surface_idx >= 0)
						cdp.surfaces[tmp.cdp_surface_idx].window_idx =
							comp.num_windows - 1;
				}
				comp.focused = comp.num_windows - 1;
				comp.need_redraw = 1;
				damage_add_full();
			}
		}
		break;
	}

	case CDP_REQ_CLIPBOARD_SET: {
		struct cdp_clipboard_set *req =
			(struct cdp_clipboard_set *)payload;

		if (req->len > 0 && req->len <= CDP_CLIPBOARD_MAX) {
			memcpy(cdp.clipboard_buf, req->text, req->len);
			cdp.clipboard_len = req->len;
			if (req->len < CDP_CLIPBOARD_MAX)
				cdp.clipboard_buf[req->len] = '\0';
		}
		break;
	}

	case CDP_REQ_CLIPBOARD_GET: {
		/* 요청 클라이언트에 클립보드 데이터 전송 */
		struct cdp_clipboard_data resp;

		resp.len = cdp.clipboard_len;
		if (cdp.clipboard_len > 0)
			memcpy(resp.text, cdp.clipboard_buf,
			       cdp.clipboard_len);
		if (cdp.clipboard_len < CDP_CLIPBOARD_MAX)
			resp.text[cdp.clipboard_len] = '\0';

		uint32_t msg_size = sizeof(uint32_t) +
				    cdp.clipboard_len + 1;

		cdp_send_msg(client_fd, CDP_EVT_CLIPBOARD_DATA,
			     &resp, msg_size);
		break;
	}

	default:
		printf("CDP: 알 수 없는 요청 type=%u\n", type);
		break;
	}
}

/*
 * CDP 입력 이벤트를 클라이언트에 전달
 *
 * Wayland의 핵심 보안 특성:
 *   X11에서는 모든 앱이 모든 입력을 볼 수 있었음 (보안 위험!)
 *   Wayland에서는 컴포지터가 포커스된 앱에만 입력을 전달.
 *   다른 앱은 절대 볼 수 없음 → 키로거 방지!
 *
 * 우리 구현:
 *   comp.focused 윈도우에 CDP surface가 연결되어 있으면
 *   해당 클라이언트에게만 이벤트를 보냄.
 */
static void cdp_route_key(uint32_t keycode, uint32_t state, char character)
{
	if (comp.focused < 0 || comp.focused >= comp.num_windows)
		return;

	struct window *win = &comp.windows[comp.focused];

	if (win->cdp_surface_idx < 0)
		return; /* 내부 윈도우 → CDP 라우팅 불필요 */

	int sidx = win->cdp_surface_idx;

	if (sidx < 0 || sidx >= MAX_CDP_SURFACES || !cdp.surfaces[sidx].active)
		return;

	struct cdp_surface *surf = &cdp.surfaces[sidx];
	int client_fd = cdp.clients[surf->client_idx].fd;

	if (client_fd < 0)
		return;

	struct cdp_key evt;

	evt.keycode = keycode;
	evt.state = state;
	evt.character = (uint32_t)character;
	evt.modifiers = (shift_held ? CDP_MOD_SHIFT : 0) |
			(ctrl_held ? CDP_MOD_CTRL : 0);

	fcntl(client_fd, F_SETFL, 0);
	cdp_send_msg(client_fd, CDP_EVT_KEY, &evt, sizeof(evt));
	fcntl(client_fd, F_SETFL, O_NONBLOCK);
}

static void cdp_route_pointer_motion(int surface_x, int surface_y)
{
	if (comp.focused < 0 || comp.focused >= comp.num_windows)
		return;

	struct window *win = &comp.windows[comp.focused];

	if (win->cdp_surface_idx < 0)
		return;

	int sidx = win->cdp_surface_idx;

	if (sidx < 0 || sidx >= MAX_CDP_SURFACES || !cdp.surfaces[sidx].active)
		return;

	struct cdp_surface *surf = &cdp.surfaces[sidx];
	int client_fd = cdp.clients[surf->client_idx].fd;

	if (client_fd < 0)
		return;

	struct cdp_pointer_motion evt;

	evt.surface_id = (uint32_t)(sidx + 1);
	evt.x = surface_x;
	evt.y = surface_y;

	fcntl(client_fd, F_SETFL, 0);
	cdp_send_msg(client_fd, CDP_EVT_POINTER_MOTION, &evt, sizeof(evt));
	fcntl(client_fd, F_SETFL, O_NONBLOCK);
}

static void cdp_route_pointer_button(uint32_t button, uint32_t state)
{
	if (comp.focused < 0 || comp.focused >= comp.num_windows)
		return;

	struct window *win = &comp.windows[comp.focused];

	if (win->cdp_surface_idx < 0)
		return;

	int sidx = win->cdp_surface_idx;

	if (sidx < 0 || sidx >= MAX_CDP_SURFACES || !cdp.surfaces[sidx].active)
		return;

	struct cdp_surface *surf = &cdp.surfaces[sidx];
	int client_fd = cdp.clients[surf->client_idx].fd;

	if (client_fd < 0)
		return;

	struct cdp_pointer_button evt;

	evt.surface_id = (uint32_t)(sidx + 1);
	evt.button = button;
	evt.state = state;

	fcntl(client_fd, F_SETFL, 0);
	cdp_send_msg(client_fd, CDP_EVT_POINTER_BUTTON, &evt, sizeof(evt));
	fcntl(client_fd, F_SETFL, O_NONBLOCK);
}

/*
 * 프레임 콜백 전송
 *
 * render_frame() 후 호출.
 * frame_requested된 모든 surface에 FRAME_DONE을 보냄.
 *
 * Wayland: wl_callback.done (frame callback)
 *
 * 이 메커니즘의 효과:
 *   1. 보이지 않는 앱은 frame을 요청하지 않음 → 렌더링 안 함 → 전력 절약
 *   2. 60Hz 모니터면 최대 60fps → 불필요한 GPU 사용 방지
 *   3. 컴포지터가 "다음 vblank 때 준비하세요"라고 알려줌 → 타이밍 동기화
 */
static void cdp_send_frame_callbacks(void)
{
	for (int i = 0; i < MAX_CDP_SURFACES; i++) {
		if (!cdp.surfaces[i].active || !cdp.surfaces[i].frame_requested)
			continue;

		int cidx = cdp.surfaces[i].client_idx;

		if (cidx < 0 || cdp.clients[cidx].fd < 0)
			continue;

		struct cdp_frame_done evt;

		evt.surface_id = (uint32_t)(i + 1);

		int client_fd = cdp.clients[cidx].fd;

		fcntl(client_fd, F_SETFL, 0);
		cdp_send_msg(client_fd, CDP_EVT_FRAME_DONE,
			     &evt, sizeof(evt));
		fcntl(client_fd, F_SETFL, O_NONBLOCK);

		cdp.surfaces[i].frame_requested = 0;
	}
}

/* ============================================================
 * 렌더링
 * ============================================================
 *
 * 컴포지팅 렌더링 순서 (Painter's Algorithm):
 *
 *   1. 배경 (가장 아래)
 *   2. 윈도우들 (앞에서 뒤로, 즉 인덱스 0 → N)
 *   3. 마우스 커서 (가장 위)
 *
 * "Painter's Algorithm"이란?
 *   화가가 그림을 그리듯, 먼 것(배경)부터 그리고
 *   가까운 것(커서)을 나중에 덧그리는 방식.
 *   나중에 그린 것이 이전 것을 덮음.
 *
 * 실제 Wayland 컴포지터에서는 알파 블렌딩(투명도 합성)을 하지만,
 * 여기서는 간단히 불투명 덧그리기만 합니다.
 */

/*
 * 배경 캐시 생성 (Class 58 추가)
 *
 * 그래디언트를 매 프레임 계산하면 느림.
 * 한 번만 계산하고 캐시하면 memcpy로 빠르게 복사 가능.
 */

/*
 * 알파 블렌딩 (Class 60)
 *
 * ARGB 픽셀의 알파 채널을 이용하여 반투명 합성.
 * Porter-Duff "source over" 연산:
 *   result = src * src_alpha + dst * (1 - src_alpha)
 *
 * 최적화:
 *   sa == 0xFF → 불투명: dst = src (memcpy와 동일)
 *   sa == 0x00 → 완전 투명: dst 유지
 *   그 외 → 채널별 블렌딩
 */
static inline uint32_t alpha_blend(uint32_t dst, uint32_t src)
{
	uint32_t sa = (src >> 24) & 0xFF;

	if (sa == 0xFF) return src;     /* 불투명 — 빠른 경로 */
	if (sa == 0x00) return dst;     /* 완전 투명 */

	uint32_t da = 255 - sa;
	uint32_t r = ((((src >> 16) & 0xFF) * sa) +
		      (((dst >> 16) & 0xFF) * da)) / 255;
	uint32_t g = ((((src >> 8) & 0xFF) * sa) +
		      (((dst >> 8) & 0xFF) * da)) / 255;
	uint32_t b = (((src & 0xFF) * sa) +
		      ((dst & 0xFF) * da)) / 255;

	return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

/*
 * 반투명 사각형 그리기 (Class 60)
 * 윈도우 그림자에 사용.
 */
static void draw_rect_alpha(struct drm_buf *buf, int x, int y,
			    int w, int h, uint32_t argb)
{
	for (int dy = 0; dy < h; dy++) {
		int py = y + dy;

		if (py < 0 || (uint32_t)py >= buf->height)
			continue;

		uint32_t *row = (uint32_t *)(buf->map + (uint32_t)py * buf->pitch);

		for (int dx = 0; dx < w; dx++) {
			int px = x + dx;

			if (px < 0 || (uint32_t)px >= buf->width)
				continue;
			row[px] = alpha_blend(row[px], argb);
		}
	}
}

/*
 * 배경 이미지 로드 시도 (Class 61)
 *
 * /usr/share/wallpaper.raw — XRGB8888 원시 데이터
 * 크기가 화면과 정확히 맞아야 함 (width × height × 4 bytes).
 * 없으면 그래디언트 fallback.
 */
#define WALLPAPER_PATH "/usr/share/wallpaper.raw"

static void render_background_cache(struct drm_buf *buf)
{
	if (!comp.bg_cache) {
		comp.bg_cache = malloc(buf->width * buf->height * 4);
		if (!comp.bg_cache) return;
	}

	/* 배경 이미지 시도 */
	uint32_t expected = buf->width * buf->height * 4;
	int wfd = open(WALLPAPER_PATH, O_RDONLY);

	if (wfd >= 0) {
		ssize_t rd = read(wfd, comp.bg_cache, expected);
		close(wfd);
		if ((uint32_t)rd == expected) {
			comp.bg_cache_valid = 1;
			printf("compositor: wallpaper loaded (%ux%u)\n",
			       buf->width, buf->height);
			return;
		}
	}

	/* 그래디언트 fallback */
	for (uint32_t y = 0; y < buf->height; y++) {
		uint32_t *line = comp.bg_cache + y * buf->width;
		uint8_t r = 20 + 15 * y / buf->height;
		uint8_t g = 25 + 20 * y / buf->height;
		uint8_t b_val = 50 + 30 * y / buf->height;
		uint32_t color = rgb(r, g, b_val);
		for (uint32_t x = 0; x < buf->width; x++)
			line[x] = color;
	}
	comp.bg_cache_valid = 1;
}

/* 배경: 캐시에서 복사 + 상단 바 */
static void render_background(struct drm_buf *buf)
{
	/* 캐시가 없으면 생성 */
	if (!comp.bg_cache_valid)
		render_background_cache(buf);

	/* 캐시에서 프레임버퍼로 복사 */
	if (comp.bg_cache) {
		for (uint32_t y = 0; y < buf->height; y++) {
			uint32_t *dst = (uint32_t *)(buf->map + y * buf->pitch);
			uint32_t *src = comp.bg_cache + y * buf->width;
			memcpy(dst, src, buf->width * 4);
		}
	}

	/* 상단 바 (태스크바 느낌) */
	int bar_h = font_height() + 4;
	draw_rect(buf, 0, 0, buf->width, bar_h, rgb(30, 30, 50));
	draw_string(buf, 8, 2, "CITC OS Compositor", rgb(180, 180, 220), 1);

	/* 우측에 시스템 정보 */
	char info[64];
	snprintf(info, sizeof(info), "Windows: %d  Mouse: %d,%d",
		 comp.num_windows, comp.mouse_x, comp.mouse_y);
	int info_x = (int)(buf->width - strlen(info) * (uint32_t)font_width() - 8);
	draw_string(buf, info_x, 2, info, rgb(120, 120, 160), 1);
}

/* 윈도우 하나 렌더링 */
static void render_window(struct drm_buf *buf, struct window *win, int focused)
{
	if (!win->visible)
		return;

	/*
	 * 패널 윈도우: 타이틀바/테두리 없이 직접 blit (Class 17 추가)
	 *
	 * 패널은 클라이언트가 전체 영역을 직접 그림.
	 * 일반 윈도우처럼 타이틀바, 테두리, 닫기 버튼이 없음.
	 * Wayland의 layer-shell surface와 동일한 개념.
	 */
	if (win->is_panel) {
		if (win->cdp_surface_idx >= 0) {
			int sidx = win->cdp_surface_idx;

			if (sidx < MAX_CDP_SURFACES &&
			    cdp.surfaces[sidx].active &&
			    cdp.surfaces[sidx].committed &&
			    cdp.surfaces[sidx].shm_map) {
				struct cdp_surface *surf = &cdp.surfaces[sidx];

				for (uint32_t sy = 0; sy < surf->buf_height; sy++) {
					int dst_y = win->y + (int)sy;

					if (dst_y < 0 || (uint32_t)dst_y >= buf->height)
						continue;
					if (dst_y >= win->y + win->h)
						break;

					uint32_t *src = (uint32_t *)
						(surf->shm_map + sy * surf->buf_stride);
					uint32_t *dst = (uint32_t *)
						(buf->map + (uint32_t)dst_y * buf->pitch);

					int use_alpha = (surf->format == 1);

					for (uint32_t sx = 0; sx < surf->buf_width; sx++) {
						int dst_x = win->x + (int)sx;

						if (dst_x < 0 || (uint32_t)dst_x >= buf->width)
							continue;
						if (dst_x >= win->x + win->w)
							break;

						if (use_alpha)
							dst[dst_x] = alpha_blend(dst[dst_x], src[sx]);
						else
							dst[dst_x] = src[sx];
					}
				}
			}
		}
		return; /* 패널은 여기서 끝 — 타이틀바/테두리 없음 */
	}

	/* === 이하 일반 윈도우 렌더링 === */

	uint32_t title_color;
	uint32_t border_color;

	if (focused) {
		title_color = rgb(win->color_r, win->color_g, win->color_b);
		border_color = rgb(100, 150, 255);
	} else {
		/* 비활성 윈도우: 어둡게 */
		title_color = rgb(win->color_r / 2,
				  win->color_g / 2,
				  win->color_b / 2);
		border_color = rgb(60, 60, 80);
	}

	/* 윈도우 그림자 (Class 60) — 오프셋 (4,4) 반투명 검정 */
	draw_rect_alpha(buf, win->x + 4, win->y + 4,
			win->w, win->h, 0x40000000);

	/* 테두리 (2px) */
	draw_rect(buf, win->x - 2, win->y - 2,
		  win->w + 4, win->h + 4, border_color);

	/* 타이틀바 */
	draw_rect(buf, win->x, win->y,
		  win->w, TITLEBAR_H, title_color);

	/* 타이틀 텍스트 */
	draw_string(buf, win->x + 6, win->y + 6,
		    win->title, rgb(255, 255, 255), 1);

	/* 타이틀바 버튼: [—][□][X] (Class 59) */
	int close_x = win->x + win->w - CLOSE_BTN_W;
	int max_x   = close_x - CLOSE_BTN_W;
	int min_x   = max_x - CLOSE_BTN_W;

	/* 닫기 버튼 [X] — 빨간색 */
	draw_rect(buf, close_x, win->y, CLOSE_BTN_W, TITLEBAR_H,
		  rgb(200, 60, 60));
	draw_char(buf, close_x + 6, win->y + 6, 'X',
		  rgb(255, 255, 255), 1);

	/* 최대화 버튼 [□] — 회색 */
	draw_rect(buf, max_x, win->y, CLOSE_BTN_W, TITLEBAR_H,
		  rgb(80, 80, 100));
	draw_char(buf, max_x + 6, win->y + 6,
		  win->maximized ? 'R' : '#',
		  rgb(255, 255, 255), 1);

	/* 최소화 버튼 [—] — 회색 */
	draw_rect(buf, min_x, win->y, CLOSE_BTN_W, TITLEBAR_H,
		  rgb(80, 80, 100));
	draw_char(buf, min_x + 6, win->y + 6, '-',
		  rgb(255, 255, 255), 1);

	/* 클라이언트 영역 (내용 부분) */
	int client_y = win->y + TITLEBAR_H;
	int client_h = win->h - TITLEBAR_H;

	draw_rect(buf, win->x, client_y,
		  win->w, client_h,
		  rgb(25, 25, 35));

	/*
	 * CDP surface가 연결된 윈도우인가?
	 *
	 * 내부 윈도우: draw_char()로 텍스트 표시 (기존 방식)
	 * CDP surface: 공유메모리에서 픽셀을 직접 복사! (Class 12 추가)
	 *
	 * 이것이 컴포지팅의 핵심!
	 *   클라이언트가 자기 버퍼에 그린 픽셀을
	 *   컴포지터가 화면의 정확한 위치에 복사합니다.
	 *
	 *   Wayland에서는 GPU가 이 작업 수행 (GL compositing / DMA-BUF).
	 *   우리는 CPU로 복사 (memcpy 기반 blit). 느리지만 개념은 동일!
	 */
	if (win->cdp_surface_idx >= 0) {
		/* CDP surface — 공유메모리에서 픽셀 복사 */
		int sidx = win->cdp_surface_idx;

		if (sidx < MAX_CDP_SURFACES &&
		    cdp.surfaces[sidx].active &&
		    cdp.surfaces[sidx].committed &&
		    cdp.surfaces[sidx].shm_map) {

			struct cdp_surface *surf = &cdp.surfaces[sidx];

			/*
			 * Blit (Block Image Transfer):
			 *   공유메모리의 각 행을 DRM 프레임버퍼에 복사.
			 *   클리핑 처리: 윈도우/화면 밖으로 나가면 잘라냄.
			 */
			for (uint32_t sy = 0; sy < surf->buf_height; sy++) {
				int dst_y = client_y + (int)sy;

				if (dst_y < 0 || (uint32_t)dst_y >= buf->height)
					continue;
				if (dst_y >= win->y + win->h)
					break;

				/* 소스: 공유메모리 한 줄 */
				uint32_t *src = (uint32_t *)
					(surf->shm_map + sy * surf->buf_stride);
				/* 목적지: DRM 프레임버퍼 한 줄 */
				uint32_t *dst = (uint32_t *)
					(buf->map + (uint32_t)dst_y * buf->pitch);

				int use_alpha = (surf->format == 1);

				for (uint32_t sx = 0; sx < surf->buf_width; sx++) {
					int dst_x = win->x + (int)sx;

					if (dst_x < 0 || (uint32_t)dst_x >= buf->width)
						continue;
					if (dst_x >= win->x + win->w)
						break;

					if (use_alpha)
						dst[dst_x] = alpha_blend(dst[dst_x], src[sx]);
					else
						dst[dst_x] = src[sx];
				}
			}
		}
	} else {
		/* 내부 윈도우 — 텍스트 표시 (기존 방식) */
		if (win->text_len > 0) {
			int chars_per_line = (win->w - 12) / 8;

			if (chars_per_line < 1)
				chars_per_line = 1;

			int tx = win->x + 6;
			int ty = client_y + 6;

			for (int i = 0; i < win->text_len; i++) {
				if (i > 0 && i % chars_per_line == 0) {
					tx = win->x + 6;
					ty += 12;
				}
				if (ty + 10 > win->y + win->h)
					break;
				draw_char(buf, tx, ty, win->text[i],
					  rgb(200, 200, 200), 1);
				tx += 8;
			}

			if (focused)
				draw_char(buf, tx, ty, '_',
					  rgb(255, 255, 100), 1);
		} else if (focused) {
			draw_char(buf, win->x + 6, client_y + 6,
				  '_', rgb(255, 255, 100), 1);
		}
	}
}

/*
 * 마우스 커서 렌더링
 *
 * 간단한 화살표 모양을 비트맵으로 정의.
 * 실제 OS에서는 하드웨어 커서(DRM cursor plane)를 사용하지만,
 * 여기서는 소프트웨어 렌더링으로 합니다.
 *
 * 하드웨어 커서 vs 소프트웨어 커서:
 *   하드웨어: GPU가 자동으로 커서를 합성. 티어링 없음.
 *   소프트웨어: 프레임마다 CPU가 커서를 그림. 간단하지만 느림.
 */
static const uint8_t cursor_bitmap[CURSOR_SIZE][CURSOR_SIZE] = {
	{1,0,0,0,0,0,0,0,0,0,0,0},
	{1,1,0,0,0,0,0,0,0,0,0,0},
	{1,2,1,0,0,0,0,0,0,0,0,0},
	{1,2,2,1,0,0,0,0,0,0,0,0},
	{1,2,2,2,1,0,0,0,0,0,0,0},
	{1,2,2,2,2,1,0,0,0,0,0,0},
	{1,2,2,2,2,2,1,0,0,0,0,0},
	{1,2,2,2,2,2,2,1,0,0,0,0},
	{1,2,2,2,2,1,1,1,1,0,0,0},
	{1,2,2,1,2,1,0,0,0,0,0,0},
	{1,1,0,0,1,2,1,0,0,0,0,0},
	{0,0,0,0,0,1,1,0,0,0,0,0},
};

static void render_cursor(struct drm_buf *buf)
{
	for (int y = 0; y < CURSOR_SIZE; y++) {
		for (int x = 0; x < CURSOR_SIZE; x++) {
			uint8_t v = cursor_bitmap[y][x];
			if (v == 1)
				draw_pixel(buf, comp.mouse_x + x,
					   comp.mouse_y + y, rgb(0, 0, 0));
			else if (v == 2)
				draw_pixel(buf, comp.mouse_x + x,
					   comp.mouse_y + y,
					   rgb(255, 255, 255));
		}
	}
}

/*
 * 전체 프레임 렌더링
 *
 * Class 58 변경: 데미지 기반 렌더링.
 *   - damage_has_any() = 0: 렌더링 건너뛰기 (idle 절전)
 *   - damage_full = 1: 전체 리드로 (기존 방식)
 *   - 부분 데미지: 전체 리드로 (추후 최적화 가능)
 *
 * 현재 구현은 "skip idle frames" 최적화에 집중.
 * 변경이 없으면 render_frame 자체를 호출하지 않음.
 */
static void render_frame(void)
{
	struct drm_buf *buf = back_buf();

	/* 1. 배경 */
	render_background(buf);

	/*
	 * 2. 윈도우들 (뒤→앞 순서)
	 *
	 * Class 17 변경: 패널 제외하고 일반 윈도우만 먼저 그림.
	 * 패널은 3단계에서 항상 일반 윈도우 위에 그립니다.
	 *
	 * 레이어 순서 (Wayland layer-shell 개념):
	 *   BACKGROUND: 배경 (render_background)
	 *   BOTTOM:     일반 윈도우들
	 *   TOP:        패널 (태스크바) ← Class 17 추가
	 *   OVERLAY:    마우스 커서
	 */
	for (int i = 0; i < comp.num_windows; i++) {
		if (comp.windows[i].is_panel)
			continue; /* 패널은 나중에 */
		int is_focused = (i == comp.focused);
		render_window(buf, &comp.windows[i], is_focused);
	}

	/* 3. 패널 윈도우 (항상 일반 윈도우 위) — Class 17 추가 */
	for (int i = 0; i < comp.num_windows; i++) {
		if (!comp.windows[i].is_panel)
			continue;
		render_window(buf, &comp.windows[i], 0);
	}

	/* 4. 마우스 커서 (항상 최상위) */
	render_cursor(buf);

	/* 5. 버퍼 스왑 */
	drm_swap();

	/* 6. CDP 프레임 콜백 전송 (Class 12 추가) */
	cdp_send_frame_callbacks();

	/* 7. 데미지 리셋 — 다음 프레임까지 다시 축적 */
	damage_reset();
}

/* ============================================================
 * 이벤트 처리
 * ============================================================
 *
 * 이벤트 기반 프로그래밍:
 *   전통적인 프로그램: 순차 실행 (위에서 아래로)
 *   이벤트 기반: "이벤트가 올 때까지 대기 → 처리 → 반복"
 *
 *   모든 GUI 프로그램은 이벤트 기반입니다.
 *   Windows의 GetMessage/DispatchMessage,
 *   macOS의 NSRunLoop,
 *   Linux의 poll/epoll이 같은 패턴.
 *
 * poll() vs epoll:
 *   poll(): 간단하고 이식성 좋음. 소수의 fd에 적합.
 *   epoll(): Linux 전용. 수천 개의 fd에 효율적.
 *   여기서는 입력 장치가 몇 개뿐이므로 poll() 사용.
 */

/*
 * 마우스 이벤트 처리
 *
 * dev 파라미터가 필요한 이유:
 *   절대좌표(태블릿) 장치는 abs_max_x/y를 알아야
 *   화면 좌표로 변환할 수 있기 때문.
 */
static void handle_mouse_event(struct input_dev *dev,
			       struct input_event *ev)
{
	if (ev->type == EV_REL) {
		/*
		 * 상대 이동 이벤트 (일반 마우스)
		 * REL_X: 수평 이동 (양수=오른쪽, 음수=왼쪽)
		 * REL_Y: 수직 이동 (양수=아래, 음수=위)
		 */
		if (ev->code == REL_X) {
			comp.mouse_x += ev->value;
			if (comp.mouse_x < 0) comp.mouse_x = 0;
			if ((uint32_t)comp.mouse_x >= drm.mode.hdisplay)
				comp.mouse_x = drm.mode.hdisplay - 1;
		} else if (ev->code == REL_Y) {
			comp.mouse_y += ev->value;
			if (comp.mouse_y < 0) comp.mouse_y = 0;
			if ((uint32_t)comp.mouse_y >= drm.mode.vdisplay)
				comp.mouse_y = drm.mode.vdisplay - 1;
		}

		/* 드래그 중이면 윈도우 이동 */
		if (comp.dragging >= 0 && comp.mouse_btn_left) {
			struct window *w = &comp.windows[comp.dragging];
			w->x = comp.mouse_x - comp.drag_off_x;
			w->y = comp.mouse_y - comp.drag_off_y;
		}

		/* 리사이즈 중이면 윈도우 크기 변경 (Class 59) */
		if (comp.resizing >= 0 && comp.mouse_btn_left) {
			struct window *rw = &comp.windows[comp.resizing];
			int dx = comp.mouse_x - comp.resize_start_x;
			int dy = comp.mouse_y - comp.resize_start_y;
			int nw = comp.resize_orig_w;
			int nh = comp.resize_orig_h;

			if (comp.resize_edge & 1) nw += dx; /* 우측 */
			if (comp.resize_edge & 2) nh += dy; /* 하단 */
			if (nw < MIN_WIN_W) nw = MIN_WIN_W;
			if (nh < MIN_WIN_H) nh = MIN_WIN_H;
			rw->w = nw;
			rw->h = nh;
		}

		/* CDP: 마우스 이동 전달 (Class 12) */
		/*
		 * Class 17: 패널에도 모션 이벤트를 전달.
		 * 패널 위에 마우스가 있으면 패널에,
		 * 아니면 포커스된 윈도우에 전달.
		 */
		{
			int hover_idx = window_at_point(comp.mouse_x,
							comp.mouse_y);
			if (hover_idx >= 0 && comp.windows[hover_idx].is_panel) {
				struct window *pw = &comp.windows[hover_idx];
				int sidx = pw->cdp_surface_idx;

				if (sidx >= 0 && sidx < MAX_CDP_SURFACES &&
				    cdp.surfaces[sidx].active) {
					int cfd = cdp.clients[cdp.surfaces[sidx].client_idx].fd;

					if (cfd >= 0) {
						struct cdp_pointer_motion evt;
						evt.surface_id = (uint32_t)(sidx + 1);
						evt.x = comp.mouse_x - pw->x;
						evt.y = comp.mouse_y - pw->y;
						fcntl(cfd, F_SETFL, 0);
						cdp_send_msg(cfd, CDP_EVT_POINTER_MOTION,
							     &evt, sizeof(evt));
						fcntl(cfd, F_SETFL, O_NONBLOCK);
					}
				}
			} else if (comp.focused >= 0 && comp.focused < comp.num_windows) {
				struct window *fw = &comp.windows[comp.focused];
				int sx = comp.mouse_x - fw->x;
				int sy = comp.mouse_y - (fw->y + TITLEBAR_H);

				cdp_route_pointer_motion(sx, sy);
			}
		}

		/* 데미지: 이전 커서 위치 + 새 커서 위치 */
		damage_add(comp.prev_mouse_x, comp.prev_mouse_y,
			   CURSOR_SIZE, CURSOR_SIZE);
		damage_add(comp.mouse_x, comp.mouse_y,
			   CURSOR_SIZE, CURSOR_SIZE);
		if (comp.dragging >= 0 || comp.resizing >= 0)
			damage_add_full(); /* 드래그/리사이즈 중 → 전체 리드로 */
		comp.prev_mouse_x = comp.mouse_x;
		comp.prev_mouse_y = comp.mouse_y;
		comp.need_redraw = 1;

	} else if (ev->type == EV_ABS) {
		/*
		 * 절대 좌표 이벤트 (USB 태블릿 — QEMU 기본)
		 *
		 * QEMU USB 태블릿은 호스트 마우스 좌표를 절대값으로 전달:
		 *   ABS_X: 0 ~ abs_max_x (보통 32767)
		 *   ABS_Y: 0 ~ abs_max_y (보통 32767)
		 *
		 * 화면 좌표로 변환:
		 *   screen_x = (abs_x * screen_width) / abs_max_x
		 *
		 * 왜 절대 좌표?
		 *   QEMU에서 호스트↔게스트 마우스 위치를 1:1 동기화하기 위함.
		 *   상대 마우스는 캡처(grab)가 필요하지만,
		 *   절대 좌표는 자유롭게 이동 가능 (마우스 통합).
		 */
		if (ev->code == ABS_X && dev->abs_max_x > 0) {
			comp.mouse_x = (int)((long)ev->value *
				drm.mode.hdisplay / dev->abs_max_x);
			if (comp.mouse_x < 0) comp.mouse_x = 0;
			if ((uint32_t)comp.mouse_x >= drm.mode.hdisplay)
				comp.mouse_x = drm.mode.hdisplay - 1;
		} else if (ev->code == ABS_Y && dev->abs_max_y > 0) {
			comp.mouse_y = (int)((long)ev->value *
				drm.mode.vdisplay / dev->abs_max_y);
			if (comp.mouse_y < 0) comp.mouse_y = 0;
			if ((uint32_t)comp.mouse_y >= drm.mode.vdisplay)
				comp.mouse_y = drm.mode.vdisplay - 1;
		}

		/* 드래그 중이면 윈도우 이동 */
		if (comp.dragging >= 0 && comp.mouse_btn_left) {
			struct window *w = &comp.windows[comp.dragging];
			w->x = comp.mouse_x - comp.drag_off_x;
			w->y = comp.mouse_y - comp.drag_off_y;
		}

		/* 리사이즈 중이면 윈도우 크기 변경 (Class 59) */
		if (comp.resizing >= 0 && comp.mouse_btn_left) {
			struct window *rw = &comp.windows[comp.resizing];
			int dx = comp.mouse_x - comp.resize_start_x;
			int dy = comp.mouse_y - comp.resize_start_y;
			int nw = comp.resize_orig_w;
			int nh = comp.resize_orig_h;

			if (comp.resize_edge & 1) nw += dx;
			if (comp.resize_edge & 2) nh += dy;
			if (nw < MIN_WIN_W) nw = MIN_WIN_W;
			if (nh < MIN_WIN_H) nh = MIN_WIN_H;
			rw->w = nw;
			rw->h = nh;
		}

		/* CDP: 마우스 이동 전달 (패널 + 포커스 윈도우) */
		{
			int hover_idx = window_at_point(comp.mouse_x,
							comp.mouse_y);
			if (hover_idx >= 0 && comp.windows[hover_idx].is_panel) {
				struct window *pw = &comp.windows[hover_idx];
				int sidx = pw->cdp_surface_idx;

				if (sidx >= 0 && sidx < MAX_CDP_SURFACES &&
				    cdp.surfaces[sidx].active) {
					int cfd = cdp.clients[cdp.surfaces[sidx].client_idx].fd;

					if (cfd >= 0) {
						struct cdp_pointer_motion evt;
						evt.surface_id = (uint32_t)(sidx + 1);
						evt.x = comp.mouse_x - pw->x;
						evt.y = comp.mouse_y - pw->y;
						fcntl(cfd, F_SETFL, 0);
						cdp_send_msg(cfd, CDP_EVT_POINTER_MOTION,
							     &evt, sizeof(evt));
						fcntl(cfd, F_SETFL, O_NONBLOCK);
					}
				}
			} else if (comp.focused >= 0 && comp.focused < comp.num_windows) {
				struct window *fw = &comp.windows[comp.focused];
				int sx = comp.mouse_x - fw->x;
				int sy = comp.mouse_y - (fw->y + TITLEBAR_H);

				cdp_route_pointer_motion(sx, sy);
			}
		}

		/* 데미지: 이전 커서 위치 + 새 커서 위치 */
		damage_add(comp.prev_mouse_x, comp.prev_mouse_y,
			   CURSOR_SIZE, CURSOR_SIZE);
		damage_add(comp.mouse_x, comp.mouse_y,
			   CURSOR_SIZE, CURSOR_SIZE);
		if (comp.dragging >= 0 || comp.resizing >= 0)
			damage_add_full();
		comp.prev_mouse_x = comp.mouse_x;
		comp.prev_mouse_y = comp.mouse_y;
		comp.need_redraw = 1;

	} else if (ev->type == EV_KEY) {
		if (ev->code == BTN_LEFT) {
			if (ev->value == 1) {
				/* 왼쪽 버튼 누름 */
				comp.mouse_btn_left = 1;

				int idx = window_at_point(comp.mouse_x,
							  comp.mouse_y);
				if (idx >= 0) {
					struct window *w = &comp.windows[idx];

					if (w->is_panel) {
						/*
						 * 패널 클릭 (Class 17):
						 *   - 포커스 변경하지 않음
						 *   - 드래그 불가
						 *   - CDP 포인터 이벤트만 전달
						 *
						 * 패널은 데스크탑의 일부이므로
						 * 다른 윈도우의 포커스를 빼앗지 않음.
						 * Windows 태스크바, macOS Dock과 동일.
						 */
						int sidx = w->cdp_surface_idx;

						if (sidx >= 0 && sidx < MAX_CDP_SURFACES &&
						    cdp.surfaces[sidx].active) {
							struct cdp_surface *surf = &cdp.surfaces[sidx];
							int cfd = cdp.clients[surf->client_idx].fd;

							if (cfd >= 0) {
								struct cdp_pointer_button evt;
								evt.surface_id = (uint32_t)(sidx + 1);
								evt.button = BTN_LEFT;
								evt.state = 1;
								fcntl(cfd, F_SETFL, 0);
								cdp_send_msg(cfd, CDP_EVT_POINTER_BUTTON,
									     &evt, sizeof(evt));
								fcntl(cfd, F_SETFL, O_NONBLOCK);
							}
						}
					} else if (is_close_btn(w, comp.mouse_x,
							 comp.mouse_y)) {
						/* 닫기 버튼 클릭 → 클라이언트 정리 */
						if (comp.focused == idx)
							comp.focused = -1;
						int sidx = w->cdp_surface_idx;
						if (sidx >= 0 &&
						    sidx < MAX_CDP_SURFACES &&
						    cdp.surfaces[sidx].active) {
							cdp_disconnect_client(
								cdp.surfaces[sidx].client_idx);
						} else {
							w->visible = 0;
						}
					} else if (is_minimize_btn(w, comp.mouse_x,
								   comp.mouse_y)) {
						/* 최소화 버튼 (Class 59) */
						w->minimized = 1;
						w->visible = 0;
						if (comp.focused == idx)
							comp.focused = -1;
					} else if (is_maximize_btn(w, comp.mouse_x,
								   comp.mouse_y)) {
						/* 최대화/복원 토글 (Class 59) */
						if (w->maximized) {
							w->x = w->saved_x;
							w->y = w->saved_y;
							w->w = w->saved_w;
							w->h = w->saved_h;
							w->maximized = 0;
						} else {
							w->saved_x = w->x;
							w->saved_y = w->y;
							w->saved_w = w->w;
							w->saved_h = w->h;
							w->x = 0;
							w->y = 0;
							w->w = (int)drm.mode.hdisplay;
							int ph = 0;
							for (int pi = 0; pi < comp.num_windows; pi++) {
								if (comp.windows[pi].is_panel &&
								    comp.windows[pi].visible)
									ph = comp.windows[pi].h;
							}
							w->h = (int)drm.mode.vdisplay - ph;
							w->maximized = 1;
						}
						cdp_send_configure(idx, w->w,
								   w->h - TITLEBAR_H);
					} else {
						int edge = resize_edge_at(w, comp.mouse_x,
									  comp.mouse_y);
						if (edge > 0) {
							/* 리사이즈 시작 (Class 59) */
							window_focus(idx);
							w = &comp.windows[comp.num_windows - 1];
							comp.resizing = comp.num_windows - 1;
							comp.resize_edge = edge;
							comp.resize_start_x = comp.mouse_x;
							comp.resize_start_y = comp.mouse_y;
							comp.resize_orig_w = w->w;
							comp.resize_orig_h = w->h;
						} else {
							/* 포커스 변경 */
							window_focus(idx);

							/* 타이틀바면 드래그 시작 */
							w = &comp.windows[comp.num_windows - 1];
							if (is_titlebar(w, comp.mouse_x,
									comp.mouse_y)) {
								comp.dragging = comp.num_windows - 1;
								comp.drag_off_x = comp.mouse_x - w->x;
								comp.drag_off_y = comp.mouse_y - w->y;
							}
						}
					}
				} else {
					comp.focused = -1;
				}
				/* CDP: 버튼 이벤트 전달 (Class 12) */
				cdp_route_pointer_button(BTN_LEFT, 1);
				damage_add_full(); /* 포커스/Z-order 변경 → 전체 리드로 */
				comp.need_redraw = 1;

			} else if (ev->value == 0) {
				/* 왼쪽 버튼 해제 */
				comp.mouse_btn_left = 0;
				comp.dragging = -1;

				/* 리사이즈 종료 → configure 전송 (Class 59) */
				if (comp.resizing >= 0) {
					struct window *rw = &comp.windows[comp.resizing];
					cdp_send_configure(comp.resizing, rw->w,
							   rw->h - TITLEBAR_H);
					comp.resizing = -1;
				}

				/* CDP: 버튼 해제 — 패널 포함 전달 */
				int hover = window_at_point(comp.mouse_x,
							    comp.mouse_y);
				if (hover >= 0 && comp.windows[hover].is_panel) {
					struct window *pw = &comp.windows[hover];
					int sidx = pw->cdp_surface_idx;

					if (sidx >= 0 && sidx < MAX_CDP_SURFACES &&
					    cdp.surfaces[sidx].active) {
						int cfd = cdp.clients[cdp.surfaces[sidx].client_idx].fd;

						if (cfd >= 0) {
							struct cdp_pointer_button evt;
							evt.surface_id = (uint32_t)(sidx + 1);
							evt.button = BTN_LEFT;
							evt.state = 0;
							fcntl(cfd, F_SETFL, 0);
							cdp_send_msg(cfd, CDP_EVT_POINTER_BUTTON,
								     &evt, sizeof(evt));
							fcntl(cfd, F_SETFL, O_NONBLOCK);
						}
					}
				} else {
					cdp_route_pointer_button(BTN_LEFT, 0);
				}
			}
		}
	}
}

/* 키보드 이벤트 처리 */
static void handle_keyboard_event(struct input_event *ev)
{
	if (ev->type != EV_KEY)
		return;

	/*
	 * Shift 키 상태 추적 (누름/해제 모두 처리)
	 *
	 * evdev는 물리 키 이벤트를 그대로 전달:
	 *   value=1: 키 누름
	 *   value=0: 키 해제
	 *   value=2: 키 반복 (길게 누를 때)
	 *
	 * Shift는 "modifier key"로, 눌러진 동안 다른 키의 의미를 변경.
	 * 눌렀을 때 shift_held=1, 놓았을 때 shift_held=0.
	 */
	if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
		shift_held = (ev->value != 0); /* 1=누름/반복, 0=해제 */
		return;
	}

	/* Ctrl 키 상태 추적 — 터미널에서 Ctrl+C, Ctrl+D 등에 필요 */
	if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) {
		ctrl_held = (ev->value != 0);
		return;
	}

	/* 키 누름(1) 또는 반복(2)만 처리 (해제는 무시) */
	if (ev->value == 0)
		return;

	/* ESC → 컴포지터 종료 */
	if (ev->code == KEY_ESC) {
		comp.running = 0;
		return;
	}

	/* 포커스된 윈도우가 없으면 무시 */
	if (comp.focused < 0 || comp.focused >= comp.num_windows)
		return;

	struct window *win = &comp.windows[comp.focused];

	/*
	 * CDP surface에 포커스? → 클라이언트에 키 이벤트 전달 (Class 12)
	 *
	 * Wayland 보안 모델:
	 *   포커스된 surface의 클라이언트에만 키 이벤트를 보냄.
	 *   다른 앱은 이 키 입력을 볼 수 없음!
	 */
	if (win->cdp_surface_idx >= 0) {
		char ch = keycode_to_char(ev->code);

		cdp_route_key(ev->code, (uint32_t)ev->value, ch);
		return;
	}

	/* 내부 윈도우 — 기존 텍스트 입력 처리 */
	if (ev->code == KEY_BACKSPACE) {
		if (win->text_len > 0) {
			win->text_len--;
			win->text[win->text_len] = '\0';
		}
	} else if (ev->code == KEY_ENTER) {
		if (win->text_len < WIN_TEXT_MAX - 1) {
			win->text[win->text_len++] = ' ';
			win->text[win->text_len] = '\0';
		}
	} else {
		char ch = keycode_to_char(ev->code);

		if (ch && win->text_len < WIN_TEXT_MAX - 1) {
			win->text[win->text_len++] = ch;
			win->text[win->text_len] = '\0';
		}
	}

	damage_add_window(comp.focused);
	comp.need_redraw = 1;
}

/* ============================================================
 * 메인 이벤트 루프
 * ============================================================
 *
 * poll() 기반 이벤트 루프:
 *
 *   struct pollfd에 감시할 fd 목록을 넣고
 *   poll()을 호출하면 이벤트가 올 때까지 블로킹.
 *   (timeout으로 주기적 갱신도 가능)
 *
 *   이벤트가 오면:
 *     1. 어떤 fd에서 왔는지 확인
 *     2. 해당 장치에서 이벤트 읽기
 *     3. 이벤트 처리 (마우스 이동, 키 입력 등)
 *     4. 필요하면 화면 갱신
 *
 *   이것이 모든 GUI 프레임워크의 기본 구조입니다.
 *   Qt의 QEventLoop, GTK의 g_main_loop, Windows의
 *   GetMessage/TranslateMessage/DispatchMessage 모두
 *   내부적으로 이와 같은 패턴을 사용합니다.
 */
static void event_loop(void)
{
	/*
	 * 확장된 poll 배열 (Class 12 수정):
	 *
	 *   fds[0..N-1]           — evdev 입력 장치 (기존)
	 *   fds[N]                — CDP 리슨 소켓 (신규)
	 *   fds[N+1..N+1+M]       — 연결된 CDP 클라이언트 소켓 (신규)
	 *
	 * Wayland 컴포지터도 동일한 패턴:
	 *   wl_event_loop에 DRM fd, 입력 fd, 리슨 소켓 fd를 모두 등록.
	 *   하나의 epoll/poll 루프에서 모든 이벤트를 처리.
	 */
	#define MAX_POLL_FDS (MAX_INPUT_FDS + 1 + MAX_CDP_CLIENTS)
	struct pollfd fds[MAX_POLL_FDS];

	/* 초기 화면 그리기 */
	comp.need_redraw = 1;
	damage_add_full();

	while (comp.running) {
		/*
		 * 화면 갱신: need_redraw 플래그 + 데미지가 있을 때만.
		 * idle 상태에서는 렌더링을 완전히 건너뜀 (Class 58).
		 */
		if (comp.need_redraw && damage_has_any()) {
			render_frame();
			comp.need_redraw = 0;
		}

		/*
		 * poll 배열을 매 루프마다 재구성
		 * (클라이언트 연결/해제로 fd 목록이 변할 수 있으므로)
		 */
		int nfds = 0;

		/* 1. evdev 입력 장치 */
		int input_start = nfds;

		for (int i = 0; i < comp.num_inputs; i++) {
			fds[nfds].fd = comp.inputs[i].fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		/* 2. CDP 리슨 소켓 */
		int listen_idx = -1;

		if (cdp.listen_fd >= 0) {
			listen_idx = nfds;
			fds[nfds].fd = cdp.listen_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		/* 3. CDP 클라이언트 소켓 */
		int client_fds_start = nfds;
		int client_map[MAX_CDP_CLIENTS];  /* fds 인덱스 → 클라이언트 인덱스 */

		for (int i = 0; i < MAX_CDP_CLIENTS; i++) {
			client_map[i] = -1;
			if (cdp.clients[i].fd >= 0) {
				client_map[nfds - client_fds_start] = i;
				fds[nfds].fd = cdp.clients[i].fd;
				fds[nfds].events = POLLIN;
				nfds++;
			}
		}

		/* poll() — 모든 fd에서 이벤트 대기 */
		int ret = poll(fds, nfds, 100);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (ret == 0) {
			/* 타임아웃: idle — 리드로 불필요 (Class 58 변경)
			 * 이전: 매 100ms마다 전체 리드로 → CPU 낭비
			 * 변경: 변경 없으면 건너뜀
			 */
			continue;
		}

		/* evdev 입력 처리 */
		for (int i = input_start; i < input_start + comp.num_inputs; i++) {
			if (!(fds[i].revents & POLLIN))
				continue;

			int input_idx = i - input_start;
			struct input_event ev;

			while (read(comp.inputs[input_idx].fd, &ev,
				    sizeof(ev)) == (ssize_t)sizeof(ev)) {

				if (comp.inputs[input_idx].type == INPUT_MOUSE)
					handle_mouse_event(&comp.inputs[input_idx], &ev);
				else if (comp.inputs[input_idx].type == INPUT_KEYBOARD)
					handle_keyboard_event(&ev);
			}
		}

		/*
		 * CDP 리슨 소켓: 새 클라이언트 연결
		 * Wayland: wl_client_create()
		 */
		if (listen_idx >= 0 && (fds[listen_idx].revents & POLLIN))
			cdp_accept_client();

		/*
		 * CDP 클라이언트 소켓: 메시지 처리
		 * Wayland: wl_client_dispatch()
		 *
		 * POLLHUP/POLLERR: 클라이언트가 소켓을 닫았거나 에러 발생.
		 * 이 경우 cdp_handle_client_msg()를 호출하면 read()가 0을
		 * 반환하여 자연스럽게 disconnect 처리됨.
		 * POLLIN만 체크하면 POLLHUP만 올 때 슬롯이 영원히 남는
		 * 버그가 발생한다!
		 */
		for (int i = client_fds_start; i < nfds; i++) {
			if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;

			int cidx = client_map[i - client_fds_start];

			if (cidx >= 0)
				cdp_handle_client_msg(cidx);
		}
	}
}

/* ============================================================
 * 메인 함수
 * ============================================================ */

int main(void)
{
	/*
	 * SIGPIPE 무시 — 클라이언트가 연결을 끊었을 때
	 * 소켓에 write하면 SIGPIPE가 발생한다.
	 * 무시하지 않으면 compositor가 죽는다!
	 */
	signal(SIGPIPE, SIG_IGN);

	printf("\n");
	printf("=== CITC OS Compositor ===\n\n");

	/* 1. DRM init */
	printf("[1/4] DRM init...\n");
	if (drm_init() < 0) {
		printf("DRM init failed!\n");
		return 1;
	}

	/* 2. Input device scan */
	printf("[2/4] Input device scan...\n");
	input_scan();

	if (comp.num_inputs == 0) {
		printf("No input devices found.\n");
		printf("Run with QEMU --gui mode.\n");
		drm_cleanup();
		return 1;
	}

	/* 3. PSF2 폰트 로드 (Class 61) */
	if (psf2_load(&g_psf2, PSF2_FONT_PATH) == 0) {
		printf("  PSF2 font: %ux%u, %u glyphs\n",
		       g_psf2.width, g_psf2.height, g_psf2.numglyph);
	} else {
		printf("  PSF2 font not found, using font8x8 fallback\n");
	}

	/* 4. CDP server init (Class 12) */
	printf("[4/5] CDP server init...\n");
	if (cdp_server_init() < 0)
		printf("  Warning: CDP server failed (internal windows only)\n");

	/* 5. Window creation */
	printf("[5/5] Window creation...\n\n");

	/* 마우스를 화면 중앙에 배치 */
	comp.mouse_x = drm.mode.hdisplay / 2;
	comp.mouse_y = drm.mode.vdisplay / 2;
	comp.focused = -1;
	comp.dragging = -1;
	comp.resizing = -1;
	comp.running = 1;

	/*
	 * 기본 윈도우 3개 생성
	 *
	 * 실제 컴포지터에서는 앱이 Wayland 프로토콜로 윈도우를 요청.
	 * 지금은 데모용으로 하드코딩합니다.
	 */
	window_create(50, 50, 300, 200, "Terminal",
		      40, 100, 200);   /* 파란색 */
	window_create(200, 150, 280, 180, "Editor",
		      50, 160, 80);    /* 녹색 */
	window_create(400, 80, 250, 160, "Info",
		      180, 80, 180);   /* 보라색 */

	/* Info 윈도우에 안내 텍스트 */
	struct window *info = &comp.windows[2];
	const char *help = "Click to focus Drag title to move Type to input ESC to quit";
	snprintf(info->text, WIN_TEXT_MAX, "%s", help);
	info->text_len = strlen(info->text);

	comp.focused = 2;  /* Info 윈도우에 초기 포커스 */

	printf("컴포지터 시작!\n");
	printf("  - QEMU 창에서 마우스를 움직이세요\n");
	printf("  - 마우스 캡처: QEMU 창 클릭 (해제: Ctrl+Alt+G)\n");
	printf("  - 윈도우 클릭 → 포커스\n");
	printf("  - 타이틀바 드래그 → 이동\n");
	printf("  - [X] 클릭 → 닫기\n");
	printf("  - 키보드 입력 → 포커스된 윈도우에 텍스트\n");
	printf("  - ESC → 종료\n");
	printf("  - CDP 클라이언트: cdp_demo 실행하면 새 윈도우 생성!\n\n");

	/* 메인 이벤트 루프 */
	event_loop();

	/* 정리 */
	cdp_server_cleanup();
	for (int i = 0; i < comp.num_inputs; i++)
		close(comp.inputs[i].fd);
	drm_cleanup();
	printf("컴포지터 종료.\n");

	return 0;
}
