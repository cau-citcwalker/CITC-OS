/*
 * win32.h — Windows API 타입 & 상수 정의
 * ========================================
 *
 * Windows의 <windows.h>에 해당하는 최소 버전.
 * kernel32.dll 구현과 테스트 프로그램에서 공유합니다.
 *
 * 왜 별도 헤더인가?
 *   pe.h는 PE 바이너리 포맷 (파일 구조) 정의.
 *   win32.h는 Win32 API 표면 (타입, 상수, 에러 코드) 정의.
 *   관심사 분리: 로더(pe.h) vs API 구현(win32.h)
 *
 * 참고:
 *   실제 windows.h는 수만 줄이지만,
 *   우리는 kernel32.dll 파일 I/O에 필요한 것만 정의합니다.
 */

#ifndef CITC_WIN32_H
#define CITC_WIN32_H

#include <stdint.h>

/* ============================================================
 * 기본 타입
 * ============================================================
 *
 * Windows는 독자적 타입 시스템을 사용합니다:
 *   DWORD = "Double WORD" = 32비트 (16비트 시절의 유산)
 *   HANDLE = 불투명 포인터 (커널 객체 참조)
 *   BOOL = int (TRUE=1, FALSE=0)
 *
 * HANDLE의 의미:
 *   Windows 커널은 모든 I/O 객체를 HANDLE로 추상화합니다.
 *   파일, 콘솔, 파이프, 이벤트, 뮤텍스, 프로세스, 스레드...
 *   전부 HANDLE 하나로 다룹니다.
 *   Linux의 fd(file descriptor)는 정수지만,
 *   Windows의 HANDLE은 포인터 크기의 불투명 값입니다.
 */
typedef void      *HANDLE;
typedef uint32_t   DWORD;
typedef unsigned int UINT;
typedef int        BOOL;
typedef int32_t    LONG;       /* 4 bytes (Windows LLP64: long=4, Linux LP64: long=8) */
typedef const char *LPCSTR;
typedef const void *LPCVOID;
typedef void      *LPVOID;
typedef uint32_t  *LPDWORD;
typedef void      *LPOVERLAPPED;

/* ============================================================
 * 특수 값
 * ============================================================ */
#define INVALID_HANDLE_VALUE  ((HANDLE)(uintptr_t)-1)
#define TRUE   1
#define FALSE  0

/* ============================================================
 * 표준 핸들 상수
 * ============================================================
 *
 * GetStdHandle()에 전달하는 값.
 * Windows에서 이 값이 음수인 이유:
 *   양수 범위는 일반 HANDLE 인덱스로 사용되므로,
 *   표준 핸들은 음수로 구분합니다.
 *   (DWORD로 캐스팅하면 0xFFFFFFF6 등의 큰 양수가 됨)
 */
#define STD_INPUT_HANDLE   ((DWORD)-10)  /* 0xFFFFFFF6 */
#define STD_OUTPUT_HANDLE  ((DWORD)-11)  /* 0xFFFFFFF5 */
#define STD_ERROR_HANDLE   ((DWORD)-12)  /* 0xFFFFFFF4 */

/* ============================================================
 * 접근 권한 플래그 (CreateFile dwDesiredAccess)
 * ============================================================
 *
 * 상위 비트를 사용하여 일반 접근 권한과 구분:
 *   GENERIC_READ  = 0x80000000 (bit 31)
 *   GENERIC_WRITE = 0x40000000 (bit 30)
 */
#define GENERIC_READ    0x80000000UL
#define GENERIC_WRITE   0x40000000UL

/* ============================================================
 * 공유 모드 (CreateFile dwShareMode)
 * ============================================================ */
#define FILE_SHARE_READ   0x00000001UL
#define FILE_SHARE_WRITE  0x00000002UL

/* ============================================================
 * 생성 모드 (CreateFile dwCreationDisposition)
 * ============================================================
 *
 * 파일이 이미 존재할 때 / 존재하지 않을 때의 동작을 결정.
 *
 *   모드              파일 존재        파일 없음
 *   ─────────────     ──────────      ─────────
 *   CREATE_NEW        ERROR           새로 생성
 *   CREATE_ALWAYS     덮어쓰기        새로 생성
 *   OPEN_EXISTING     열기            ERROR
 *   OPEN_ALWAYS       열기            새로 생성
 *   TRUNCATE_EXISTING 비우고 열기     ERROR
 */
#define CREATE_NEW         1
#define CREATE_ALWAYS      2
#define OPEN_EXISTING      3
#define OPEN_ALWAYS        4
#define TRUNCATE_EXISTING  5

/* ============================================================
 * 파일 속성 (CreateFile dwFlagsAndAttributes)
 * ============================================================ */
#define FILE_ATTRIBUTE_NORMAL  0x00000080UL

/* ============================================================
 * SetFilePointer 이동 기준
 * ============================================================
 *
 * POSIX lseek()의 SEEK_SET/CUR/END에 대응:
 *   FILE_BEGIN   = SEEK_SET (파일 시작 기준)
 *   FILE_CURRENT = SEEK_CUR (현재 위치 기준)
 *   FILE_END     = SEEK_END (파일 끝 기준)
 */
#define FILE_BEGIN    0
#define FILE_CURRENT  1
#define FILE_END      2

/* ============================================================
 * Win32 에러 코드
 * ============================================================
 *
 * GetLastError()가 반환하는 값.
 * POSIX errno와 1:1 매핑은 아니지만, 대략적인 대응이 있습니다:
 *
 *   Win32 에러 코드          POSIX errno
 *   ─────────────────        ──────────
 *   ERROR_FILE_NOT_FOUND     ENOENT
 *   ERROR_PATH_NOT_FOUND     ENOTDIR
 *   ERROR_ACCESS_DENIED      EACCES
 *   ERROR_INVALID_HANDLE     EBADF
 *   ERROR_TOO_MANY_OPEN_FILES EMFILE
 *   ERROR_DISK_FULL          ENOSPC
 *   ERROR_ALREADY_EXISTS     EEXIST
 */
#define ERROR_SUCCESS              0
#define ERROR_FILE_NOT_FOUND       2
#define ERROR_PATH_NOT_FOUND       3
#define ERROR_TOO_MANY_OPEN_FILES  4
#define ERROR_ACCESS_DENIED        5
#define ERROR_INVALID_HANDLE       6
#define ERROR_GEN_FAILURE          31
#define ERROR_INVALID_PARAMETER    87
#define ERROR_DISK_FULL            112
#define ERROR_ALREADY_EXISTS       183

/* 서비스 에러 코드 */
#define ERROR_SERVICE_DOES_NOT_EXIST  1060
#define ERROR_SERVICE_ALREADY_RUNNING 1056

/* Token 접근 권한 */
#define TOKEN_QUERY               0x0008
#define TOKEN_ADJUST_PRIVILEGES   0x0020

/* SC_MANAGER 접근 권한 */
#define SC_MANAGER_ALL_ACCESS     0xF003F

/* SetFilePointer / GetFileSize 실패 반환값 */
#define INVALID_SET_FILE_POINTER  ((DWORD)-1)
#define INVALID_FILE_SIZE         ((DWORD)-1)

/* ============================================================
 * VirtualAlloc / VirtualFree 상수
 * ============================================================ */

/* 할당 타입 (flAllocationType) */
#define MEM_COMMIT     0x00001000
#define MEM_RESERVE    0x00002000
#define MEM_RELEASE    0x00008000

/* 메모리 보호 (flProtect) */
#define PAGE_NOACCESS           0x01
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_EXECUTE            0x10
#define PAGE_EXECUTE_READ       0x20
#define PAGE_EXECUTE_READWRITE  0x40

/* ============================================================
 * Heap 상수
 * ============================================================ */
#define HEAP_ZERO_MEMORY  0x00000008

/* ============================================================
 * WaitForSingleObject 상수
 * ============================================================ */
#define WAIT_OBJECT_0   0x00000000
#define WAIT_TIMEOUT    0x00000102
#define WAIT_FAILED     ((DWORD)-1)
#define INFINITE        ((DWORD)-1)

/* ============================================================
 * 프로세스/스레드 상수
 * ============================================================ */
#define MAX_PATH  260

/* ============================================================
 * GUI 타입 (Phase 3: user32 + gdi32)
 * ============================================================
 *
 * HWND: 윈도우 핸들 (user-mode, win32k.sys 관리)
 * HDC:  Device Context (GDI 그리기 대상 + 상태)
 * WPARAM/LPARAM: 메시지 파라미터 (역사적 이름: Word/Long param)
 * LRESULT: 메시지 처리 결과
 * WNDPROC: 윈도우 프로시저 콜백 함수 타입
 */
typedef void     *HWND;
typedef void     *HDC;
typedef void     *HBRUSH;
typedef void     *HGDIOBJ;
typedef void     *HICON;
typedef void     *HCURSOR;
typedef uintptr_t WPARAM;
typedef intptr_t  LPARAM;
typedef intptr_t  LRESULT;

/* WNDPROC — 윈도우 프로시저 콜백 */
typedef LRESULT (__attribute__((ms_abi)) *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

/* ============================================================
 * 윈도우 메시지 (WM_*)
 * ============================================================
 *
 * 모든 Win32 윈도우 통신은 메시지로 이루어짐.
 * 하드웨어 이벤트(키보드, 마우스)도 메시지로 전달.
 *
 *   PostMessage → 큐에 추가 (비동기)
 *   SendMessage → WndProc 직접 호출 (동기)
 *   GetMessage  → 큐에서 꺼내기 (블로킹)
 */
#define WM_CREATE       0x0001
#define WM_DESTROY      0x0002
#define WM_SIZE         0x0005
#define SIZE_RESTORED   0
#define SIZE_MINIMIZED  1
#define SIZE_MAXIMIZED  2

/* 클립보드 포맷 (Class 62) */
#define CF_TEXT         1
#define WM_SETFOCUS     0x0007
#define WM_KILLFOCUS    0x0008
#define WM_PAINT        0x000F
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_SHOWWINDOW   0x0018
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_CHAR         0x0102
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_RBUTTONDOWN  0x0204
#define WM_RBUTTONUP    0x0205
#define WM_TIMER        0x0113
#define WM_SETTEXT      0x000C
#define WM_GETTEXT      0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_MOVE         0x0003

/* ============================================================
 * 윈도우 스타일 (WS_*)
 * ============================================================ */
#define WS_OVERLAPPED    0x00000000L
#define WS_CAPTION       0x00C00000L
#define WS_SYSMENU       0x00080000L
#define WS_THICKFRAME    0x00040000L
#define WS_MINIMIZEBOX   0x00020000L
#define WS_MAXIMIZEBOX   0x00010000L
#define WS_VISIBLE       0x10000000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
			     WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

/* ShowWindow 명령 */
#define SW_HIDE          0
#define SW_SHOWNORMAL    1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE      3
#define SW_SHOW          5
#define SW_MINIMIZE      6
#define SW_RESTORE       9
#define SW_SHOWDEFAULT   10

/* CW_USEDEFAULT — 시스템이 위치/크기 결정 */
#define CW_USEDEFAULT    ((int)0x80000000)

/* ============================================================
 * GetWindowLong 인덱스
 * ============================================================
 *
 * 윈도우 속성에 접근하는 음수 인덱스:
 *   GWL_WNDPROC: 윈도우 프로시저 (서브클래싱)
 *   GWL_STYLE: 윈도우 스타일 (WS_*)
 *   GWL_EXSTYLE: 확장 스타일
 *   GWLP_USERDATA: 앱 전용 데이터
 *
 * 음수를 쓰는 이유:
 *   양수(0, 4, 8...)는 cbWndExtra 영역 접근에 사용.
 *   음수는 시스템 속성을 의미.
 */
#define GWL_WNDPROC    (-4)
#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)
#define GWLP_USERDATA  (-21)

/* ============================================================
 * GetSystemMetrics 인덱스
 * ============================================================ */
#define SM_CXSCREEN    0
#define SM_CYSCREEN    1
#define SM_CXICON      11
#define SM_CYICON      12
#define SM_CXCURSOR    13
#define SM_CYCURSOR    14

/* ============================================================
 * DrawText 플래그
 * ============================================================
 *
 * DrawTextA의 형식 제어:
 *   DT_CENTER + DT_VCENTER + DT_SINGLELINE = 사각형 중앙 정렬
 *   DT_CALCRECT = 그리지 않고 필요한 RECT만 계산
 */
#define DT_TOP         0x00000000
#define DT_LEFT        0x00000000
#define DT_CENTER      0x00000001
#define DT_RIGHT       0x00000002
#define DT_VCENTER     0x00000004
#define DT_BOTTOM      0x00000008
#define DT_WORDBREAK   0x00000010
#define DT_SINGLELINE  0x00000020
#define DT_NOCLIP      0x00000100
#define DT_CALCRECT    0x00000400
#define DT_NOPREFIX    0x00000800

/* ============================================================
 * GUI 구조체
 * ============================================================ */

/* WNDCLASSA — 윈도우 클래스 등록 정보 */
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

/* MSG — 메시지 구조체 */
typedef struct {
	HWND   hwnd;
	UINT   message;
	WPARAM wParam;
	LPARAM lParam;
	DWORD  time;
	LONG   pt_x;
	LONG   pt_y;
} MSG;

/* RECT — 사각형 */
typedef struct {
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
} RECT;

/* PAINTSTRUCT — BeginPaint/EndPaint용 */
typedef struct {
	HDC  hdc;
	BOOL fErase;
	RECT rcPaint;
	BOOL fRestore;
	BOOL fIncUpdate;
	char rgbReserved[32];
} PAINTSTRUCT;

/* TEXTMETRICA — 폰트 메트릭스 */
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

/* ============================================================
 * 색상 (GDI)
 * ============================================================ */
typedef DWORD COLORREF;
#define RGB(r, g, b) ((COLORREF)(((DWORD)(r)) | \
			(((DWORD)(g)) << 8) | \
			(((DWORD)(b)) << 16)))
#define GetRValue(c)  ((unsigned char)(c))
#define GetGValue(c)  ((unsigned char)((c) >> 8))
#define GetBValue(c)  ((unsigned char)((c) >> 16))
#define CLR_INVALID   0xFFFFFFFF

/* GDI stock objects
 *
 * GetStockObject(index)로 얻는 미리 정의된 GDI 오브젝트.
 * 앱이 직접 Create/Delete 할 필요 없음 (시스템 소유).
 */
#define WHITE_BRUSH       0
#define LTGRAY_BRUSH      1
#define GRAY_BRUSH        2
#define DKGRAY_BRUSH      3
#define BLACK_BRUSH       4
#define NULL_BRUSH        5
#define WHITE_PEN         6
#define BLACK_PEN         7
#define NULL_PEN          8
#define SYSTEM_FONT       13
#define DEFAULT_GUI_FONT  17

/* 배경 모드 */
#define TRANSPARENT   1
#define OPAQUE        2

/* ROP 코드 (BitBlt) */
#define SRCCOPY       0x00CC0020
#define BLACKNESS     0x00000042
#define WHITENESS     0x00FF0062

/* ============================================================
 * MessageBox 상수
 * ============================================================ */
#define MB_OK           0x00000000
#define MB_ICONERROR    0x00000010
#define MB_ICONWARNING  0x00000030
#define IDOK            1

/* ============================================================
 * 메시지 매크로
 * ============================================================ */
#define MAKELPARAM(l, h) ((LPARAM)(((uint16_t)(l)) | ((uint32_t)((uint16_t)(h))) << 16))
#define LOWORD(l)        ((uint16_t)((uintptr_t)(l) & 0xFFFF))
#define HIWORD(l)        ((uint16_t)(((uintptr_t)(l) >> 16) & 0xFFFF))
#define GET_X_LPARAM(lp) ((int)(int16_t)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(int16_t)HIWORD(lp))

/* ============================================================
 * COM (Component Object Model) 기본 타입
 * ============================================================
 *
 * DirectX는 COM 인터페이스로 설계되어 있다:
 *   모든 DX 객체는 IUnknown을 상속.
 *   메서드 호출 = vtable 포인터를 통한 간접 호출.
 *
 *   C에서의 COM 호출 패턴:
 *     pObj->lpVtbl->Method(pObj, arg1, arg2);
 *
 *   C++에서는 syntactic sugar로:
 *     pObj->Method(arg1, arg2);
 *
 * HRESULT: COM 표준 반환값
 *   >= 0 : 성공 (S_OK = 0, S_FALSE = 1)
 *   <  0 : 실패 (최상위 비트 = 1)
 *
 * GUID: 128비트 고유 식별자
 *   COM 인터페이스마다 고유 IID(Interface ID)를 가짐.
 *   QueryInterface(riid, ppv)로 인터페이스 캐스팅.
 */
typedef uint32_t ULONG;
typedef int32_t  HRESULT;

#define S_OK           ((HRESULT)0)
#define S_FALSE        ((HRESULT)1)
#define E_NOINTERFACE  ((HRESULT)0x80004002)
#define E_POINTER      ((HRESULT)0x80004003)
#define E_FAIL         ((HRESULT)0x80004005)
#define E_OUTOFMEMORY  ((HRESULT)0x8007000E)
#define E_INVALIDARG   ((HRESULT)0x80070057)

#define SUCCEEDED(hr)  ((HRESULT)(hr) >= 0)
#define FAILED(hr)     ((HRESULT)(hr) < 0)

/* GUID — 128비트 고유 식별자 */
typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t  Data4[8];
} GUID;

typedef GUID IID;
typedef const IID *REFIID;

/* IUnknown — 모든 COM 인터페이스의 루트 */
typedef struct IUnknownVtbl {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *This,
							  REFIID riid,
							  void **ppvObject);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *This);
	ULONG   (__attribute__((ms_abi)) *Release)(void *This);
} IUnknownVtbl;

/* ============================================================
 * 오디오 타입
 * ============================================================ */

/* WAVEFORMATEX — PCM 오디오 포맷 기술자 */
typedef struct {
	uint16_t wFormatTag;
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} WAVEFORMATEX;

#define WAVE_FORMAT_PCM 1

/* DSBUFFERDESC — DirectSound 버퍼 기술자 */
typedef struct {
	DWORD dwSize;
	DWORD dwFlags;
	DWORD dwBufferBytes;
	DWORD dwReserved;
	WAVEFORMATEX *lpwfxFormat;
} DSBUFFERDESC;

/* DirectSound 플래그 */
#define DSBCAPS_PRIMARYBUFFER  0x00000001
#define DSBCAPS_CTRLVOLUME     0x00000080
#define DSBCAPS_CTRLFREQUENCY  0x00000020
#define DSBCAPS_GLOBALFOCUS    0x00008000
#define DSSCL_PRIORITY         2
#define DSBPLAY_LOOPING        0x00000001

/* DS error codes */
#define DS_OK                  0
#define DSERR_GENERIC         ((HRESULT)0x80004005)

/* ============================================================
 * 스레딩 & 동기화 타입
 * ============================================================
 *
 * Win32 스레딩 모델:
 *   CreateThread → 스레드 핸들 반환 (HANDLE)
 *   WaitForSingleObject → 핸들이 시그널 상태가 될 때까지 대기
 *
 * 동기화 객체:
 *   Event — 시그널/논시그널 상태 (manual/auto reset)
 *   Mutex — 상호 배제 (재귀적)
 *   CriticalSection — 프로세스 내 경량 뮤텍스
 */

/* 스레드 프로시저 — ms_abi로 호출되는 콜백 */
typedef DWORD (__attribute__((ms_abi)) *LPTHREAD_START_ROUTINE)(void *);

/*
 * CRITICAL_SECTION — 경량 뮤텍스
 *
 * 실제 Windows: 40바이트 (DebugInfo, LockCount, RecursionCount 등)
 * 우리 구현: 내부적으로 pthread_mutex_t를 가리키는 포인터
 * 크기를 넉넉히 잡아 앱이 스택/구조체에 직접 선언해도 안전하게.
 */
typedef struct {
	void *DebugInfo;
	long LockCount;
	long RecursionCount;
	void *OwningThread;
	void *LockSemaphore;
	uintptr_t SpinCount;
} CRITICAL_SECTION;

/* SECURITY_ATTRIBUTES — 대부분 NULL로 전달되지만 선언 필요 */
typedef struct {
	DWORD nLength;
	void *lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;

/* ============================================================
 * 시간 타입
 * ============================================================ */

/* FILETIME — 100ns 단위, 1601-01-01 기준 */
typedef struct {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME;

/* LARGE_INTEGER — 64비트 정수 (union) */
typedef union {
	struct {
		DWORD LowPart;
		LONG  HighPart;
	};
	int64_t QuadPart;
} LARGE_INTEGER;

/* ============================================================
 * 파일 검색 타입
 * ============================================================ */

/* WIN32_FIND_DATAA — FindFirstFileA 결과 */
typedef struct {
	DWORD    dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD    nFileSizeHigh;
	DWORD    nFileSizeLow;
	DWORD    dwReserved0;
	DWORD    dwReserved1;
	char     cFileName[MAX_PATH];
	char     cAlternateFileName[14];
} WIN32_FIND_DATAA;

/* 파일 속성 상수 */
#define FILE_ATTRIBUTE_READONLY   0x00000001
#define FILE_ATTRIBUTE_DIRECTORY  0x00000010
#define FILE_ATTRIBUTE_ARCHIVE    0x00000020
#define INVALID_FILE_ATTRIBUTES   ((DWORD)-1)

/* GetFileType 반환값 */
#define FILE_TYPE_UNKNOWN  0x0000
#define FILE_TYPE_DISK     0x0001
#define FILE_TYPE_CHAR     0x0002
#define FILE_TYPE_PIPE     0x0003

/* ============================================================
 * 시스템 정보 타입
 * ============================================================ */

/* SYSTEM_INFO */
typedef struct {
	uint16_t wProcessorArchitecture;
	uint16_t wReserved;
	DWORD    dwPageSize;
	void    *lpMinimumApplicationAddress;
	void    *lpMaximumApplicationAddress;
	uintptr_t dwActiveProcessorMask;
	DWORD    dwNumberOfProcessors;
	DWORD    dwProcessorType;
	DWORD    dwAllocationGranularity;
	uint16_t wProcessorLevel;
	uint16_t wProcessorRevision;
} SYSTEM_INFO;

/* OSVERSIONINFOA */
typedef struct {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	char  szCSDVersion[128];
} OSVERSIONINFOA;

/* MEMORYSTATUS */
typedef struct {
	DWORD dwLength;
	DWORD dwMemoryLoad;
	uint64_t ullTotalPhys;
	uint64_t ullAvailPhys;
	uint64_t ullTotalPageFile;
	uint64_t ullAvailPageFile;
	uint64_t ullTotalVirtual;
	uint64_t ullAvailVirtual;
} MEMORYSTATUSEX;

/* PROCESSOR_ARCHITECTURE */
#define PROCESSOR_ARCHITECTURE_AMD64  9

/* ============================================================
 * XInput 타입 & 상수
 * ============================================================ */

/* XInput 버튼 비트마스크 */
#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000

/* XInput 에러 코드 */
#define ERROR_SUCCESS                 0
#define ERROR_DEVICE_NOT_CONNECTED    0x048F

/* XInput 디바이스 타입 */
#define XINPUT_DEVTYPE_GAMEPAD        0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD     0x01

/* XInput 최대 컨트롤러 수 */
#define XUSER_MAX_COUNT               4

typedef struct {
	uint16_t wButtons;
	uint8_t  bLeftTrigger;
	uint8_t  bRightTrigger;
	int16_t  sThumbLX;
	int16_t  sThumbLY;
	int16_t  sThumbRX;
	int16_t  sThumbRY;
} XINPUT_GAMEPAD;

typedef struct {
	uint32_t      dwPacketNumber;
	XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

typedef struct {
	uint16_t wLeftMotorSpeed;
	uint16_t wRightMotorSpeed;
} XINPUT_VIBRATION;

typedef struct {
	uint8_t  Type;
	uint8_t  SubType;
	uint16_t Flags;
	XINPUT_GAMEPAD Gamepad;
	XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES;

#endif /* CITC_WIN32_H */
