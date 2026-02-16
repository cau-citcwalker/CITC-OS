/*
 * user32.h — CITC OS user32.dll
 * ==============================
 *
 * Windows User32 (User Interface DLL) 구현.
 *
 * User32의 핵심 역할:
 *   - 윈도우 관리: HWND 생성/파괴, 크기/위치
 *   - 메시지 시스템: GetMessageA/DispatchMessageA 루프
 *   - 입력 처리: 키보드/마우스 이벤트 → Win32 메시지
 *   - 그리기 트리거: BeginPaint/EndPaint
 *
 * CDP (CITC Display Protocol) 통합:
 *   HWND 1:1 ↔ CDP surface
 *   CDP 이벤트 → Win32 메시지 변환
 *   컴포지터 미실행 시: 로컬 픽셀 버퍼 + self-pipe 모드
 */

#ifndef CITC_USER32_H
#define CITC_USER32_H

#include "../../../include/win32.h"
#include "../../../include/stub_entry.h"

/*
 * user32 서브시스템 초기화.
 * 테이블 초기화 + self-pipe 생성.
 * CDP 연결은 CreateWindowExA 첫 호출 시 lazy 수행.
 */
void user32_init(void);

/* user32.dll 스텁 테이블 (user32.c에서 정의) */
extern struct stub_entry user32_stub_table[];

/*
 * 내부 API — dxgi.c의 SwapChain::Present가 호출
 *
 * DXGI SwapChain은 HWND의 픽셀 버퍼에 직접 접근하여
 * 렌더링 결과를 복사하고, CDP commit으로 화면 갱신.
 */
int  user32_get_window_pixels(HWND hwnd, uint32_t **pixels,
			      int *width, int *height);
void user32_commit_window(HWND hwnd);

#endif /* CITC_USER32_H */
