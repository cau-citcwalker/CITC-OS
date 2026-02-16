/*
 * gdi32.h — CITC OS gdi32.dll
 * ============================
 *
 * Windows GDI (Graphics Device Interface) 구현.
 *
 * GDI의 핵심 개념:
 *   HDC (Device Context) = 그리기 대상 + 그리기 상태
 *   - 어디에 그릴지 (윈도우 픽셀 버퍼)
 *   - 어떻게 그릴지 (색상, 브러시, 폰트, 배경 모드)
 *
 * Phase 3에서 구현하는 함수:
 *   TextOutA, SetPixel, Rectangle, FillRect,
 *   CreateSolidBrush, SelectObject, SetTextColor, SetBkColor
 */

#ifndef CITC_GDI32_H
#define CITC_GDI32_H

#include "../../../include/win32.h"
#include "../../../include/stub_entry.h"

/* 스텁 테이블 (citcrun이 참조) */
extern struct stub_entry gdi32_stub_table[];

/*
 * 내부 API — user32의 BeginPaint/EndPaint가 호출
 *
 * user32는 HWND의 픽셀 버퍼를 알고 있으므로,
 * HDC를 만들 때 픽셀 포인터와 크기를 직접 전달.
 */
HDC gdi32_create_dc_for_window(HWND hwnd, uint32_t *pixels,
			       int width, int height);
void gdi32_release_dc(HDC hdc);

#endif /* CITC_GDI32_H */
