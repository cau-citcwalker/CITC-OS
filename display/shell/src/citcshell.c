/*
 * citcshell — CITC OS 데스크탑 셸 (태스크바 + 앱 런처)
 * ======================================================
 *
 * CITC OS의 데스크탑 셸입니다.
 * 화면 하단에 태스크바(패널)를 표시하고, 앱을 실행합니다.
 *
 * 데스크탑 셸이란?
 *   Wayland/X11에서 데스크탑 환경의 핵심 UI를 제공하는 프로그램.
 *   - Windows: explorer.exe (태스크바 + 시작 메뉴 + 파일 관리자)
 *   - macOS: Dock + Finder + Menu Bar
 *   - GNOME: gnome-shell (Activities + 패널 + 앱 런처)
 *   - KDE: plasmashell (패널 + 위젯 + 앱 런처)
 *
 *   핵심: 셸은 컴포지터와 별개의 프로세스!
 *     컴포지터 = 윈도우를 관리하고 합성하는 서버
 *     셸 = 태스크바/독/런처를 그리는 클라이언트
 *     둘은 프로토콜(Wayland/CDP)로 통신합니다.
 *
 * Wayland에서의 대응:
 *   이 프로그램            ↔  plasmashell / gnome-shell / waybar
 *   CDP_REQ_SET_PANEL      ↔  wlr-layer-shell (zwlr_layer_surface_v1)
 *   cdp_set_panel(bottom)  ↔  set_anchor(BOTTOM) + set_exclusive_zone()
 *   버튼 클릭 → fork+exec ↔  D-Bus 앱 런처 / xdg-desktop-portal
 *
 * 구조:
 *   1. CDP 컴포지터에 연결
 *   2. 패널 surface 생성 (화면 전체 너비 × 32px)
 *   3. CDP_REQ_SET_PANEL으로 "패널" 역할 선언
 *   4. 버튼/시계를 직접 픽셀로 그림
 *   5. poll() 루프: 클릭 이벤트 처리 + 1초마다 시계 업데이트
 *
 * 빌드:
 *   gcc -static -Wall -I../protocol -o citcshell citcshell.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/wait.h>

/*
 * CDP 클라이언트 라이브러리 (header-only)
 * cdp_connect, cdp_create_surface, cdp_set_panel, cdp_dispatch 등
 */
#include "cdp_client.h"

/*
 * .desktop 파일 파서 (Class 21)
 * /usr/share/applications/ 의 .desktop 파일에서 앱 목록을 읽어
 * 태스크바 버튼을 동적으로 생성합니다.
 */
#include "desktop_entry.h"

/*
 * 8×8 비트맵 폰트 (fbdraw에서 공유)
 * font8x8_basic[128][8] — ASCII 문자별 8바이트 비트맵
 */
#include "../../fbdraw/src/font8x8.h"

/* PSF2 폰트 (Class 61) */
#include "../../font/psf2.h"

static struct psf2_font g_shell_psf2;
static int g_shell_font_w = 8;
static int g_shell_font_h = 8;

/* ============================================================
 * 상수
 * ============================================================ */

#define PANEL_HEIGHT     32     /* 패널 높이 (픽셀) */
#define BTN_HEIGHT       22     /* 버튼 높이 */
#define BTN_MARGIN       8      /* 버튼 간격 */
#define BTN_PADDING      12     /* 버튼 내부 여백 (좌우) */
#define CLOCK_WIDTH      80     /* 시계 영역 너비 */

/* 색상 (XRGB8888) */
#define COL_PANEL_BG     0x002B2B3D   /* 패널 배경: 짙은 남색 */
#define COL_BTN_NORMAL   0x003D3D56   /* 버튼: 약간 밝은 남색 */
#define COL_BTN_HOVER    0x005A5A80   /* 버튼 호버: 더 밝은 남색 */
#define COL_BTN_LOGO     0x004488CC   /* 로고 버튼: 파란색 */
#define COL_TEXT_WHITE    0x00E8E8F0   /* 텍스트: 흰색 */
#define COL_TEXT_DIM      0x008888AA   /* 텍스트: 흐린색 (시계) */
#define COL_SEPARATOR     0x00444466   /* 구분선 */

/* ============================================================
 * 버튼 구조체
 * ============================================================
 *
 * 태스크바에 표시되는 각 버튼.
 * command가 NULL이면 표시만 하고 클릭 불가 (로고 등).
 */
struct button {
	int x, y, w, h;          /* 위치와 크기 */
	const char *label;        /* 표시 텍스트 */
	const char *command;      /* fork+exec할 경로 (NULL=비활성) */
	int hovered;              /* 마우스 hover 상태 */
};

/*
 * 버튼 목록
 *
 * 순서: [CITC OS] [앱1] [앱2] ...
 *
 * v0.1: 하드코딩
 * v0.2 (Class 21): .desktop 파일에서 동적으로 읽기
 */
#define MAX_BUTTONS  8
static struct button buttons[MAX_BUTTONS];
static int num_buttons;

/*
 * .desktop 파일에서 읽은 앱 정보 저장소 (Class 21)
 *
 * 왜 전역/정적인가?
 *   button 구조체의 label/command가 const char* 포인터이므로,
 *   .desktop에서 읽은 문자열이 button보다 오래 살아있어야 함.
 *   지역 변수에 저장하면 함수 반환 시 사라져서 dangling pointer!
 *   → 정적 배열에 저장하여 프로그램 종료까지 유지.
 */
static struct desktop_entry desktop_apps[MAX_DESKTOP_ENTRIES];

/*
 * 윈도우 목록 버튼 (Class 63)
 * 태스크바에 열린 윈도우를 표시.
 * 클릭하면 해당 윈도우에 포커스.
 */
#define MAX_WIN_BTNS  CDP_MAX_WINLIST
struct win_button {
	int x, y, w, h;
	uint32_t surface_id;
	char title[32];
	int minimized;
	int hovered;
};

static struct win_button win_btns[MAX_WIN_BTNS];
static int num_win_btns;
static int launcher_end_x;   /* 런처 버튼 끝 x 좌표 */

/* ============================================================
 * 전역 상태
 * ============================================================ */

static struct cdp_conn *conn;
static struct cdp_window *panel_win;
static int need_redraw;
static int running = 1;

/* ============================================================
 * SIGCHLD 핸들러 — 좀비 프로세스 방지
 * ============================================================
 *
 * fork()로 앱을 실행하면, 앱이 종료될 때 부모(citcshell)가
 * wait()을 해줘야 합니다. 안 하면 좀비 프로세스가 남음!
 *
 * 좀비(zombie) 프로세스란?
 *   이미 종료되었지만 부모가 종료 상태를 아직 안 읽은 프로세스.
 *   ps에서 "Z" 상태로 보임. 자원은 거의 안 차지하지만
 *   PID 슬롯을 점유하므로 누적되면 문제.
 *
 * SIGCHLD:
 *   자식 프로세스가 종료될 때 부모에게 전달되는 시그널.
 *   이 시그널을 처리하면서 waitpid()를 호출하면
 *   좀비를 자동으로 정리할 수 있습니다.
 *
 * WNOHANG:
 *   waitpid()에서 블로킹하지 않고 바로 리턴.
 *   남은 좀비가 없으면 0 또는 -1 리턴.
 */
static void sigchld_handler(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		; /* 모든 종료된 자식 회수 */
}

/* ============================================================
 * 그리기 함수들
 * ============================================================
 *
 * CDP 공유메모리 버퍼에 직접 그립니다.
 * compositor.c, cdp_demo.c와 동일한 패턴.
 *
 * 왜 그리기 라이브러리를 안 쓰나?
 *   실제 데스크탑에서는 Cairo, Pango 등의 라이브러리를 사용하지만,
 *   우리는 교육 목적이므로 직접 구현. 원리를 이해하는 것이 목표!
 */

/* 사각형 채우기 */
static void draw_rect(uint32_t *pixels, int width, int height,
		      int rx, int ry, int rw, int rh, uint32_t color)
{
	for (int y = ry; y < ry + rh; y++) {
		if (y < 0 || y >= height)
			continue;
		for (int x = rx; x < rx + rw; x++) {
			if (x >= 0 && x < width)
				pixels[y * width + x] = color;
		}
	}
}

/* 한 글자 그리기 (PSF2 우선, font8x8 폴백) */
static void draw_char(uint32_t *pixels, int width, int height,
		      int cx, int cy, char c, uint32_t color)
{
	if (g_shell_psf2.loaded) {
		psf2_draw_char(pixels, width, cx, cy, c, color,
			       &g_shell_psf2);
		return;
	}

	unsigned char ch = (unsigned char)c;

	if (ch > 127)
		return;

	for (int row = 0; row < 8; row++) {
		uint8_t bits = font8x8_basic[ch][row];

		for (int col = 0; col < 8; col++) {
			if (bits & (1 << col)) {
				int px = cx + col;
				int py = cy + row;

				if (px >= 0 && px < width &&
				    py >= 0 && py < height)
					pixels[py * width + px] = color;
			}
		}
	}
}

/* 문자열 그리기 */
static void draw_string(uint32_t *pixels, int width, int height,
			int sx, int sy, const char *str, uint32_t color)
{
	while (*str) {
		draw_char(pixels, width, height, sx, sy, *str, color);
		sx += g_shell_font_w;
		str++;
	}
}

/* ============================================================
 * 버튼 생성
 * ============================================================ */

static void add_button(int x, const char *label, const char *command)
{
	if (num_buttons >= MAX_BUTTONS)
		return;

	struct button *btn = &buttons[num_buttons];
	int label_len = (int)strlen(label);

	btn->x = x;
	btn->y = (PANEL_HEIGHT - BTN_HEIGHT) / 2;
	btn->w = label_len * g_shell_font_w + BTN_PADDING * 2;
	btn->h = BTN_HEIGHT;
	btn->label = label;
	btn->command = command;
	btn->hovered = 0;
	num_buttons++;
}

static int setup_buttons(void)
{
	int x = BTN_MARGIN;
	int app_count;

	/* 로고 버튼 (클릭 불가) */
	add_button(x, "CITC OS", NULL);
	x += buttons[num_buttons - 1].w + BTN_MARGIN;

	/* 구분선 자리 */
	x += 4;

	/*
	 * .desktop 파일에서 앱 목록 로드 (Class 21)
	 *
	 * /usr/share/applications/ 의 .desktop 파일을 읽어서
	 * Name=과 Exec= 필드로 버튼을 동적 생성.
	 *
	 * .desktop 파일이 없으면 하드코딩 폴백 사용.
	 * (부팅 환경에 따라 디렉토리가 없을 수 있으므로)
	 */
	app_count = load_desktop_entries(desktop_apps, MAX_DESKTOP_ENTRIES);

	if (app_count > 0) {
		/* .desktop 파일 기반 버튼 생성 */
		for (int i = 0; i < app_count && num_buttons < MAX_BUTTONS; i++) {
			add_button(x, desktop_apps[i].name,
				   desktop_apps[i].exec);
			x += buttons[num_buttons - 1].w + BTN_MARGIN;
		}
	} else {
		/* 폴백: 하드코딩 (이전 방식) */
		add_button(x, "Terminal", "/usr/bin/citcterm");
		x += buttons[num_buttons - 1].w + BTN_MARGIN;

		add_button(x, "Demo", "/usr/bin/cdp_demo");
		x += buttons[num_buttons - 1].w + BTN_MARGIN;
	}

	launcher_end_x = x;
	return x; /* 다음 사용 가능한 x 좌표 */
}

/* ============================================================
 * 윈도우 목록 갱신 (Class 63)
 * ============================================================ */

static void update_window_list(void)
{
	struct cdp_window_list wl;

	if (cdp_list_windows(conn, &wl) < 0)
		return;

	num_win_btns = 0;
	int x = launcher_end_x + 8; /* 구분선 뒤 */

	for (uint32_t i = 0; i < wl.count && (int)i < MAX_WIN_BTNS; i++) {
		struct win_button *wb = &win_btns[num_win_btns];
		int title_len = (int)strlen(wl.entries[i].title);

		if (title_len > 12)
			title_len = 12; /* 최대 12자 표시 */

		wb->x = x;
		wb->y = (PANEL_HEIGHT - BTN_HEIGHT) / 2;
		wb->w = title_len * g_shell_font_w + BTN_PADDING;
		wb->h = BTN_HEIGHT;
		wb->surface_id = wl.entries[i].surface_id;
		strncpy(wb->title, wl.entries[i].title,
			sizeof(wb->title) - 1);
		wb->title[sizeof(wb->title) - 1] = '\0';
		if (strlen(wb->title) > 12)
			wb->title[12] = '\0';
		wb->minimized = wl.entries[i].minimized;
		wb->hovered = 0;

		x += wb->w + 4;
		num_win_btns++;
	}
}

/* ============================================================
 * 패널 렌더링
 * ============================================================
 *
 * 전체 패널을 다시 그립니다:
 *   1. 배경색
 *   2. 버튼들 (로고 + 런처)
 *   3. 우측에 시계
 */
static void render_panel(void)
{
	if (!panel_win || !panel_win->pixels)
		return;

	uint32_t *px = panel_win->pixels;
	int w = (int)panel_win->width;
	int h = (int)panel_win->height;

	/* 1. 배경 */
	draw_rect(px, w, h, 0, 0, w, h, COL_PANEL_BG);

	/* 상단 1px 하이라이트 (입체감) */
	draw_rect(px, w, h, 0, 0, w, 1, COL_SEPARATOR);

	/* 2. 버튼들 */
	for (int i = 0; i < num_buttons; i++) {
		struct button *btn = &buttons[i];
		uint32_t bg_color;

		if (i == 0) {
			/* 로고 버튼 — 항상 파란색 */
			bg_color = COL_BTN_LOGO;
		} else if (btn->hovered) {
			bg_color = COL_BTN_HOVER;
		} else {
			bg_color = COL_BTN_NORMAL;
		}

		draw_rect(px, w, h, btn->x, btn->y,
			  btn->w, btn->h, bg_color);

		/* 버튼 텍스트 (중앙 정렬) */
		int text_x = btn->x + BTN_PADDING;
		int text_y = btn->y + (btn->h - g_shell_font_h) / 2;

		draw_string(px, w, h, text_x, text_y,
			    btn->label, COL_TEXT_WHITE);

		/* 로고 버튼 뒤 구분선 */
		if (i == 0) {
			int sep_x = btn->x + btn->w + BTN_MARGIN / 2;

			draw_rect(px, w, h, sep_x, 4, 1, h - 8,
				  COL_SEPARATOR);
		}
	}

	/* 2.5 윈도우 목록 구분선 + 버튼 (Class 63) */
	if (num_win_btns > 0) {
		/* 구분선 */
		int sep_x = launcher_end_x + 2;

		draw_rect(px, w, h, sep_x, 4, 1, h - 8,
			  COL_SEPARATOR);

		for (int i = 0; i < num_win_btns; i++) {
			struct win_button *wb = &win_btns[i];
			uint32_t bg_color = wb->hovered ? COL_BTN_HOVER :
					    0x00333350;

			draw_rect(px, w, h, wb->x, wb->y,
				  wb->w, wb->h, bg_color);

			int text_x = wb->x + BTN_PADDING / 2;
			int text_y = wb->y +
				     (wb->h - g_shell_font_h) / 2;

			draw_string(px, w, h, text_x, text_y,
				    wb->title,
				    wb->minimized ? COL_TEXT_DIM :
						    COL_TEXT_WHITE);
		}
	}

	/*
	 * 3. 시계 (우측)
	 *
	 * /proc/uptime에서 시스템 가동 시간을 읽습니다.
	 * 실제 데스크탑에서는 wall clock(localtime)을 표시하지만,
	 * initramfs에서는 RTC(하드웨어 시계)가 설정되지 않을 수 있으므로
	 * uptime이 더 신뢰할 수 있습니다.
	 *
	 * /proc/uptime 형식: "123.45 100.23\n"
	 *   첫 번째 숫자 = 전체 시스템 가동 시간 (초)
	 *   두 번째 숫자 = idle 시간 (초)
	 */
	char clock_buf[32];
	int uptime_sec = 0;
	FILE *f = fopen("/proc/uptime", "r");

	if (f) {
		double up;

		if (fscanf(f, "%lf", &up) == 1)
			uptime_sec = (int)up;
		fclose(f);
	}

	int hrs = uptime_sec / 3600;
	int mins = (uptime_sec % 3600) / 60;
	int secs = uptime_sec % 60;

	snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d:%02d",
		 hrs, mins, secs);

	int clock_x = w - (int)strlen(clock_buf) * g_shell_font_w - BTN_MARGIN;
	int clock_y = (h - g_shell_font_h) / 2;

	draw_string(px, w, h, clock_x, clock_y,
		    clock_buf, COL_TEXT_DIM);
}

/* ============================================================
 * 앱 실행 (fork + exec)
 * ============================================================
 *
 * 버튼을 클릭하면 새 프로세스를 만들어 앱을 실행합니다.
 *
 * fork()란?
 *   현재 프로세스를 복제하여 자식 프로세스를 생성.
 *   부모: fork() → 자식 PID 반환 (>0)
 *   자식: fork() → 0 반환
 *
 * execl()란?
 *   현재 프로세스를 새 프로그램으로 교체.
 *   fork() 후 자식에서 execl()을 호출하면
 *   자식이 완전히 새 프로그램으로 바뀜.
 *
 * fork + exec = Unix의 프로세스 생성 패턴:
 *   Windows: CreateProcess() 한 번으로 생성+실행
 *   Unix: fork()로 복제 → exec()로 교체 (2단계)
 *   이유: fork와 exec 사이에 fd 재배치, 환경 설정 등 가능
 */
static void launch_app(const char *path)
{
	printf("citcshell: launching %s\n", path);

	pid_t pid = fork();

	if (pid < 0) {
		perror("citcshell: fork");
		return;
	}

	if (pid == 0) {
		/* 자식 프로세스 — 새 앱으로 교체 */

		/*
		 * setsid(): 새 세션 생성
		 * 자식을 citcshell의 터미널 세션에서 분리.
		 * 이렇게 하면 citcshell이 종료되어도 자식이 살아남음.
		 */
		setsid();

		execl(path, path, NULL);

		/* execl 실패 시에만 여기 도달 */
		perror("citcshell: exec");
		_exit(1);
	}

	/* 부모 프로세스 — 계속 진행 */
	printf("citcshell: started PID %d\n", (int)pid);
}

/* ============================================================
 * CDP 이벤트 콜백
 * ============================================================ */

/*
 * 마우스 이동 — 버튼 hover 상태 업데이트
 *
 * 컴포지터가 패널 위에서 마우스가 움직일 때마다
 * surface-local 좌표와 함께 이 콜백을 호출합니다.
 */
static void on_pointer_motion(uint32_t surface_id, int x, int y)
{
	(void)surface_id;

	int changed = 0;

	for (int i = 0; i < num_buttons; i++) {
		struct button *btn = &buttons[i];
		int inside = (x >= btn->x && x < btn->x + btn->w &&
			      y >= btn->y && y < btn->y + btn->h);

		if (inside != btn->hovered) {
			btn->hovered = inside;
			changed = 1;
		}
	}

	/* 윈도우 버튼 hover (Class 63) */
	for (int i = 0; i < num_win_btns; i++) {
		struct win_button *wb = &win_btns[i];
		int inside = (x >= wb->x && x < wb->x + wb->w &&
			      y >= wb->y && y < wb->y + wb->h);

		if (inside != wb->hovered) {
			wb->hovered = inside;
			changed = 1;
		}
	}

	if (changed)
		need_redraw = 1;
}

/*
 * 마우스 버튼 — 클릭 감지 → 앱 실행
 *
 * state=1(누름)일 때 버튼 위에 있으면 해당 앱을 실행.
 * state=0(해제)는 무시.
 */
static void on_pointer_button(uint32_t surface_id, uint32_t button,
			      uint32_t state)
{
	(void)surface_id;
	(void)button;

	if (state != 1) /* 누름만 처리 */
		return;

	for (int i = 0; i < num_buttons; i++) {
		struct button *btn = &buttons[i];

		if (btn->hovered && btn->command) {
			launch_app(btn->command);
			return;
		}
	}

	/* 윈도우 버튼 클릭 → 포커스/복원 (Class 63) */
	for (int i = 0; i < num_win_btns; i++) {
		struct win_button *wb = &win_btns[i];

		if (wb->hovered && wb->surface_id > 0) {
			cdp_raise_surface(conn, wb->surface_id);
			return;
		}
	}
}

/* ============================================================
 * 메인
 * ============================================================ */

int main(void)
{
	printf("========================================\n");
	printf("  citcshell — CITC OS Desktop Shell\n");
	printf("========================================\n\n");

	/* SIGCHLD 핸들러 등록 (좀비 방지) */
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	/* 0. PSF2 폰트 로드 (Class 61) */
	if (psf2_load(&g_shell_psf2, "/usr/share/fonts/ter-116n.psf") == 0) {
		g_shell_font_w = (int)g_shell_psf2.width;
		g_shell_font_h = (int)g_shell_psf2.height;
		printf("citcshell: PSF2 폰트 로드 %ux%u\n",
		       g_shell_psf2.width, g_shell_psf2.height);
	} else {
		printf("citcshell: PSF2 없음 — font8x8 사용\n");
	}

	/* 1. CDP 컴포지터에 연결 */
	conn = cdp_connect();
	if (!conn) {
		fprintf(stderr, "citcshell: compositor에 연결할 수 없습니다\n");
		return 1;
	}

	printf("citcshell: 화면 크기 %ux%u\n",
	       conn->screen_width, conn->screen_height);

	/*
	 * 2. 패널 surface 생성
	 *
	 * 화면 전체 너비 × PANEL_HEIGHT 크기로 생성.
	 * 이 surface가 태스크바가 됩니다.
	 *
	 * 일반 surface로 생성한 뒤 SET_PANEL로 패널로 전환하는 이유:
	 *   Wayland의 layer-shell도 같은 패턴을 따릅니다.
	 *   먼저 surface를 만들고, 역할을 나중에 지정.
	 *   이렇게 하면 프로토콜이 확장 가능합니다.
	 */
	panel_win = cdp_create_surface(conn,
				       (int)conn->screen_width,
				       PANEL_HEIGHT,
				       "citcshell");
	if (!panel_win) {
		fprintf(stderr, "citcshell: surface 생성 실패\n");
		cdp_disconnect(conn);
		return 1;
	}

	/*
	 * 3. 패널 역할 선언
	 *
	 * 컴포지터에게 "이 surface는 하단 패널이야"라고 알림.
	 * 컴포지터는:
	 *   - 위치를 화면 하단으로 이동
	 *   - 크기를 화면 전체 너비로 조정
	 *   - 타이틀바/테두리 제거
	 *   - 항상 일반 윈도우 위에 표시
	 *   - 드래그 불가
	 */
	cdp_set_panel(conn, panel_win, 0 /* bottom */, PANEL_HEIGHT);

	/* 4. 버튼 배치 */
	setup_buttons();

	/* 이벤트 콜백 등록 */
	conn->on_pointer_motion = on_pointer_motion;
	conn->on_pointer_button = on_pointer_button;

	/* 5. 초기 렌더링 */
	render_panel();
	cdp_commit_to(conn, panel_win);

	printf("citcshell: 패널 준비 완료 (%ux%d)\n",
	       conn->screen_width, PANEL_HEIGHT);

	/*
	 * 6. 이벤트 루프
	 *
	 * poll() 기반 이벤트 루프.
	 * poll()은 "지정된 fd에 읽을 데이터가 올 때까지 대기"하는 시스템콜.
	 *
	 * 타임아웃 = 1000ms:
	 *   1초마다 시계를 업데이트하기 위해 타임아웃을 설정.
	 *   데이터가 오면 즉시 깨어나고(이벤트 처리),
	 *   1초 동안 아무 것도 없으면 시계 업데이트.
	 *
	 * 실제 Wayland 클라이언트의 이벤트 루프:
	 *   while (wl_display_dispatch() >= 0) { ... }
	 *   내부적으로 poll/epoll을 사용하여 소켓을 감시.
	 */
	struct pollfd pfd;

	pfd.fd = conn->sock_fd;
	pfd.events = POLLIN;

	while (running) {
		int ret = poll(&pfd, 1, 1000); /* 1초 타임아웃 */

		if (ret < 0) {
			if (errno == EINTR)
				continue; /* 시그널 인터럽트 → 재시도 */
			perror("citcshell: poll");
			break;
		}

		/* 소켓에 이벤트가 있으면 처리 */
		if (ret > 0 && (pfd.revents & POLLIN)) {
			if (cdp_dispatch(conn) < 0) {
				printf("citcshell: compositor 연결 끊김\n");
				break;
			}
		}

		/* 매 루프마다 시계 + 윈도우 목록 업데이트 */
		need_redraw = 1;

		if (need_redraw) {
			update_window_list();
			render_panel();
			cdp_commit_to(conn, panel_win);
			need_redraw = 0;
		}
	}

	/* 정리 */
	cdp_disconnect(conn);
	printf("citcshell: 종료\n");

	return 0;
}
