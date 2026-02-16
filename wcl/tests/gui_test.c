/*
 * gui_test.c — CITC OS WCL GUI 테스트
 * ======================================
 *
 * Phase 3 GUI 통합 테스트:
 *   RegisterClassA → CreateWindowExA → 메시지 루프
 *   → WM_PAINT(TextOutA) → WM_CLOSE → WM_DESTROY → 종료
 *
 * 자동 테스트: WM_PAINT 처리 후 자동으로 WM_CLOSE를 보내
 * 전체 생명주기를 검증합니다.
 * (컴포지터 없이도 WSL에서 테스트 가능)
 *
 * 빌드:
 *   x86_64-w64-mingw32-gcc -nostdlib -o gui_test.exe gui_test.c \
 *       -lkernel32 -luser32 -lgdi32 -Wl,-e,_start
 *
 * 실행:
 *   citcrun gui_test.exe
 */

#include <stdint.h>

/* === 기본 Windows 타입 (windows.h 대신) === */

typedef void         *HANDLE;
typedef void         *HWND;
typedef void         *HDC;
typedef void         *HBRUSH;
typedef void         *HGDIOBJ;
typedef unsigned int  UINT;
typedef unsigned long DWORD;
typedef int           BOOL;
typedef int32_t       LONG;
typedef const char   *LPCSTR;
typedef const void   *LPCVOID;
typedef unsigned long *LPDWORD;
typedef void         *LPOVERLAPPED;
typedef void         *LPVOID;
typedef uint64_t      WPARAM;
typedef int64_t       LPARAM;
typedef int64_t       LRESULT;

#define NULL ((void *)0)
#define TRUE  1
#define FALSE 0

/* 표준 핸들 */
#define STD_OUTPUT_HANDLE ((DWORD)-11)

/* 윈도우 메시지 */
#define WM_CREATE       0x0001
#define WM_DESTROY      0x0002
#define WM_PAINT        0x000F
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_TIMER        0x0113

/* 윈도우 스타일 */
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_VISIBLE          0x10000000L

/* ShowWindow */
#define SW_SHOWDEFAULT  10

/* CW_USEDEFAULT */
#define CW_USEDEFAULT   ((int)0x80000000)

/* 배경 모드 */
#define TRANSPARENT     1

/* GetWindowLong 인덱스 */
#define GWL_STYLE       (-16)
#define GWLP_USERDATA   (-21)

/* GetSystemMetrics */
#define SM_CXSCREEN     0

/* DrawText 플래그 */
#define DT_CENTER       0x00000001
#define DT_VCENTER      0x00000004
#define DT_SINGLELINE   0x00000020
#define DT_CALCRECT     0x00000400

/* COLORREF */
typedef void         *HICON;
typedef void         *HCURSOR;

/* TEXTMETRICA 구조체 */
typedef struct {
	LONG tmHeight;
	LONG tmAscent;
	LONG tmDescent;
	LONG tmInternalLeading;
	LONG tmExternalLeading;
	LONG tmAveCharWidth;
	LONG tmMaxCharWidth;
	LONG tmWeight;
	LONG tmOverhang;
	LONG tmDigitizedAspectX;
	LONG tmDigitizedAspectY;
	char tmFirstChar;
	char tmLastChar;
	char tmDefaultChar;
	char tmBreakChar;
	char tmItalic;
	char tmUnderlined;
	char tmStruckOut;
	char tmPitchAndFamily;
	char tmCharSet;
} TEXTMETRICA;

typedef DWORD COLORREF;
#define RGB(r, g, b) ((COLORREF)(((DWORD)(r)) | \
			(((DWORD)(g)) << 8) | \
			(((DWORD)(b)) << 16)))

/* === 구조체 === */

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
	UINT      style;
	WNDPROC   lpfnWndProc;
	int       cbClsExtra;
	int       cbWndExtra;
	HANDLE    hInstance;
	HANDLE    hIcon;
	HANDLE    hCursor;
	HBRUSH    hbrBackground;
	LPCSTR    lpszMenuName;
	LPCSTR    lpszClassName;
} WNDCLASSA;

typedef struct {
	HWND   hwnd;
	UINT   message;
	WPARAM wParam;
	LPARAM lParam;
	DWORD  time;
	LONG   pt_x;
	LONG   pt_y;
} MSG;

typedef struct {
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
} RECT;

typedef struct {
	HDC  hdc;
	BOOL fErase;
	RECT rcPaint;
	BOOL fRestore;
	BOOL fIncUpdate;
	char rgbReserved[32];
} PAINTSTRUCT;

/* === kernel32.dll 임포트 === */

__declspec(dllimport) void __stdcall ExitProcess(UINT);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, LPCVOID, DWORD,
					       LPDWORD, LPOVERLAPPED);

/* === user32.dll 임포트 === */

__declspec(dllimport) UINT __stdcall RegisterClassA(const WNDCLASSA *);
__declspec(dllimport) HWND __stdcall CreateWindowExA(
	DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
	HWND, HANDLE, HANDLE, LPVOID);
__declspec(dllimport) BOOL __stdcall ShowWindow(HWND, int);
__declspec(dllimport) BOOL __stdcall UpdateWindow(HWND);
__declspec(dllimport) BOOL __stdcall GetMessageA(MSG *, HWND, UINT, UINT);
__declspec(dllimport) BOOL __stdcall TranslateMessage(const MSG *);
__declspec(dllimport) LRESULT __stdcall DispatchMessageA(const MSG *);
__declspec(dllimport) void __stdcall PostQuitMessage(int);
__declspec(dllimport) LRESULT __stdcall DefWindowProcA(HWND, UINT,
						       WPARAM, LPARAM);
__declspec(dllimport) BOOL __stdcall PostMessageA(HWND, UINT,
						  WPARAM, LPARAM);
__declspec(dllimport) HDC __stdcall BeginPaint(HWND, PAINTSTRUCT *);
__declspec(dllimport) BOOL __stdcall EndPaint(HWND, const PAINTSTRUCT *);
__declspec(dllimport) BOOL __stdcall GetClientRect(HWND, RECT *);

/* 타이머 */
__declspec(dllimport) uintptr_t __stdcall SetTimer(HWND, uintptr_t,
						   UINT, void *);
__declspec(dllimport) BOOL __stdcall KillTimer(HWND, uintptr_t);

/* 윈도우 속성 */
__declspec(dllimport) LONG __stdcall GetWindowLongA(HWND, int);
__declspec(dllimport) LONG __stdcall SetWindowLongA(HWND, int, LONG);
__declspec(dllimport) BOOL __stdcall IsWindow(HWND);
__declspec(dllimport) BOOL __stdcall IsWindowVisible(HWND);
__declspec(dllimport) BOOL __stdcall GetWindowRect(HWND, RECT *);
__declspec(dllimport) BOOL __stdcall SetWindowTextA(HWND, const char *);
__declspec(dllimport) int __stdcall GetWindowTextA(HWND, char *, int);
__declspec(dllimport) BOOL __stdcall MoveWindow(HWND, int, int,
						int, int, BOOL);

/* 포커스 */
__declspec(dllimport) HWND __stdcall SetFocus(HWND);
__declspec(dllimport) HWND __stdcall GetFocus(void);

/* 시스템 정보 */
__declspec(dllimport) int __stdcall GetSystemMetrics(int);

/* 리소스 스텁 */
__declspec(dllimport) HCURSOR __stdcall LoadCursorA(HANDLE, const char *);
__declspec(dllimport) HICON __stdcall LoadIconA(HANDLE, const char *);

/* === gdi32.dll 임포트 === */

__declspec(dllimport) BOOL __stdcall TextOutA(HDC, int, int,
					      const char *, int);
__declspec(dllimport) COLORREF __stdcall SetTextColor(HDC, COLORREF);
__declspec(dllimport) int __stdcall SetBkMode(HDC, int);
__declspec(dllimport) HGDIOBJ __stdcall GetStockObject(int);
__declspec(dllimport) int __stdcall DrawTextA(HDC, const char *, int,
					      RECT *, UINT);
__declspec(dllimport) BOOL __stdcall GetTextMetricsA(HDC, TEXTMETRICA *);

/* === 유틸리티 (CRT 없이) === */

static HANDLE g_stdout;

static void print(const char *s)
{
	DWORD written;
	DWORD len = 0;

	while (s[len])
		len++;
	WriteFile(g_stdout, s, len, &written, NULL);
}

static void print_num(int num)
{
	char buf[16];
	int i = 0;

	if (num == 0) {
		buf[i++] = '0';
	} else {
		while (num > 0) {
			buf[i++] = '0' + (char)(num % 10);
			num /= 10;
		}
	}

	DWORD written;
	char rev[16];

	for (int j = 0; j < i; j++)
		rev[j] = buf[i - 1 - j];
	WriteFile(g_stdout, rev, (DWORD)i, &written, NULL);
}

/* === 테스트 상태 === */

static int g_pass = 0;
static int g_fail = 0;
static int g_paint_count = 0;

/* === WndProc (윈도우 프로시저) === */

LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CREATE:
		print("[WM_CREATE] OK\n");
		g_pass++;
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		if (!hdc) {
			print("[WM_PAINT] FAIL (BeginPaint returned NULL)\n");
			g_fail++;
			return 0;
		}

		/* 텍스트 색상: 빨강 */
		SetTextColor(hdc, RGB(255, 0, 0));
		SetBkMode(hdc, TRANSPARENT);

		/* 텍스트 출력 */
		const char *text = "Hello Win32 GUI!";
		int text_len = 16;

		TextOutA(hdc, 10, 10, text, text_len);

		EndPaint(hwnd, &ps);

		g_paint_count++;
		print("[WM_PAINT] OK (TextOutA done)\n");
		g_pass++;

		/* 자동 테스트: 첫 WM_PAINT 후 WM_CLOSE 전송 */
		PostMessageA(hwnd, WM_CLOSE, 0, 0);
		return 0;
	}

	case WM_DESTROY:
		print("[WM_DESTROY] OK\n");
		g_pass++;
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* === 엔트리포인트 === */

void _start(void)
{
	g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

	print("=== Win32 GUI Test (Phase 3) ===\n\n");

	/* 1. RegisterClassA */
	print("[1] RegisterClassA... ");
	WNDCLASSA wc;
	int i;

	/* memset 대체 (CRT 없음) */
	char *p = (char *)&wc;
	for (i = 0; i < (int)sizeof(wc); i++)
		p[i] = 0;

	wc.lpfnWndProc = WndProc;
	wc.lpszClassName = "GuiTestClass";
	wc.hbrBackground = (HBRUSH)(uintptr_t)6;  /* COLOR_WINDOW + 1 */

	UINT atom = RegisterClassA(&wc);

	if (atom) {
		print("OK (atom=");
		print_num((int)atom);
		print(")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
		goto done;
	}

	/* 2. CreateWindowExA */
	print("[2] CreateWindowExA... ");
	HWND hwnd = CreateWindowExA(
		0,                       /* dwExStyle */
		"GuiTestClass",          /* lpClassName */
		"GUI Test Window",       /* lpWindowName */
		WS_OVERLAPPEDWINDOW,     /* dwStyle */
		CW_USEDEFAULT,           /* x */
		CW_USEDEFAULT,           /* y */
		400,                     /* width */
		300,                     /* height */
		NULL,                    /* hWndParent */
		NULL,                    /* hMenu */
		NULL,                    /* hInstance */
		NULL                     /* lpParam */
	);
	/* WM_CREATE는 CreateWindowExA 내부에서 전송됨 */

	if (hwnd) {
		print("OK (HWND=0x");
		/* 간단한 hex 출력 */
		uintptr_t val = (uintptr_t)hwnd;
		char hex[17];
		for (i = 0; i < 8; i++) {
			int nibble = (int)((val >> (28 - i * 4)) & 0xF);
			hex[i] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
		}
		hex[8] = '\0';
		print(hex);
		print(")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
		goto done;
	}

	/* 3. GetClientRect */
	print("[3] GetClientRect... ");
	RECT rc;

	if (GetClientRect(hwnd, &rc)) {
		print("OK (");
		print_num((int)rc.right);
		print("x");
		print_num((int)rc.bottom);
		print(")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* 4. ShowWindow + UpdateWindow */
	print("[4] ShowWindow... ");
	ShowWindow(hwnd, SW_SHOWDEFAULT);
	print("OK\n");
	g_pass++;

	print("[5] UpdateWindow... ");
	UpdateWindow(hwnd);
	/* UpdateWindow이 WM_PAINT를 동기 전송 → WndProc에서 처리 */
	print("OK\n");
	g_pass++;

	/* 6. 메시지 루프 */
	print("[6] Message loop...\n");
	MSG msg_buf;

	while (GetMessageA(&msg_buf, NULL, 0, 0)) {
		TranslateMessage(&msg_buf);
		DispatchMessageA(&msg_buf);
	}

	print("[6] Message loop ended (WM_QUIT received)\n");
	g_pass++;

	/*
	 * 추가 API 테스트 (Phase 3 보강)
	 * 윈도우는 이미 파괴되었으므로 새 윈도우 생성하여 테스트.
	 */
	print("\n--- Phase 3+ Extended API Tests ---\n");

	/* 새 윈도우 생성 (추가 테스트용) */
	HWND hw2 = CreateWindowExA(
		0, "GuiTestClass", "Test2", WS_OVERLAPPEDWINDOW,
		50, 50, 320, 240, NULL, NULL, NULL, NULL);
	if (!hw2) {
		print("[ERR] CreateWindowExA for extended tests FAIL\n");
		g_fail++;
		goto done;
	}
	/* WM_CREATE counted above → +1 already */

	ShowWindow(hw2, 10);

	/* [10] SetTimer */
	print("[10] SetTimer(100ms)... ");
	uintptr_t tid = SetTimer(hw2, 1, 100, NULL);
	if (tid) {
		print("OK (id=");
		print_num((int)tid);
		print(")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [11] KillTimer */
	print("[11] KillTimer... ");
	if (KillTimer(hw2, tid)) {
		print("OK\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [12] GetWindowLongA(GWL_STYLE) */
	print("[12] GetWindowLongA(GWL_STYLE)... ");
	LONG style = GetWindowLongA(hw2, GWL_STYLE);
	if (style == (LONG)WS_OVERLAPPEDWINDOW) {
		print("OK (0x");
		/* 간단한 hex */
		uintptr_t sv = (uintptr_t)(unsigned long)style;
		char hx[9];
		for (i = 0; i < 8; i++) {
			int nb = (int)((sv >> (28 - i * 4)) & 0xF);
			hx[i] = nb < 10 ? '0' + nb : 'a' + nb - 10;
		}
		hx[8] = '\0';
		print(hx);
		print(")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [13] SetWindowLongA(GWLP_USERDATA) roundtrip */
	print("[13] SetWindowLongA(GWLP_USERDATA)... ");
	SetWindowLongA(hw2, GWLP_USERDATA, 0x12345);
	LONG ud = GetWindowLongA(hw2, GWLP_USERDATA);
	if (ud == 0x12345) {
		print("OK (roundtrip)\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [14] IsWindow / IsWindowVisible */
	print("[14] IsWindow/IsWindowVisible... ");
	if (IsWindow(hw2) && IsWindowVisible(hw2)) {
		print("OK\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [15] GetWindowRect */
	print("[15] GetWindowRect... ");
	RECT wr;
	if (GetWindowRect(hw2, &wr) && wr.left == 50 && wr.top == 50 &&
	    wr.right == 370 && wr.bottom == 290) {
		print("OK (");
		print_num((int)wr.left);
		print(",");
		print_num((int)wr.top);
		print(",");
		print_num((int)wr.right);
		print(",");
		print_num((int)wr.bottom);
		print(")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [16] SetWindowTextA + GetWindowTextA roundtrip */
	print("[16] SetWindowTextA + GetWindowTextA... ");
	SetWindowTextA(hw2, "NewTitle");
	char title_buf[64];
	int tlen = GetWindowTextA(hw2, title_buf, 64);
	/* 수동 strcmp */
	int title_ok = (tlen == 8);
	if (title_ok) {
		const char *exp = "NewTitle";
		for (i = 0; i < 8; i++) {
			if (title_buf[i] != exp[i]) {
				title_ok = 0;
				break;
			}
		}
	}
	if (title_ok) {
		print("OK (\"");
		print(title_buf);
		print("\")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [17] GetSystemMetrics(SM_CXSCREEN) */
	print("[17] GetSystemMetrics(SM_CXSCREEN)... ");
	int cx = GetSystemMetrics(SM_CXSCREEN);
	if (cx > 0) {
		print("OK (");
		print_num(cx);
		print(")\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [18] GetStockObject(WHITE_BRUSH=0) */
	print("[18] GetStockObject(WHITE_BRUSH)... ");
	HGDIOBJ stock = GetStockObject(0);  /* WHITE_BRUSH */
	if (stock) {
		print("OK (non-NULL)\n");
		g_pass++;
	} else {
		print("FAIL\n");
		g_fail++;
	}

	/* [19] DrawTextA(DT_CALCRECT) */
	print("[19] DrawTextA(DT_CALCRECT)... ");
	{
		PAINTSTRUCT ps2;
		HDC hdc2 = BeginPaint(hw2, &ps2);
		if (hdc2) {
			RECT dr = { 0, 0, 0, 0 };
			int h = DrawTextA(hdc2, "Test", 4, &dr, DT_CALCRECT);
			EndPaint(hw2, &ps2);
			if (h == 8 && dr.right == 32 && dr.bottom == 8) {
				print("OK (h=");
				print_num(h);
				print(" r=");
				print_num((int)dr.right);
				print(")\n");
				g_pass++;
			} else {
				print("FAIL (h=");
				print_num(h);
				print(")\n");
				g_fail++;
			}
		} else {
			print("FAIL (no HDC)\n");
			g_fail++;
		}
	}

	/* [20] GetTextMetricsA */
	print("[20] GetTextMetricsA... ");
	{
		PAINTSTRUCT ps3;
		HDC hdc3 = BeginPaint(hw2, &ps3);
		if (hdc3) {
			TEXTMETRICA tm;
			char *tp = (char *)&tm;
			for (i = 0; i < (int)sizeof(tm); i++)
				tp[i] = 0;
			BOOL ok = GetTextMetricsA(hdc3, &tm);
			EndPaint(hw2, &ps3);
			if (ok && tm.tmHeight == 8 && tm.tmAveCharWidth == 8) {
				print("OK (height=");
				print_num((int)tm.tmHeight);
				print(" avg_w=");
				print_num((int)tm.tmAveCharWidth);
				print(")\n");
				g_pass++;
			} else {
				print("FAIL\n");
				g_fail++;
			}
		} else {
			print("FAIL (no HDC)\n");
			g_fail++;
		}
	}

done:
	/* 결과 요약 */
	print("\n=== Result: ");
	print_num(g_pass);
	print(" passed, ");
	print_num(g_fail);
	print(" failed ===\n");

	ExitProcess(g_fail > 0 ? 1 : 0);
}
