/*
 * user32.c — CITC OS user32.dll 구현
 * ==================================
 *
 * Windows User32 (User Interface DLL).
 *
 * User32는 Win32의 윈도우 관리 + 메시지 시스템:
 *   RegisterClassA → WNDCLASS 등록
 *   CreateWindowExA → HWND 생성 + CDP surface 매핑
 *   GetMessageA → 메시지 루프 (블로킹)
 *   DispatchMessageA → WndProc 콜백 호출
 *
 * HWND (Window Handle)은 독립 네임스페이스:
 *   HWND = (void*)(index + HWND_OFFSET)
 *   NT HANDLE(0x100+), HDC(0x20000+), HGDI(0x30000+)와 범위 분리.
 *
 * CDP (CITC Display Protocol) 통합:
 *   HWND → CDP surface 1:1 매핑
 *   CDP 이벤트 → Win32 메시지 변환
 *   컴포지터 미실행 시: 로컬 픽셀 버퍼 + self-pipe 모드
 *
 * Linux 대응:
 *   HWND ≈ X11 Window 또는 Wayland xdg_toplevel
 *   GetMessageA ≈ wl_display_dispatch() 또는 XNextEvent()
 *   WndProc ≈ 이벤트 핸들러 콜백
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>

#include "../../../include/win32.h"
#include "../gdi32/gdi32.h"

/* Linux evdev keycode → Windows VK_* 변환 */
#include "keymap.h"

/* CDP 클라이언트 (header-only 라이브러리) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../../display/protocol/cdp_client.h"
#pragma GCC diagnostic pop

/* ============================================================
 * 윈도우 클래스 테이블
 * ============================================================
 *
 * RegisterClassA()로 등록한 클래스를 저장.
 * 실제 Windows: win32k.sys가 관리하는 atom table.
 * 우리 구현: 단순 배열.
 */
#define MAX_WNDCLASSES 16

struct wndclass_entry {
	char class_name[64];
	WNDPROC wndproc;
	UINT style;
	HBRUSH hbr_background;
	int active;
};

static struct wndclass_entry wndclass_table[MAX_WNDCLASSES];

/* ============================================================
 * 윈도우 테이블
 * ============================================================
 *
 * HWND = (void*)(index + HWND_OFFSET)
 * HANDLE(0x100+), HDC(0x20000+), HGDI(0x30000+)와 범위 분리.
 */
#define MAX_WINDOWS   64
#define HWND_OFFSET   0x10000

struct wnd_entry {
	int active;
	char class_name[64];
	char title[128];
	int x, y, width, height;   /* 클라이언트 영역 */
	DWORD style;
	DWORD ex_style;             /* 확장 스타일 (GWL_EXSTYLE) */
	WNDPROC wndproc;
	uintptr_t user_data;        /* GWLP_USERDATA */

	/* CDP surface 바인딩 */
	struct cdp_window *cdp_win; /* CDP 윈도우 (NULL if no compositor) */
	uint32_t *pixels;           /* XRGB8888 픽셀 버퍼 */
	int local_pixels;           /* 1 = 로컬 mmap (fallback 모드) */

	/* 상태 */
	int visible;
	int needs_paint;
};

static struct wnd_entry wnd_table[MAX_WINDOWS];

/* ============================================================
 * 메시지 큐
 * ============================================================
 *
 * Win32 스레드별 메시지 큐의 단순화 버전.
 * Phase 3에서는 단일 스레드만 지원.
 *
 * 실제 Windows:
 *   각 스레드가 자신의 메시지 큐를 가짐.
 *   PostMessage → 대상 윈도우의 스레드 큐에 추가.
 *   GetMessage → 자기 스레드 큐에서 꺼내기.
 */
#define MSG_QUEUE_SIZE 256

static struct {
	MSG messages[MSG_QUEUE_SIZE];
	int head, tail, count;
	int quit_posted, quit_code;
} msg_queue;

/* ============================================================
 * CDP 연결 + self-pipe
 * ============================================================
 *
 * g_cdp: CDP 컴포지터 연결 (NULL = 컴포지터 없음)
 * msg_pipe: PostMessage → GetMessage 깨우기용
 *
 * GetMessageA의 블로킹 전략:
 *   poll() on:
 *     [0] g_cdp->sock_fd  — CDP 이벤트
 *     [1] msg_pipe[0]     — PostMessage 알림
 */
static struct cdp_conn *g_cdp = NULL;
static int msg_pipe[2] = {-1, -1};
static int cdp_init_done = 0;

/* 현재 포커스 윈도우 */
static HWND g_focus_hwnd = NULL;

/* 마지막 키의 ASCII 문자 (TranslateMessage에서 WM_CHAR 생성용) */
static char g_last_char = 0;

/* ============================================================
 * 타이머 테이블
 * ============================================================
 *
 * SetTimer() → 타이머 등록 → GetMessageA poll()에서 체크
 * → 만료 시 WM_TIMER 메시지 생성.
 *
 * 우선순위: 일반 메시지 > WM_TIMER > WM_PAINT
 * (실제 Windows와 동일)
 */
#define MAX_TIMERS 32

struct timer_entry {
	int active;
	HWND hwnd;
	uintptr_t timer_id;
	UINT interval_ms;
	uint64_t next_fire_ms;   /* monotonic 밀리초 */
};

static struct timer_entry timer_table[MAX_TIMERS];

/* monotonic 시간 (밀리초) */
static uint64_t get_monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ============================================================
 * 내부 유틸리티
 * ============================================================ */

static struct wnd_entry *hwnd_to_wnd(HWND hwnd)
{
	uintptr_t val = (uintptr_t)hwnd;

	if (val < HWND_OFFSET)
		return NULL;

	int idx = (int)(val - HWND_OFFSET);

	if (idx < 0 || idx >= MAX_WINDOWS)
		return NULL;
	if (!wnd_table[idx].active)
		return NULL;

	return &wnd_table[idx];
}

/* 메시지 큐에 추가 */
static int enqueue_msg(const MSG *m)
{
	if (msg_queue.count >= MSG_QUEUE_SIZE)
		return -1;

	msg_queue.messages[msg_queue.tail] = *m;
	msg_queue.tail = (msg_queue.tail + 1) % MSG_QUEUE_SIZE;
	msg_queue.count++;
	return 0;
}

/* self-pipe에 1바이트 쓰기 (GetMessageA poll() 깨우기) */
static void wakeup_msg_loop(void)
{
	if (msg_pipe[1] >= 0) {
		char c = 'W';

		if (write(msg_pipe[1], &c, 1) < 0) {
			/* non-blocking pipe full — 무시 (이미 깨어있음) */
		}
	}
}

/* CDP surface_id → HWND 변환 */
static HWND surface_to_hwnd(uint32_t surface_id)
{
	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (wnd_table[i].active && wnd_table[i].cdp_win &&
		    wnd_table[i].cdp_win->surface_id == surface_id)
			return (HWND)(uintptr_t)(i + HWND_OFFSET);
	}
	return NULL;
}

/* WNDCLASS 검색 */
static struct wndclass_entry *find_wndclass(const char *name)
{
	for (int i = 0; i < MAX_WNDCLASSES; i++) {
		if (wndclass_table[i].active &&
		    strcmp(wndclass_table[i].class_name, name) == 0)
			return &wndclass_table[i];
	}
	return NULL;
}

/* ============================================================
 * CDP 이벤트 → Win32 메시지 변환
 * ============================================================
 *
 * CDP 이벤트 콜백 함수들.
 * cdp_dispatch()가 이벤트를 읽으면 이 콜백이 호출됨.
 * Win32 메시지로 변환하여 큐에 추가.
 *
 * CDP → Win32 매핑:
 *   CDP_EVT_KEY(state=1)          → WM_KEYDOWN
 *   CDP_EVT_KEY(state=0)          → WM_KEYUP
 *   CDP_EVT_POINTER_MOTION        → WM_MOUSEMOVE
 *   CDP_EVT_POINTER_BUTTON(L,1)   → WM_LBUTTONDOWN
 *   CDP_EVT_POINTER_BUTTON(L,0)   → WM_LBUTTONUP
 *   CDP_EVT_FOCUS_IN              → WM_SETFOCUS
 *   CDP_EVT_FOCUS_OUT             → WM_KILLFOCUS
 */
static void on_cdp_key(uint32_t keycode, uint32_t state, char ch)
{
	if (!g_focus_hwnd)
		return;

	/*
	 * Linux evdev keycode → Windows VK_* 변환.
	 *
	 * Win32 WM_KEYDOWN/WM_KEYUP:
	 *   wParam = Virtual Key code (VK_*)
	 *   lParam = repeat count (bit 0-15)
	 *          | scan code (bit 16-23)
	 *          | extended (bit 24)
	 *          | context (bit 29)
	 *          | previous state (bit 30)
	 *          | transition state (bit 31)
	 *
	 * Phase 3: lParam에 스캔코드 + 기본 플래그만 설정.
	 * CDP의 ASCII character는 TranslateMessage에서
	 * WM_CHAR로 별도 변환.
	 */
	uint32_t vk = linux_keycode_to_vk(keycode);

	if (vk == 0)
		return;   /* 매핑 없는 키 무시 */

	MSG m;

	memset(&m, 0, sizeof(m));
	m.hwnd = g_focus_hwnd;
	m.message = state ? WM_KEYDOWN : WM_KEYUP;
	m.wParam = (WPARAM)vk;

	/*
	 * lParam 구성:
	 *   bit 0-15:  repeat count (1)
	 *   bit 16-23: scan code (evdev keycode를 스캔코드로 사용)
	 *   bit 30:    previous state (KEYUP이면 1)
	 *   bit 31:    transition state (KEYUP이면 1)
	 */
	LPARAM lparam = 1;                        /* repeat count = 1 */

	lparam |= (LPARAM)(keycode & 0xFF) << 16; /* scan code */
	if (!state) {
		lparam |= (LPARAM)1 << 30;        /* previous state = 1 */
		lparam |= (LPARAM)1 << 31;        /* transition = 1 (releasing) */
	}
	m.lParam = lparam;

	enqueue_msg(&m);

	/*
	 * CDP의 ASCII character 저장: TranslateMessage에서 사용.
	 * lParam의 하위 8비트는 repeat count이므로,
	 * WM_CHAR 생성 시 CDP character를 직접 사용.
	 *
	 * 별도 저장소: 마지막 키의 ASCII 값.
	 */
	if (state && ch)
		g_last_char = ch;
}

static void on_cdp_pointer_motion(uint32_t surface_id, int x, int y)
{
	HWND hwnd = surface_to_hwnd(surface_id);

	if (!hwnd)
		return;

	MSG m;

	memset(&m, 0, sizeof(m));
	m.hwnd = hwnd;
	m.message = WM_MOUSEMOVE;
	m.lParam = MAKELPARAM(x, y);
	enqueue_msg(&m);
}

static void on_cdp_pointer_button(uint32_t surface_id, uint32_t button,
				  uint32_t state)
{
	HWND hwnd = surface_to_hwnd(surface_id);

	if (!hwnd)
		return;

	MSG m;

	memset(&m, 0, sizeof(m));
	m.hwnd = hwnd;

	/*
	 * Linux BTN_LEFT=0x110, BTN_RIGHT=0x111
	 * → WM_LBUTTONDOWN/UP, WM_RBUTTONDOWN/UP
	 */
	if (button == 0x110)        /* BTN_LEFT */
		m.message = state ? WM_LBUTTONDOWN : WM_LBUTTONUP;
	else if (button == 0x111)   /* BTN_RIGHT */
		m.message = state ? WM_RBUTTONDOWN : WM_RBUTTONUP;
	else
		return;

	enqueue_msg(&m);
}

static void on_cdp_focus_in(uint32_t surface_id)
{
	HWND hwnd = surface_to_hwnd(surface_id);

	g_focus_hwnd = hwnd;

	if (hwnd) {
		MSG m;

		memset(&m, 0, sizeof(m));
		m.hwnd = hwnd;
		m.message = WM_SETFOCUS;
		enqueue_msg(&m);
	}
}

static void on_cdp_focus_out(uint32_t surface_id)
{
	HWND hwnd = surface_to_hwnd(surface_id);

	if (hwnd) {
		MSG m;

		memset(&m, 0, sizeof(m));
		m.hwnd = hwnd;
		m.message = WM_KILLFOCUS;
		enqueue_msg(&m);
	}

	if (g_focus_hwnd == hwnd)
		g_focus_hwnd = NULL;
}

/* ============================================================
 * CDP 연결 (lazy 초기화)
 * ============================================================
 *
 * CreateWindowExA 첫 호출 시 CDP 연결 시도.
 * 콘솔 앱은 이 함수가 호출되지 않으므로
 * CDP 연결 타임아웃(2.5초)을 피할 수 있음.
 */
static void ensure_cdp_init(void)
{
	if (cdp_init_done)
		return;
	cdp_init_done = 1;

	g_cdp = cdp_connect();

	if (g_cdp) {
		g_cdp->on_key = on_cdp_key;
		g_cdp->on_pointer_motion = on_cdp_pointer_motion;
		g_cdp->on_pointer_button = on_cdp_pointer_button;
		g_cdp->on_focus_in = on_cdp_focus_in;
		g_cdp->on_focus_out = on_cdp_focus_out;
	} else {
		fprintf(stderr, "user32: CDP 컴포지터 없음 (로컬 모드)\n");
	}
}

/* ============================================================
 * user32 초기화
 * ============================================================ */

void user32_init(void)
{
	memset(wndclass_table, 0, sizeof(wndclass_table));
	memset(wnd_table, 0, sizeof(wnd_table));
	memset(&msg_queue, 0, sizeof(msg_queue));
	memset(timer_table, 0, sizeof(timer_table));

	/* self-pipe 생성 (PostMessage → GetMessage 깨우기) */
	if (pipe(msg_pipe) < 0) {
		fprintf(stderr, "user32: pipe() 실패\n");
		msg_pipe[0] = msg_pipe[1] = -1;
	} else {
		/* non-blocking (write 시 블록 방지) */
		fcntl(msg_pipe[0], F_SETFL, O_NONBLOCK);
		fcntl(msg_pipe[1], F_SETFL, O_NONBLOCK);
	}
}

/* ============================================================
 * Win32 API 함수
 * ============================================================ */

/* 전방 선언 (상호 참조용) */
__attribute__((ms_abi))
static LRESULT u32_DefWindowProcA(HWND, UINT, WPARAM, LPARAM);

/* --- RegisterClassA --- */

__attribute__((ms_abi))
static uint16_t u32_RegisterClassA(const WNDCLASSA *wc)
{
	if (!wc || !wc->lpszClassName)
		return 0;

	if (find_wndclass(wc->lpszClassName))
		return 0;

	for (int i = 0; i < MAX_WNDCLASSES; i++) {
		if (!wndclass_table[i].active) {
			wndclass_table[i].active = 1;
			strncpy(wndclass_table[i].class_name,
				wc->lpszClassName,
				sizeof(wndclass_table[i].class_name) - 1);
			wndclass_table[i].wndproc = wc->lpfnWndProc;
			wndclass_table[i].style = wc->style;
			wndclass_table[i].hbr_background = wc->hbrBackground;
			return (uint16_t)(i + 1);   /* atom = 1-based */
		}
	}

	return 0;
}

/* --- CreateWindowExA --- */

/*
 * 윈도우 생성의 전체 흐름:
 *   1. WNDCLASS 검색 → WndProc 획득
 *   2. wnd_entry 할당
 *   3. CDP surface 생성 (또는 로컬 버퍼)
 *   4. WM_CREATE 전송 (동기)
 *   5. HWND 반환
 */
__attribute__((ms_abi))
static HWND u32_CreateWindowExA(DWORD ex_style,
				const char *class_name,
				const char *window_name,
				DWORD style,
				int x, int y, int width, int height,
				HWND parent, HANDLE menu,
				HANDLE instance, void *param)
{
	(void)ex_style; (void)parent; (void)menu;
	(void)instance; (void)param;

	if (!class_name)
		return NULL;

	/* WNDCLASS 검색 */
	struct wndclass_entry *wc = find_wndclass(class_name);

	if (!wc) {
		fprintf(stderr, "user32: 클래스 '%s' 미등록\n", class_name);
		return NULL;
	}

	/* CW_USEDEFAULT 처리 */
	if (x == CW_USEDEFAULT) x = 100;
	if (y == CW_USEDEFAULT) y = 100;
	if (width == CW_USEDEFAULT) width = 640;
	if (height == CW_USEDEFAULT) height = 480;

	/* 빈 슬롯 찾기 */
	struct wnd_entry *w = NULL;
	int idx = -1;

	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (!wnd_table[i].active) {
			w = &wnd_table[i];
			idx = i;
			break;
		}
	}

	if (!w) {
		fprintf(stderr, "user32: 윈도우 테이블 가득 참\n");
		return NULL;
	}

	/* 엔트리 초기화 */
	memset(w, 0, sizeof(*w));
	w->active = 1;
	strncpy(w->class_name, class_name, sizeof(w->class_name) - 1);
	if (window_name)
		strncpy(w->title, window_name, sizeof(w->title) - 1);
	w->x = x;
	w->y = y;
	w->width = width;
	w->height = height;
	w->style = style;
	w->wndproc = wc->wndproc;
	w->needs_paint = 1;

	/* CDP 연결 (lazy 초기화) */
	ensure_cdp_init();

	/* CDP surface 생성 또는 로컬 버퍼 할당 */
	if (g_cdp) {
		w->cdp_win = cdp_create_surface(g_cdp, width, height,
						window_name ? window_name : "");
		if (w->cdp_win) {
			w->pixels = w->cdp_win->pixels;
		} else {
			fprintf(stderr, "user32: CDP surface 생성 실패\n");
			w->active = 0;
			return NULL;
		}
	} else {
		/* 컴포지터 없음 — 로컬 픽셀 버퍼 */
		size_t buf_size = (size_t)width * height * 4;

		w->pixels = mmap(NULL, buf_size,
				 PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (w->pixels == MAP_FAILED) {
			w->pixels = NULL;
			w->active = 0;
			return NULL;
		}
		memset(w->pixels, 0xFF, buf_size); /* 흰색 초기화 */
		w->local_pixels = 1;
	}

	HWND hwnd = (HWND)(uintptr_t)(idx + HWND_OFFSET);

	/* 포커스 설정 (첫 번째 윈도우) */
	if (!g_focus_hwnd)
		g_focus_hwnd = hwnd;

	/* WM_CREATE 동기 전송 */
	LRESULT result = w->wndproc(hwnd, WM_CREATE, 0, 0);

	if (result == -1) {
		/* WndProc가 생성 거부 */
		if (w->cdp_win)
			cdp_destroy_surface(g_cdp, w->cdp_win);
		else if (w->local_pixels && w->pixels)
			munmap(w->pixels, (size_t)width * height * 4);
		w->active = 0;
		return NULL;
	}

	return hwnd;
}

/* --- DestroyWindow --- */

__attribute__((ms_abi))
static int32_t u32_DestroyWindow(HWND hwnd)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return FALSE;

	/* WM_DESTROY 동기 전송 */
	w->wndproc(hwnd, WM_DESTROY, 0, 0);

	/* CDP surface 해제 */
	if (w->cdp_win && g_cdp)
		cdp_destroy_surface(g_cdp, w->cdp_win);
	else if (w->local_pixels && w->pixels)
		munmap(w->pixels, (size_t)w->width * w->height * 4);

	/* 포커스 해제 */
	if (g_focus_hwnd == hwnd)
		g_focus_hwnd = NULL;

	w->active = 0;
	return TRUE;
}

/* --- ShowWindow --- */

__attribute__((ms_abi))
static int32_t u32_ShowWindow(HWND hwnd, int cmd)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return FALSE;

	int was_visible = w->visible;

	if (cmd == SW_HIDE) {
		w->visible = 0;
	} else {
		w->visible = 1;
		w->needs_paint = 1;

		/* CDP commit (최초 표시) */
		if (w->cdp_win && g_cdp)
			cdp_commit_to(g_cdp, w->cdp_win);
	}

	return was_visible;
}

/* --- UpdateWindow --- */

/*
 * needs_paint가 설정된 윈도우에 WM_PAINT를 동기적으로 전송.
 * 메시지 큐를 거치지 않고 WndProc 직접 호출.
 */
__attribute__((ms_abi))
static int32_t u32_UpdateWindow(HWND hwnd)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return FALSE;

	if (w->needs_paint)
		w->wndproc(hwnd, WM_PAINT, 0, 0);

	return TRUE;
}

/* --- GetMessageA --- */

/*
 * 메시지 루프의 핵심: 이벤트 대기 + 메시지 반환.
 *
 * 블로킹 전략:
 *   1. quit_posted → FALSE 반환 (루프 종료)
 *   2. 큐에 메시지 → 꺼내서 반환
 *   3. needs_paint 윈도우 → WM_PAINT 생성 (최저 우선순위)
 *   4. poll()로 이벤트 대기:
 *      - CDP 소켓: 키보드/마우스 이벤트
 *      - self-pipe: PostMessage 알림
 */
__attribute__((ms_abi))
static int32_t u32_GetMessageA(MSG *msg, HWND filter_hwnd,
			       UINT filter_min, UINT filter_max)
{
	(void)filter_hwnd; (void)filter_min; (void)filter_max;

	if (!msg)
		return -1;

	while (1) {
		/* 1. WM_QUIT 체크 */
		if (msg_queue.quit_posted) {
			msg->hwnd = NULL;
			msg->message = WM_QUIT;
			msg->wParam = (WPARAM)msg_queue.quit_code;
			msg->lParam = 0;
			return FALSE;
		}

		/* 2. 큐에서 메시지 꺼내기 */
		if (msg_queue.count > 0) {
			*msg = msg_queue.messages[msg_queue.head];
			msg_queue.head = (msg_queue.head + 1) % MSG_QUEUE_SIZE;
			msg_queue.count--;
			return TRUE;
		}

		/* 3. 타이머 체크 (WM_PAINT보다 높은 우선순위) */
		{
			uint64_t now = get_monotonic_ms();

			for (int i = 0; i < MAX_TIMERS; i++) {
				if (!timer_table[i].active)
					continue;
				if (now >= timer_table[i].next_fire_ms) {
					MSG tm;

					memset(&tm, 0, sizeof(tm));
					tm.hwnd = timer_table[i].hwnd;
					tm.message = WM_TIMER;
					tm.wParam = (WPARAM)timer_table[i].timer_id;
					enqueue_msg(&tm);

					/* 다음 발화 시각 설정 */
					timer_table[i].next_fire_ms =
						now + timer_table[i].interval_ms;
				}
			}
		}

		if (msg_queue.count > 0)
			continue;

		/* 4. WM_PAINT 생성 (최저 우선순위) */
		for (int i = 0; i < MAX_WINDOWS; i++) {
			if (wnd_table[i].active && wnd_table[i].needs_paint) {
				MSG pm;

				memset(&pm, 0, sizeof(pm));
				pm.hwnd = (HWND)(uintptr_t)(i + HWND_OFFSET);
				pm.message = WM_PAINT;
				enqueue_msg(&pm);
				break;
			}
		}

		if (msg_queue.count > 0)
			continue;

		/* 5. poll()로 이벤트 대기 */
		struct pollfd fds[2];
		int nfds = 0;

		if (g_cdp && g_cdp->sock_fd >= 0) {
			fds[nfds].fd = g_cdp->sock_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}
		if (msg_pipe[0] >= 0) {
			fds[nfds].fd = msg_pipe[0];
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (nfds == 0) {
			usleep(10000);
			continue;
		}

		/* poll timeout: 가장 가까운 타이머까지 남은 시간 */
		int poll_timeout = 100;
		{
			uint64_t now = get_monotonic_ms();

			for (int i = 0; i < MAX_TIMERS; i++) {
				if (!timer_table[i].active)
					continue;
				int64_t remain = (int64_t)(timer_table[i].next_fire_ms - now);

				if (remain <= 0) {
					poll_timeout = 0;
					break;
				}
				if (remain < poll_timeout)
					poll_timeout = (int)remain;
			}
		}

		int ret = poll(fds, (nfds_t)nfds, poll_timeout);

		if (ret > 0) {
			for (int i = 0; i < nfds; i++) {
				if (!(fds[i].revents & POLLIN))
					continue;

				if (g_cdp && fds[i].fd == g_cdp->sock_fd) {
					/* CDP 이벤트 → 콜백 → 큐에 추가 */
					if (cdp_dispatch(g_cdp) < 0) {
						/* 컴포지터 연결 끊김 */
						fprintf(stderr,
							"user32: CDP 연결 끊김\n");
						g_cdp = NULL;
					}
				} else if (fds[i].fd == msg_pipe[0]) {
					/* self-pipe drain */
					char buf[64];

					while (read(msg_pipe[0], buf,
						    sizeof(buf)) > 0)
						;
				}
			}
		}
	}
}

/* --- TranslateMessage --- */

/*
 * WM_KEYDOWN에서 WM_CHAR 생성.
 *
 * 실제 Windows: ToUnicode()로 가상 키코드 → 문자 변환.
 * 우리 구현: CDP가 키 이벤트에 ASCII 변환 결과를 포함하므로,
 *            on_cdp_key에서 저장한 g_last_char를 사용.
 *
 * VK가 인쇄 가능 문자(알파벳, 숫자)인 경우
 * g_last_char가 없으면 VK 코드에서 소문자로 변환하여 대체.
 */
__attribute__((ms_abi))
static int32_t u32_TranslateMessage(const MSG *msg)
{
	if (!msg || msg->message != WM_KEYDOWN)
		return FALSE;

	char ch = g_last_char;

	g_last_char = 0;

	/* CDP character가 없으면 VK에서 추론 */
	if (ch == 0) {
		uint32_t vk = (uint32_t)msg->wParam;

		if (vk >= 'A' && vk <= 'Z')
			ch = (char)(vk + 32);     /* 소문자 */
		else if (vk >= '0' && vk <= '9')
			ch = (char)vk;
		else if (vk == VK_SPACE)
			ch = ' ';
		else if (vk == VK_RETURN)
			ch = '\r';
		else if (vk == VK_TAB)
			ch = '\t';
		else if (vk == VK_BACK)
			ch = '\b';
	}

	if (ch == 0)
		return FALSE;

	MSG char_msg;

	memset(&char_msg, 0, sizeof(char_msg));
	char_msg.hwnd = msg->hwnd;
	char_msg.message = WM_CHAR;
	char_msg.wParam = (WPARAM)(unsigned char)ch;
	enqueue_msg(&char_msg);

	return TRUE;
}

/* --- DispatchMessageA --- */

__attribute__((ms_abi))
static LRESULT u32_DispatchMessageA(const MSG *msg)
{
	if (!msg)
		return 0;

	struct wnd_entry *w = hwnd_to_wnd(msg->hwnd);

	if (!w || !w->wndproc)
		return 0;

	return w->wndproc(msg->hwnd, msg->message,
			  msg->wParam, msg->lParam);
}

/* --- PostQuitMessage --- */

__attribute__((ms_abi))
static void u32_PostQuitMessage(int exit_code)
{
	msg_queue.quit_posted = 1;
	msg_queue.quit_code = exit_code;
	wakeup_msg_loop();
}

/* --- PostMessageA --- */

__attribute__((ms_abi))
static int32_t u32_PostMessageA(HWND hwnd, UINT msg,
				WPARAM wParam, LPARAM lParam)
{
	MSG m;

	memset(&m, 0, sizeof(m));
	m.hwnd = hwnd;
	m.message = msg;
	m.wParam = wParam;
	m.lParam = lParam;

	if (enqueue_msg(&m) < 0)
		return FALSE;

	wakeup_msg_loop();
	return TRUE;
}

/* --- SendMessageA --- */

/*
 * WndProc 직접 호출 (동기).
 * PostMessageA와 달리 큐를 거치지 않음.
 */
__attribute__((ms_abi))
static LRESULT u32_SendMessageA(HWND hwnd, UINT msg,
				WPARAM wParam, LPARAM lParam)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w || !w->wndproc)
		return 0;

	return w->wndproc(hwnd, msg, wParam, lParam);
}

/* --- BeginPaint / EndPaint --- */

/*
 * BeginPaint — WM_PAINT 처리 시작
 *
 * 1. GDI32에 HDC 할당 요청
 * 2. 배경 지우기 (fErase)
 * 3. PAINTSTRUCT 채우기
 * 4. needs_paint 클리어
 */
__attribute__((ms_abi))
static HDC u32_BeginPaint(HWND hwnd, PAINTSTRUCT *ps)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w || !ps)
		return NULL;

	memset(ps, 0, sizeof(*ps));

	/* GDI32에 HDC 할당 */
	HDC hdc = gdi32_create_dc_for_window(hwnd, w->pixels,
					      w->width, w->height);
	if (!hdc)
		return NULL;

	ps->hdc = hdc;
	ps->fErase = TRUE;
	ps->rcPaint.left = 0;
	ps->rcPaint.top = 0;
	ps->rcPaint.right = w->width;
	ps->rcPaint.bottom = w->height;

	/* 배경 지우기 */
	struct wndclass_entry *wc = find_wndclass(w->class_name);

	if (wc && wc->hbr_background) {
		/*
		 * hbrBackground 해석:
		 *   실제 HBRUSH → GDI32의 brush 색상 사용
		 *   작은 정수 (COLOR_xxx + 1) → 흰색으로 대체
		 *   v0.1: 항상 흰색으로 간소화
		 */
		uint32_t bg_pixel = 0x00FFFFFF; /* 흰색 */

		for (int y = 0; y < w->height; y++) {
			for (int x = 0; x < w->width; x++)
				w->pixels[y * w->width + x] = bg_pixel;
		}
	}

	w->needs_paint = 0;
	return hdc;
}

/*
 * EndPaint — WM_PAINT 처리 완료
 *
 * HDC 해제 + CDP commit (컴포지터에 렌더링 완료 알림)
 */
__attribute__((ms_abi))
static int32_t u32_EndPaint(HWND hwnd, const PAINTSTRUCT *ps)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!ps || !ps->hdc)
		return FALSE;

	gdi32_release_dc(ps->hdc);

	/* CDP commit — 화면 갱신 */
	if (w && w->cdp_win && g_cdp)
		cdp_commit_to(g_cdp, w->cdp_win);

	return TRUE;
}

/* --- GetClientRect --- */

__attribute__((ms_abi))
static int32_t u32_GetClientRect(HWND hwnd, RECT *rect)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w || !rect)
		return FALSE;

	rect->left = 0;
	rect->top = 0;
	rect->right = w->width;
	rect->bottom = w->height;

	return TRUE;
}

/* --- InvalidateRect --- */

__attribute__((ms_abi))
static int32_t u32_InvalidateRect(HWND hwnd, const RECT *rect,
				  int32_t erase)
{
	(void)rect; (void)erase;

	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return FALSE;

	w->needs_paint = 1;
	return TRUE;
}

/* --- MessageBoxA --- */

/*
 * 간이 구현: stderr에 출력하고 IDOK 반환.
 * Phase 3에서 실제 모달 다이얼로그는 미구현.
 */
__attribute__((ms_abi))
static int u32_MessageBoxA(HWND hwnd, const char *text,
			   const char *caption, UINT type)
{
	(void)hwnd; (void)type;

	fprintf(stderr, "[MessageBox] %s: %s\n",
		caption ? caption : "(null)",
		text ? text : "(null)");

	return IDOK;
}

/* --- DefWindowProcA --- */

/*
 * 기본 윈도우 프로시저.
 * 앱이 처리하지 않은 메시지의 기본 동작.
 *
 * 실제 Windows DefWindowProc은 수백 개의 메시지를 처리하지만,
 * Phase 3에서는 핵심만 구현.
 */
__attribute__((ms_abi))
static LRESULT u32_DefWindowProcA(HWND hwnd, UINT msg,
				  WPARAM wParam, LPARAM lParam)
{
	(void)wParam; (void)lParam;

	switch (msg) {
	case WM_CLOSE:
		/* WM_CLOSE 기본 동작: 윈도우 파괴 */
		u32_DestroyWindow(hwnd);
		return 0;

	case WM_PAINT: {
		/* 기본: BeginPaint + EndPaint (배경만 그리기) */
		PAINTSTRUCT ps;
		HDC hdc = u32_BeginPaint(hwnd, &ps);

		if (hdc)
			u32_EndPaint(hwnd, &ps);
		return 0;
	}

	default:
		return 0;
	}
}

/* --- SetTimer --- */

/*
 * 타이머 등록: interval_ms 마다 WM_TIMER 메시지 생성.
 * 반환값: timer ID (0이면 실패).
 *
 * 실제 Windows: callback이 있으면 WM_TIMER 대신 콜백 호출.
 * Phase 3: callback은 무시하고 항상 WM_TIMER 전송.
 */
__attribute__((ms_abi))
static uintptr_t u32_SetTimer(HWND hwnd, uintptr_t id,
			      UINT interval, void *callback)
{
	(void)callback;

	/* 기존 타이머 갱신 검사 */
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timer_table[i].active &&
		    timer_table[i].hwnd == hwnd &&
		    timer_table[i].timer_id == id) {
			timer_table[i].interval_ms = interval;
			timer_table[i].next_fire_ms =
				get_monotonic_ms() + interval;
			return id;
		}
	}

	/* 새 슬롯 할당 */
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!timer_table[i].active) {
			timer_table[i].active = 1;
			timer_table[i].hwnd = hwnd;
			timer_table[i].timer_id = id ? id : (uintptr_t)(i + 1);
			timer_table[i].interval_ms = interval;
			timer_table[i].next_fire_ms =
				get_monotonic_ms() + interval;
			return timer_table[i].timer_id;
		}
	}

	return 0;  /* 테이블 가득 참 */
}

/* --- KillTimer --- */

__attribute__((ms_abi))
static int32_t u32_KillTimer(HWND hwnd, uintptr_t id)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (timer_table[i].active &&
		    timer_table[i].hwnd == hwnd &&
		    timer_table[i].timer_id == id) {
			timer_table[i].active = 0;
			return TRUE;
		}
	}
	return FALSE;
}

/* --- GetWindowLongA --- */

/*
 * 윈도우 속성 조회.
 *
 * 핵심 용도:
 *   GWL_WNDPROC: 기존 프로시저 저장 후 교체 (서브클래싱)
 *   GWLP_USERDATA: 앱별 데이터 저장 (this 포인터 등)
 *   GWL_STYLE: 스타일 플래그 조회
 */
__attribute__((ms_abi))
static LONG u32_GetWindowLongA(HWND hwnd, int index)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return 0;

	switch (index) {
	case GWL_WNDPROC:
		return (LONG)(uintptr_t)w->wndproc;
	case GWL_STYLE:
		return (LONG)w->style;
	case GWL_EXSTYLE:
		return (LONG)w->ex_style;
	case GWLP_USERDATA:
		return (LONG)w->user_data;
	default:
		return 0;
	}
}

/* --- SetWindowLongA --- */

__attribute__((ms_abi))
static LONG u32_SetWindowLongA(HWND hwnd, int index, LONG new_val)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return 0;

	LONG old = 0;

	switch (index) {
	case GWL_WNDPROC:
		old = (LONG)(uintptr_t)w->wndproc;
		w->wndproc = (WNDPROC)(uintptr_t)new_val;
		break;
	case GWL_STYLE:
		old = (LONG)w->style;
		w->style = (DWORD)new_val;
		break;
	case GWL_EXSTYLE:
		old = (LONG)w->ex_style;
		w->ex_style = (DWORD)new_val;
		break;
	case GWLP_USERDATA:
		old = (LONG)w->user_data;
		w->user_data = (uintptr_t)new_val;
		break;
	}

	return old;
}

/* --- IsWindow --- */

__attribute__((ms_abi))
static int32_t u32_IsWindow(HWND hwnd)
{
	return hwnd_to_wnd(hwnd) != NULL ? TRUE : FALSE;
}

/* --- IsWindowVisible --- */

__attribute__((ms_abi))
static int32_t u32_IsWindowVisible(HWND hwnd)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return FALSE;

	return w->visible ? TRUE : FALSE;
}

/* --- GetWindowRect --- */

/*
 * 스크린 좌표 기준 윈도우 사각형.
 * GetClientRect은 (0,0) 기준이지만,
 * GetWindowRect은 실제 화면 위치.
 */
__attribute__((ms_abi))
static int32_t u32_GetWindowRect(HWND hwnd, RECT *rect)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w || !rect)
		return FALSE;

	rect->left = w->x;
	rect->top = w->y;
	rect->right = w->x + w->width;
	rect->bottom = w->y + w->height;

	return TRUE;
}

/* --- SetWindowTextA --- */

__attribute__((ms_abi))
static int32_t u32_SetWindowTextA(HWND hwnd, const char *text)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return FALSE;

	if (text)
		strncpy(w->title, text, sizeof(w->title) - 1);
	else
		w->title[0] = '\0';

	return TRUE;
}

/* --- GetWindowTextA --- */

__attribute__((ms_abi))
static int u32_GetWindowTextA(HWND hwnd, char *buf, int max_count)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w || !buf || max_count <= 0)
		return 0;

	int len = (int)strlen(w->title);

	if (len >= max_count)
		len = max_count - 1;

	memcpy(buf, w->title, (size_t)len);
	buf[len] = '\0';

	return len;
}

/* --- MoveWindow --- */

__attribute__((ms_abi))
static int32_t u32_MoveWindow(HWND hwnd, int x, int y,
			      int width, int height, int32_t repaint)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);

	if (!w)
		return FALSE;

	w->x = x;
	w->y = y;
	w->width = width;
	w->height = height;

	if (repaint)
		w->needs_paint = 1;

	return TRUE;
}

/* --- SetFocus --- */

__attribute__((ms_abi))
static HWND u32_SetFocus(HWND hwnd)
{
	HWND old = g_focus_hwnd;

	if (hwnd && !hwnd_to_wnd(hwnd))
		return NULL;

	g_focus_hwnd = hwnd;
	return old;
}

/* --- GetFocus --- */

__attribute__((ms_abi))
static HWND u32_GetFocus(void)
{
	return g_focus_hwnd;
}

/* --- GetSystemMetrics --- */

/*
 * 시스템 정보 조회.
 * Phase 3: 기본 화면/아이콘 크기만 반환.
 */
__attribute__((ms_abi))
static int u32_GetSystemMetrics(int index)
{
	switch (index) {
	case SM_CXSCREEN:  return 800;   /* 기본 해상도 */
	case SM_CYSCREEN:  return 600;
	case SM_CXICON:    return 32;
	case SM_CYICON:    return 32;
	case SM_CXCURSOR:  return 32;
	case SM_CYCURSOR:  return 32;
	default:           return 0;
	}
}

/* --- LoadCursorA / LoadIconA --- */

/*
 * 스텁: 더미 핸들 반환.
 * Phase 3에서 실제 커서/아이콘 렌더링은 미구현.
 * RegisterClassA에 전달할 비NULL 값만 필요.
 */
__attribute__((ms_abi))
static HCURSOR u32_LoadCursorA(HANDLE instance, const char *name)
{
	(void)instance; (void)name;
	return (HCURSOR)(uintptr_t)0xCCCC0001;
}

__attribute__((ms_abi))
static HICON u32_LoadIconA(HANDLE instance, const char *name)
{
	(void)instance; (void)name;
	return (HICON)(uintptr_t)0xCCCC0002;
}

/* ============================================================
 * 내부 API — DXGI SwapChain 연동
 * ============================================================
 *
 * DXGI의 SwapChain::Present()가 사용:
 *   1. get_window_pixels → HWND의 픽셀 버퍼 포인터 획득
 *   2. 백버퍼를 memcpy
 *   3. commit_window → CDP commit (컴포지터에 프레임 전달)
 */

int user32_get_window_pixels(HWND hwnd, uint32_t **pixels,
			     int *width, int *height)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);
	if (!w) return -1;

	if (pixels) *pixels = w->pixels;
	if (width)  *width = w->width;
	if (height) *height = w->height;
	return 0;
}

void user32_commit_window(HWND hwnd)
{
	struct wnd_entry *w = hwnd_to_wnd(hwnd);
	if (!w) return;

	if (w->cdp_win && g_cdp)
		cdp_commit_to(g_cdp, w->cdp_win);
}

/* ============================================================
 * 스텁 테이블
 * ============================================================ */

#include "../../../include/stub_entry.h"

struct stub_entry user32_stub_table[] = {
	/* 윈도우 클래스 */
	{ "user32.dll", "RegisterClassA",    (void *)u32_RegisterClassA },

	/* 윈도우 관리 */
	{ "user32.dll", "CreateWindowExA",   (void *)u32_CreateWindowExA },
	{ "user32.dll", "DestroyWindow",     (void *)u32_DestroyWindow },
	{ "user32.dll", "ShowWindow",        (void *)u32_ShowWindow },
	{ "user32.dll", "UpdateWindow",      (void *)u32_UpdateWindow },

	/* 메시지 루프 */
	{ "user32.dll", "GetMessageA",       (void *)u32_GetMessageA },
	{ "user32.dll", "TranslateMessage",  (void *)u32_TranslateMessage },
	{ "user32.dll", "DispatchMessageA",  (void *)u32_DispatchMessageA },
	{ "user32.dll", "PostQuitMessage",   (void *)u32_PostQuitMessage },
	{ "user32.dll", "DefWindowProcA",    (void *)u32_DefWindowProcA },

	/* 메시지 전송 */
	{ "user32.dll", "PostMessageA",      (void *)u32_PostMessageA },
	{ "user32.dll", "SendMessageA",      (void *)u32_SendMessageA },

	/* 그리기 */
	{ "user32.dll", "BeginPaint",        (void *)u32_BeginPaint },
	{ "user32.dll", "EndPaint",          (void *)u32_EndPaint },
	{ "user32.dll", "InvalidateRect",    (void *)u32_InvalidateRect },

	/* 유틸리티 */
	{ "user32.dll", "GetClientRect",     (void *)u32_GetClientRect },
	{ "user32.dll", "MessageBoxA",       (void *)u32_MessageBoxA },

	/* 타이머 */
	{ "user32.dll", "SetTimer",          (void *)u32_SetTimer },
	{ "user32.dll", "KillTimer",         (void *)u32_KillTimer },

	/* 윈도우 속성 */
	{ "user32.dll", "GetWindowLongA",    (void *)u32_GetWindowLongA },
	{ "user32.dll", "SetWindowLongA",    (void *)u32_SetWindowLongA },
	{ "user32.dll", "IsWindow",          (void *)u32_IsWindow },
	{ "user32.dll", "IsWindowVisible",   (void *)u32_IsWindowVisible },
	{ "user32.dll", "GetWindowRect",     (void *)u32_GetWindowRect },
	{ "user32.dll", "SetWindowTextA",    (void *)u32_SetWindowTextA },
	{ "user32.dll", "GetWindowTextA",    (void *)u32_GetWindowTextA },
	{ "user32.dll", "MoveWindow",        (void *)u32_MoveWindow },

	/* 포커스 */
	{ "user32.dll", "SetFocus",          (void *)u32_SetFocus },
	{ "user32.dll", "GetFocus",          (void *)u32_GetFocus },

	/* 시스템 정보 */
	{ "user32.dll", "GetSystemMetrics",  (void *)u32_GetSystemMetrics },

	/* 리소스 스텁 */
	{ "user32.dll", "LoadCursorA",       (void *)u32_LoadCursorA },
	{ "user32.dll", "LoadIconA",         (void *)u32_LoadIconA },

	{ NULL, NULL, NULL }
};
