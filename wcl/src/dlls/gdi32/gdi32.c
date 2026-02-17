/*
 * gdi32.c — CITC OS gdi32.dll 구현
 * ==================================
 *
 * Windows GDI (Graphics Device Interface).
 *
 * GDI는 Win32의 2D 그래픽 엔진:
 *   앱 → GetDC(hwnd) → HDC 획득
 *     → TextOutA(hdc, ...) 등 그리기
 *     → ReleaseDC(hwnd, hdc)
 *
 * HDC (Device Context)는 상태 머신:
 *   - text_color: TextOutA의 글자 색
 *   - bk_color:   배경 색 (OPAQUE 모드)
 *   - bk_mode:    TRANSPARENT(배경 안 그림) / OPAQUE(배경 그림)
 *   - brush:      도형 채우기 색
 *
 * Linux 대응:
 *   HDC ≈ Cairo context (cairo_t) 또는 X11 GC (Graphics Context)
 *   둘 다 "그리기 대상 + 상태"를 묶는 개념.
 *
 * 폰트:
 *   display/fbdraw/src/font8x8.h의 8x8 비트맵 폰트 재사용.
 *   실제 Windows GDI는 TrueType/OpenType 폰트 래스터라이저를 가지지만,
 *   Phase 3에서는 교육적 단순함을 위해 고정폭 비트맵 폰트 사용.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "../../../include/win32.h"

/* 8x8 비트맵 폰트 (compositor와 공유) */
#include "../../../../display/fbdraw/src/font8x8.h"

/* PSF2 폰트 (Class 61) */
#include "../../../../display/font/psf2.h"

static struct psf2_font g_gdi_psf2;
static int g_gdi_font_w = 8;
static int g_gdi_font_h = 8;
static int g_gdi_psf2_init = 0;

/* PSF2 폰트 지연 로드 (최초 TextOut 호출 시) */
static void gdi32_ensure_font(void)
{
	if (g_gdi_psf2_init)
		return;
	g_gdi_psf2_init = 1;
	if (psf2_load(&g_gdi_psf2, "/usr/share/fonts/ter-116n.psf") == 0) {
		g_gdi_font_w = (int)g_gdi_psf2.width;
		g_gdi_font_h = (int)g_gdi_psf2.height;
	}
}

/* ============================================================
 * Device Context 테이블
 * ============================================================
 *
 * HDC = (void*)(index + HDC_OFFSET)
 * HANDLE/HWND와 범위가 겹치지 않도록 오프셋 사용.
 *
 * 실제 Windows:
 *   HDC는 GDI32.dll 내부 테이블에 저장 (win32k.sys)
 *   HANDLE과는 다른 네임스페이스
 */
#define MAX_DCS     128
#define HDC_OFFSET  0x20000

struct dc_entry {
	int active;
	HWND hwnd;
	uint32_t *pixels;
	int width, height;

	/* GDI 상태 머신 */
	COLORREF text_color;
	COLORREF bk_color;
	int bk_mode;
	COLORREF brush_color;
};

static struct dc_entry dc_table[MAX_DCS];

/* ============================================================
 * GDI 오브젝트 테이블 (브러시 등)
 * ============================================================
 *
 * HBRUSH = (void*)(index + HGDI_OFFSET)
 */
#define MAX_GDI_OBJECTS 64
#define HGDI_OFFSET     0x30000

enum gdi_obj_type {
	GDI_FREE = 0,
	GDI_BRUSH,
};

struct gdi_object {
	enum gdi_obj_type type;
	COLORREF color;
};

static struct gdi_object gdi_obj_table[MAX_GDI_OBJECTS];

/* ============================================================
 * 스톡 오브젝트 테이블
 * ============================================================
 *
 * GetStockObject(index)로 반환하는 시스템 GDI 오브젝트.
 * 앱이 DeleteObject()로 해제하면 안 됨 (시스템 소유).
 *
 * HGDIOBJ = (void*)(STOCK_OFFSET + index) 형태로 반환.
 * SelectObject에서 STOCK_OFFSET 범위를 인식하여 처리.
 */
#define STOCK_OFFSET     0x40000
#define MAX_STOCK_OBJECTS 20

enum { STOCK_BRUSH = 1, STOCK_PEN = 2, STOCK_FONT = 3 };

static struct {
	int type;        /* STOCK_BRUSH, STOCK_PEN, STOCK_FONT */
	COLORREF color;
} stock_objects[MAX_STOCK_OBJECTS] = {
	[WHITE_BRUSH]      = { STOCK_BRUSH, 0x00FFFFFF },
	[LTGRAY_BRUSH]     = { STOCK_BRUSH, 0x00C0C0C0 },
	[GRAY_BRUSH]       = { STOCK_BRUSH, 0x00808080 },
	[DKGRAY_BRUSH]     = { STOCK_BRUSH, 0x00404040 },
	[BLACK_BRUSH]      = { STOCK_BRUSH, 0x00000000 },
	[NULL_BRUSH]       = { STOCK_BRUSH, 0x00000000 },  /* 투명 */
	[WHITE_PEN]        = { STOCK_PEN,   0x00FFFFFF },
	[BLACK_PEN]        = { STOCK_PEN,   0x00000000 },
	[NULL_PEN]         = { STOCK_PEN,   0x00000000 },
	[SYSTEM_FONT]      = { STOCK_FONT,  0x00000000 },
	[DEFAULT_GUI_FONT] = { STOCK_FONT,  0x00000000 },
};

/* ============================================================
 * 내부 유틸리티
 * ============================================================ */

static struct dc_entry *hdc_to_dc(HDC hdc)
{
	uintptr_t val = (uintptr_t)hdc;

	if (val < HDC_OFFSET)
		return NULL;

	int idx = (int)(val - HDC_OFFSET);

	if (idx < 0 || idx >= MAX_DCS)
		return NULL;
	if (!dc_table[idx].active)
		return NULL;

	return &dc_table[idx];
}

static HDC alloc_dc(void)
{
	for (int i = 0; i < MAX_DCS; i++) {
		if (!dc_table[i].active) {
			memset(&dc_table[i], 0, sizeof(dc_table[i]));
			dc_table[i].active = 1;
			dc_table[i].text_color = RGB(0, 0, 0);
			dc_table[i].bk_color = RGB(255, 255, 255);
			dc_table[i].bk_mode = OPAQUE;
			dc_table[i].brush_color = RGB(255, 255, 255);
			return (HDC)(uintptr_t)(i + HDC_OFFSET);
		}
	}
	return NULL;
}

static struct gdi_object *hobj_to_obj(HGDIOBJ h)
{
	uintptr_t val = (uintptr_t)h;

	if (val < HGDI_OFFSET)
		return NULL;

	int idx = (int)(val - HGDI_OFFSET);

	if (idx < 0 || idx >= MAX_GDI_OBJECTS)
		return NULL;
	if (gdi_obj_table[idx].type == GDI_FREE)
		return NULL;

	return &gdi_obj_table[idx];
}

/* COLORREF(BGR) → XRGB8888 픽셀 값 변환 */
static uint32_t colorref_to_pixel(COLORREF c)
{
	return ((uint32_t)GetRValue(c) << 16) |
	       ((uint32_t)GetGValue(c) << 8) |
	       ((uint32_t)GetBValue(c));
}

/* ============================================================
 * 내부 API — user32에서 호출
 * ============================================================ */

HDC gdi32_create_dc_for_window(HWND hwnd, uint32_t *pixels,
			       int width, int height)
{
	HDC hdc = alloc_dc();

	if (!hdc)
		return NULL;

	struct dc_entry *dc = hdc_to_dc(hdc);

	dc->hwnd = hwnd;
	dc->pixels = pixels;
	dc->width = width;
	dc->height = height;

	return hdc;
}

void gdi32_release_dc(HDC hdc)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (dc)
		dc->active = 0;
}

/* ============================================================
 * GDI32 API 함수
 * ============================================================ */

/* --- GetDC / ReleaseDC --- */

/*
 * GetDC는 user32에서 호출되는 것이 정상이지만,
 * gdi32.dll export이기도 함.
 * 여기서는 HWND에서 픽셀 버퍼를 가져와야 하므로
 * user32의 내부 도움이 필요. 간이 구현.
 */
__attribute__((ms_abi))
static HDC g32_GetDC(HWND hwnd)
{
	(void)hwnd;
	/* user32 연동 없이는 제한적. BeginPaint 경로 사용 권장 */
	return NULL;
}

__attribute__((ms_abi))
static int32_t g32_ReleaseDC(HWND hwnd, HDC hdc)
{
	(void)hwnd;
	gdi32_release_dc(hdc);
	return 1;
}

/* --- TextOutA --- */

/*
 * TextOutA — 텍스트 출력
 *
 * font8x8 비트맵 폰트로 픽셀 버퍼에 직접 렌더링.
 * 각 글자: 8x8 픽셀, 1비트 = 1픽셀.
 *
 * 실제 Windows GDI는 TrueType 래스터라이저를 사용하지만,
 * 교육용 v0.1에서는 고정폭 비트맵 폰트로 충분.
 */
__attribute__((ms_abi))
static int32_t g32_TextOutA(HDC hdc, int x, int y,
			    const char *text, int len)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc || !dc->pixels || !text)
		return FALSE;

	gdi32_ensure_font();

	uint32_t fg = colorref_to_pixel(dc->text_color);
	uint32_t bg = colorref_to_pixel(dc->bk_color);

	for (int c = 0; c < len && text[c]; c++) {
		unsigned char ch = (unsigned char)text[c];

		if (ch > 127)
			ch = '?';

		if (g_gdi_psf2.loaded) {
			/* OPAQUE: 배경 먼저 채우기 */
			if (dc->bk_mode == OPAQUE) {
				for (int row = 0; row < g_gdi_font_h; row++) {
					int py = y + row;
					if (py < 0 || py >= dc->height)
						continue;
					for (int col = 0; col < g_gdi_font_w; col++) {
						int px = x + c * g_gdi_font_w + col;
						if (px >= 0 && px < dc->width)
							dc->pixels[py * dc->width + px] = bg;
					}
				}
			}
			psf2_draw_char(dc->pixels, dc->width,
				       x + c * g_gdi_font_w, y,
				       (char)ch, fg, &g_gdi_psf2);
		} else {
			const uint8_t *glyph =
				(const uint8_t *)font8x8_basic[ch];

			for (int row = 0; row < 8; row++) {
				int py = y + row;

				if (py < 0 || py >= dc->height)
					continue;

				for (int col = 0; col < 8; col++) {
					int px = x + c * 8 + col;

					if (px < 0 || px >= dc->width)
						continue;

					if (glyph[row] & (1 << col)) {
						dc->pixels[py * dc->width + px] = fg;
					} else if (dc->bk_mode == OPAQUE) {
						dc->pixels[py * dc->width + px] = bg;
					}
				}
			}
		}
	}

	return TRUE;
}

/* --- SetPixel / GetPixel --- */

__attribute__((ms_abi))
static COLORREF g32_SetPixel(HDC hdc, int x, int y, COLORREF color)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc || !dc->pixels)
		return CLR_INVALID;
	if (x < 0 || x >= dc->width || y < 0 || y >= dc->height)
		return CLR_INVALID;

	dc->pixels[y * dc->width + x] = colorref_to_pixel(color);
	return color;
}

__attribute__((ms_abi))
static COLORREF g32_GetPixel(HDC hdc, int x, int y)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc || !dc->pixels)
		return CLR_INVALID;
	if (x < 0 || x >= dc->width || y < 0 || y >= dc->height)
		return CLR_INVALID;

	uint32_t px = dc->pixels[y * dc->width + x];

	return RGB((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
}

/* --- Rectangle --- */

__attribute__((ms_abi))
static int32_t g32_Rectangle(HDC hdc, int left, int top,
			     int right, int bottom)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc || !dc->pixels)
		return FALSE;

	uint32_t px = colorref_to_pixel(dc->brush_color);

	/* 수평선 (상단, 하단) */
	for (int x = left; x < right; x++) {
		if (x >= 0 && x < dc->width) {
			if (top >= 0 && top < dc->height)
				dc->pixels[top * dc->width + x] = px;
			if (bottom - 1 >= 0 && bottom - 1 < dc->height)
				dc->pixels[(bottom - 1) * dc->width + x] = px;
		}
	}

	/* 수직선 (좌측, 우측) */
	for (int y = top; y < bottom; y++) {
		if (y >= 0 && y < dc->height) {
			if (left >= 0 && left < dc->width)
				dc->pixels[y * dc->width + left] = px;
			if (right - 1 >= 0 && right - 1 < dc->width)
				dc->pixels[y * dc->width + right - 1] = px;
		}
	}

	return TRUE;
}

/* --- FillRect --- */

/*
 * FillRect는 실제 Windows에서 user32.dll export이지만,
 * 구현은 GDI 픽셀 버퍼를 직접 조작하므로 여기에 배치.
 * stub table에서는 user32.dll로 등록.
 */
__attribute__((ms_abi))
static int32_t g32_FillRect(HDC hdc, const RECT *rect, HBRUSH brush)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc || !dc->pixels || !rect)
		return 0;

	COLORREF color = dc->brush_color;
	struct gdi_object *obj = hobj_to_obj(brush);

	if (obj && obj->type == GDI_BRUSH)
		color = obj->color;

	uint32_t px = colorref_to_pixel(color);

	for (int y = rect->top; y < rect->bottom; y++) {
		if (y < 0 || y >= dc->height)
			continue;
		for (int x = rect->left; x < rect->right; x++) {
			if (x < 0 || x >= dc->width)
				continue;
			dc->pixels[y * dc->width + x] = px;
		}
	}

	return 1;
}

/* --- GDI 오브젝트 관리 --- */

__attribute__((ms_abi))
static HBRUSH g32_CreateSolidBrush(COLORREF color)
{
	for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
		if (gdi_obj_table[i].type == GDI_FREE) {
			gdi_obj_table[i].type = GDI_BRUSH;
			gdi_obj_table[i].color = color;
			return (HBRUSH)(uintptr_t)(i + HGDI_OFFSET);
		}
	}
	return NULL;
}

__attribute__((ms_abi))
static int32_t g32_DeleteObject(HGDIOBJ obj)
{
	/* 스톡 오브젝트는 삭제 불가 (시스템 소유) */
	uintptr_t val = (uintptr_t)obj;

	if (val >= STOCK_OFFSET && val < STOCK_OFFSET + MAX_STOCK_OBJECTS)
		return TRUE;  /* 성공으로 반환하되 실제 삭제 안 함 */

	struct gdi_object *o = hobj_to_obj(obj);

	if (!o)
		return FALSE;

	o->type = GDI_FREE;
	return TRUE;
}

__attribute__((ms_abi))
static HGDIOBJ g32_SelectObject(HDC hdc, HGDIOBJ obj)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc)
		return NULL;

	uintptr_t val = (uintptr_t)obj;

	/* 스톡 오브젝트 범위 체크 */
	if (val >= STOCK_OFFSET && val < STOCK_OFFSET + MAX_STOCK_OBJECTS) {
		int idx = (int)(val - STOCK_OFFSET);

		if (stock_objects[idx].type == STOCK_BRUSH)
			dc->brush_color = stock_objects[idx].color;
		return NULL;
	}

	/* 일반 GDI 오브젝트 */
	struct gdi_object *o = hobj_to_obj(obj);

	if (!o)
		return NULL;

	if (o->type == GDI_BRUSH)
		dc->brush_color = o->color;

	return NULL;
}

/* --- DC 상태 설정 --- */

__attribute__((ms_abi))
static COLORREF g32_SetTextColor(HDC hdc, COLORREF color)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc)
		return CLR_INVALID;

	COLORREF old = dc->text_color;

	dc->text_color = color;
	return old;
}

__attribute__((ms_abi))
static COLORREF g32_SetBkColor(HDC hdc, COLORREF color)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc)
		return CLR_INVALID;

	COLORREF old = dc->bk_color;

	dc->bk_color = color;
	return old;
}

__attribute__((ms_abi))
static int32_t g32_SetBkMode(HDC hdc, int mode)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc)
		return 0;

	int old = dc->bk_mode;

	dc->bk_mode = mode;
	return old;
}

/* --- GetStockObject --- */

/*
 * 시스템 미리 정의 GDI 오브젝트 반환.
 * STOCK_OFFSET + index 형태의 특수 HGDIOBJ.
 */
__attribute__((ms_abi))
static HGDIOBJ g32_GetStockObject(int index)
{
	if (index < 0 || index >= MAX_STOCK_OBJECTS)
		return NULL;

	if (stock_objects[index].type == 0)
		return NULL;

	return (HGDIOBJ)(uintptr_t)(STOCK_OFFSET + index);
}

/* --- DrawTextA --- */

/*
 * 포맷된 텍스트 출력.
 *
 * TextOutA의 확장판:
 *   DT_SINGLELINE: 한 줄로 출력 (\n 무시)
 *   DT_CENTER: 수평 중앙 정렬
 *   DT_VCENTER + DT_SINGLELINE: 수직 중앙 정렬
 *   DT_CALCRECT: 실제로 그리지 않고 필요한 RECT만 계산
 *
 * 반환값: 텍스트 높이 (픽셀).
 */
__attribute__((ms_abi))
static int g32_DrawTextA(HDC hdc, const char *text, int count,
			 RECT *rect, UINT format)
{
	struct dc_entry *dc = hdc_to_dc(hdc);

	if (!dc || !text || !rect)
		return 0;

	if (count < 0) {
		count = 0;
		while (text[count])
			count++;
	}

	gdi32_ensure_font();

	int font_w = g_gdi_font_w, font_h = g_gdi_font_h;
	int text_w = count * font_w;
	int text_h = font_h;

	/* DT_CALCRECT: 필요한 크기만 계산 */
	if (format & DT_CALCRECT) {
		rect->right = rect->left + text_w;
		rect->bottom = rect->top + text_h;
		return text_h;
	}

	int rect_w = rect->right - rect->left;
	int rect_h = rect->bottom - rect->top;

	/* 시작 위치 계산 */
	int x = rect->left;
	int y = rect->top;

	if (format & DT_CENTER)
		x = rect->left + (rect_w - text_w) / 2;
	else if (format & DT_RIGHT)
		x = rect->right - text_w;

	if ((format & DT_VCENTER) && (format & DT_SINGLELINE))
		y = rect->top + (rect_h - text_h) / 2;
	else if (format & DT_BOTTOM)
		y = rect->bottom - text_h;

	/* 렌더링 (TextOutA 로직 재사용) */
	if (!dc->pixels)
		return text_h;

	uint32_t fg = colorref_to_pixel(dc->text_color);
	uint32_t bg = colorref_to_pixel(dc->bk_color);

	for (int c = 0; c < count && text[c]; c++) {
		unsigned char ch = (unsigned char)text[c];

		if (ch > 127)
			ch = '?';

		if (g_gdi_psf2.loaded) {
			if (dc->bk_mode == OPAQUE) {
				for (int row = 0; row < font_h; row++) {
					int py = y + row;
					if (py < 0 || py >= dc->height)
						continue;
					for (int col = 0; col < font_w; col++) {
						int px2 = x + c * font_w + col;
						if (px2 >= 0 && px2 < dc->width)
							dc->pixels[py * dc->width + px2] = bg;
					}
				}
			}
			psf2_draw_char(dc->pixels, dc->width,
				       x + c * font_w, y,
				       (char)ch, fg, &g_gdi_psf2);
		} else {
			const uint8_t *glyph =
				(const uint8_t *)font8x8_basic[ch];

			for (int row = 0; row < 8; row++) {
				int py = y + row;

				if (py < 0 || py >= dc->height)
					continue;

				for (int col = 0; col < 8; col++) {
					int px2 = x + c * 8 + col;

					if (px2 < 0 || px2 >= dc->width)
						continue;

					if (glyph[row] & (1 << col)) {
						dc->pixels[py * dc->width + px2] = fg;
					} else if (dc->bk_mode == OPAQUE) {
						dc->pixels[py * dc->width + px2] = bg;
					}
				}
			}
		}
	}

	return text_h;
}

/* --- GetTextMetricsA --- */

/*
 * 폰트 메트릭스 조회.
 * Phase 3: font8x8 고정폭 폰트의 메트릭스 반환.
 */
__attribute__((ms_abi))
static int32_t g32_GetTextMetricsA(HDC hdc, TEXTMETRICA *tm)
{
	(void)hdc;

	if (!tm)
		return FALSE;

	gdi32_ensure_font();

	memset(tm, 0, sizeof(*tm));
	tm->tmHeight = g_gdi_font_h;
	tm->tmAscent = g_gdi_font_h - 1;
	tm->tmDescent = 1;
	tm->tmAveCharWidth = g_gdi_font_w;
	tm->tmMaxCharWidth = g_gdi_font_w;
	tm->tmWeight = 400;        /* FW_NORMAL */
	tm->tmFirstChar = 0x20;    /* space */
	tm->tmLastChar = 0x7E;     /* tilde */
	tm->tmDefaultChar = '?';

	return TRUE;
}

/* ============================================================
 * 스텁 테이블
 * ============================================================ */

#include "../../../include/stub_entry.h"

struct stub_entry gdi32_stub_table[] = {
	/* DC 관리 */
	{ "gdi32.dll", "GetDC",            (void *)g32_GetDC },
	{ "gdi32.dll", "ReleaseDC",        (void *)g32_ReleaseDC },

	/* 텍스트 */
	{ "gdi32.dll", "TextOutA",         (void *)g32_TextOutA },

	/* 픽셀 */
	{ "gdi32.dll", "SetPixel",         (void *)g32_SetPixel },
	{ "gdi32.dll", "GetPixel",         (void *)g32_GetPixel },

	/* 도형 */
	{ "gdi32.dll", "Rectangle",        (void *)g32_Rectangle },

	/* 오브젝트 */
	{ "gdi32.dll", "CreateSolidBrush", (void *)g32_CreateSolidBrush },
	{ "gdi32.dll", "DeleteObject",     (void *)g32_DeleteObject },
	{ "gdi32.dll", "SelectObject",     (void *)g32_SelectObject },

	/* DC 상태 */
	{ "gdi32.dll", "SetTextColor",     (void *)g32_SetTextColor },
	{ "gdi32.dll", "SetBkColor",       (void *)g32_SetBkColor },
	{ "gdi32.dll", "SetBkMode",        (void *)g32_SetBkMode },

	/* FillRect는 user32.dll export */
	{ "user32.dll", "FillRect",        (void *)g32_FillRect },

	/* 스톡 오브젝트 */
	{ "gdi32.dll", "GetStockObject",   (void *)g32_GetStockObject },

	/* 텍스트 확장 */
	{ "user32.dll", "DrawTextA",       (void *)g32_DrawTextA },
	{ "gdi32.dll", "GetTextMetricsA",  (void *)g32_GetTextMetricsA },

	{ NULL, NULL, NULL }
};
