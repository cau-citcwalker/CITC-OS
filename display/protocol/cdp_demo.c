/*
 * CDP Demo — CITC Display Protocol 클라이언트 예제
 * =================================================
 *
 * 이 프로그램은 컴포지터와 **별도의 프로세스**로 실행됩니다.
 * 소켓을 통해 컴포지터에 연결하고:
 *   1. surface(윈도우) 생성 요청
 *   2. 공유메모리에 직접 픽셀 그리기
 *   3. commit하여 화면에 표시
 *   4. 입력 이벤트 수신 및 처리
 *
 * 이것이 모든 Wayland 앱의 기본 구조입니다!
 *
 * 실행 방법 (QEMU 시리얼 콘솔에서):
 *   compositor &       ← 컴포지터를 백그라운드로 실행
 *   sleep 2            ← 소켓 생성 대기
 *   cdp_demo           ← 이 프로그램 실행!
 *
 * 데모 내용:
 *   - 시간에 따라 변하는 그라디언트 배경 (애니메이션)
 *   - 키보드 입력 → 텍스트 표시
 *   - 마우스 위치 → 십자선 그리기
 *   - ESC → 종료
 *
 * 빌드:
 *   gcc -static -Wall -o cdp_demo cdp_demo.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <linux/input-event-codes.h>   /* KEY_ESC 등 */

#include "cdp_client.h"

/* ============================================================
 * 전역 상태
 * ============================================================ */

static struct cdp_conn *conn;
static struct cdp_window *win;
static int running = 1;

/* 애니메이션 프레임 카운터 */
static uint32_t frame_count;

/* 마우스 위치 (surface-local) */
static int mouse_x = -1, mouse_y = -1;

/* 텍스트 입력 버퍼 */
#define TEXT_MAX 128
static char text_buf[TEXT_MAX];
static int text_len;

/* 8x8 비트맵 폰트 — 컴포지터와 동일한 폰트 사용 */
#include "../fbdraw/src/font8x8.h"

/* ============================================================
 * 그리기 함수들
 * ============================================================
 *
 * 클라이언트는 자기 공유메모리 버퍼에 직접 그립니다.
 * 컴포지터는 이 메모리를 읽어서 화면에 합성합니다.
 *
 * Wayland에서 실제 앱은 Cairo, Skia, OpenGL 등을 사용하지만,
 * 여기서는 직접 픽셀을 조작하여 원리를 이해합니다.
 */

static inline uint32_t make_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* 사각형 채우기 */
static void fill_rect(uint32_t *px, int w, int h,
		      int rx, int ry, int rw, int rh, uint32_t color)
{
	for (int y = ry; y < ry + rh && y < h; y++) {
		if (y < 0) continue;
		for (int x = rx; x < rx + rw && x < w; x++) {
			if (x < 0) continue;
			px[y * w + x] = color;
		}
	}
}

/* 한 글자 그리기 (8x8 폰트) */
static void draw_char_at(uint32_t *px, int w, int h,
			 int cx, int cy, char ch, uint32_t color)
{
	if (ch < 32 || ch > 126)
		return;

	const uint8_t *glyph = font8x8_basic[(int)ch];

	for (int row = 0; row < 8; row++) {
		int py = cy + row;

		if (py < 0 || py >= h)
			continue;

		for (int col = 0; col < 8; col++) {
			int px_x = cx + col;

			if (px_x < 0 || px_x >= w)
				continue;

			if (glyph[row] & (1 << col))
				px[py * w + px_x] = color;
		}
	}
}

/* 문자열 그리기 */
static void draw_text(uint32_t *px, int w, int h,
		      int tx, int ty, const char *str, uint32_t color)
{
	while (*str) {
		draw_char_at(px, w, h, tx, ty, *str, color);
		tx += 8;
		str++;
	}
}

/* ============================================================
 * 프레임 렌더링
 * ============================================================
 *
 * 이 함수가 "앱의 화면 그리기"에 해당합니다.
 * Wayland 앱이 wl_buffer에 그리는 것과 동일!
 *
 * pixels 포인터는 공유메모리를 가리키고,
 * 여기에 그린 내용이 컴포지터에 의해 화면에 합성됩니다.
 */
static void render(void)
{
	uint32_t *px = win->pixels;
	int w = (int)win->width;
	int h = (int)win->height;

	/*
	 * 1. 배경: 시간에 따라 변하는 그라디언트
	 *
	 * frame_count를 사용하여 색상이 서서히 변합니다.
	 * 이것이 "프레임 콜백으로 애니메이션하기"의 핵심!
	 * 매 프레임마다 약간 다른 색상 → 움직이는 효과
	 */
	uint32_t phase = frame_count * 2;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t r = (uint8_t)((x + phase) % 256);
			uint8_t g = (uint8_t)((y + phase / 3) % 200);
			uint8_t b = (uint8_t)(100 + (x + y + phase) % 100);

			/* 약간 어둡게 (배경이 너무 밝지 않게) */
			r = r / 3 + 20;
			g = g / 3 + 20;
			b = b / 4 + 40;

			px[y * w + x] = make_rgb(r, g, b);
		}
	}

	/* 2. 상단에 정보 표시 */
	fill_rect(px, w, h, 0, 0, w, 14, make_rgb(10, 10, 30));
	char info[64];

	snprintf(info, sizeof(info), "CDP Demo  Frame:%u  Mouse:%d,%d",
		 frame_count, mouse_x, mouse_y);
	draw_text(px, w, h, 4, 3, info, make_rgb(180, 200, 255));

	/* 3. 텍스트 입력 영역 */
	fill_rect(px, w, h, 4, h - 22, w - 8, 18, make_rgb(20, 20, 40));

	if (text_len > 0) {
		draw_text(px, w, h, 8, h - 18, text_buf,
			  make_rgb(200, 220, 200));
		/* 커서 */
		int cursor_x = 8 + text_len * 8;

		draw_char_at(px, w, h, cursor_x, h - 18, '_',
			     make_rgb(255, 255, 100));
	} else {
		draw_text(px, w, h, 8, h - 18, "Type here..._",
			  make_rgb(100, 100, 120));
	}

	/*
	 * 4. 마우스 십자선
	 *
	 * 컴포지터가 보내준 surface-local 좌표를 사용.
	 * 실제 Wayland에서 wl_pointer.motion이 보내는 좌표와 동일한 개념.
	 */
	if (mouse_x >= 0 && mouse_y >= 0 &&
	    mouse_x < w && mouse_y < h) {
		uint32_t cross_color = make_rgb(255, 255, 0);

		/* 수평선 */
		for (int x = mouse_x - 8; x <= mouse_x + 8; x++) {
			if (x >= 0 && x < w)
				px[mouse_y * w + x] = cross_color;
		}
		/* 수직선 */
		for (int y = mouse_y - 8; y <= mouse_y + 8; y++) {
			if (y >= 0 && y < h)
				px[y * w + mouse_x] = cross_color;
		}
	}

	frame_count++;
}

/* ============================================================
 * 이벤트 콜백
 * ============================================================
 *
 * 컴포지터가 보내는 이벤트를 처리.
 * Wayland: wl_keyboard.key, wl_pointer.motion 등의 리스너.
 */

static void on_key(uint32_t keycode, uint32_t state, char ch)
{
	/* press(1) 또는 repeat(2)만 처리 */
	if (state == 0)
		return;

	/* ESC → 종료 */
	if (keycode == KEY_ESC) {
		running = 0;
		return;
	}

	/* Backspace */
	if (keycode == KEY_BACKSPACE) {
		if (text_len > 0) {
			text_len--;
			text_buf[text_len] = '\0';
		}
		return;
	}

	/* Enter */
	if (keycode == KEY_ENTER) {
		if (text_len < TEXT_MAX - 1) {
			text_buf[text_len++] = ' ';
			text_buf[text_len] = '\0';
		}
		return;
	}

	/* 일반 문자 */
	if (ch && text_len < TEXT_MAX - 1) {
		text_buf[text_len++] = ch;
		text_buf[text_len] = '\0';
	}
}

static void on_pointer_motion(uint32_t surface_id, int x, int y)
{
	(void)surface_id;
	mouse_x = x;
	mouse_y = y;
}

static void on_frame(uint32_t surface_id)
{
	(void)surface_id;

	/*
	 * 프레임 콜백이 왔다 = 컴포지터가 이전 프레임을 화면에 표시했다
	 *
	 * 이제 다음 프레임을 그리고 commit할 차례!
	 *
	 * Wayland 렌더링 루프:
	 *   1. 프레임 콜백 요청 (wl_surface.frame)
	 *   2. [대기]
	 *   3. 콜백 수신 (wl_callback.done)
	 *   4. 그리기
	 *   5. commit (wl_surface.commit)
	 *   → 1로 돌아감
	 */
	render();
	cdp_commit_to(conn, win);
	cdp_request_frame(conn, win);
}

/* ============================================================
 * 메인 함수
 * ============================================================ */

int main(void)
{
	printf("\n");
	printf("=== CDP Demo Client ===\n\n");

	/*
	 * 1. 컴포지터에 연결
	 * Wayland: wl_display_connect(NULL)
	 */
	printf("[1/3] 컴포지터에 연결...\n");
	conn = cdp_connect();
	if (!conn) {
		printf("연결 실패! compositor가 실행 중인지 확인하세요.\n");
		printf("  compositor &\n");
		printf("  sleep 2\n");
		printf("  cdp_demo\n");
		return 1;
	}

	/*
	 * 2. Surface(윈도우) 생성
	 * Wayland: wl_compositor_create_surface + xdg_toplevel
	 */
	printf("[2/3] Surface 생성...\n");
	win = cdp_create_surface(conn, 300, 180, "CDP Demo");
	if (!win) {
		printf("Surface 생성 실패!\n");
		cdp_disconnect(conn);
		return 1;
	}

	/*
	 * 3. 이벤트 콜백 설정
	 * Wayland: wl_keyboard_listener, wl_pointer_listener 등록
	 */
	printf("[3/3] 이벤트 콜백 설정...\n\n");
	conn->on_key = on_key;
	conn->on_pointer_motion = on_pointer_motion;
	conn->on_frame = on_frame;

	/*
	 * 4. 첫 프레임 그리기 + commit + 프레임 콜백 요청
	 *
	 * 첫 프레임은 직접 그리고 commit합니다.
	 * 이후 프레임은 frame callback을 통해 자동으로 그려집니다.
	 */
	render();
	cdp_commit_to(conn, win);
	cdp_request_frame(conn, win);

	printf("CDP Demo 시작! (ESC로 종료)\n");
	printf("  키보드 입력 → 텍스트 표시\n");
	printf("  마우스 이동 → 십자선\n\n");

	/*
	 * 5. 이벤트 루프
	 * Wayland: while (wl_display_dispatch() >= 0) { ... }
	 *
	 * cdp_dispatch()는 서버에서 오는 이벤트를 하나씩 읽고
	 * 등록된 콜백을 호출합니다.
	 * 연결이 끊기면 -1을 반환합니다.
	 */
	while (running && cdp_dispatch(conn) >= 0)
		;

	/* 6. 정리 */
	printf("\nCDP Demo 종료.\n");
	cdp_destroy_surface(conn, win);
	cdp_disconnect(conn);

	return 0;
}
