/*
 * hello_win.c — CITC OS WCL 테스트 프로그램
 * ============================================
 *
 * 최소한의 Windows 프로그램:
 *   1. GetStdHandle로 stdout 핸들 얻기
 *   2. WriteFile로 메시지 출력
 *   3. ExitProcess로 종료
 *
 * CRT(C Runtime)를 사용하지 않습니다 — _start를 직접 정의.
 * 이렇게 하면 msvcrt.dll 없이도 실행 가능.
 * (msvcrt.dll의 수백 개 함수를 구현할 필요 없음!)
 *
 * 빌드 (Linux에서 크로스 컴파일):
 *   x86_64-w64-mingw32-gcc -nostdlib -o hello.exe hello_win.c -lkernel32
 *
 * 플래그 설명:
 *   -nostdlib: C 런타임 링크 안 함 (printf, main 등 사용 불가)
 *   -lkernel32: kernel32.dll의 임포트 라이브러리 링크
 *     → .exe의 임포트 테이블에 kernel32.dll 함수가 등록됨
 *     → 실행 시 로더(citcrun)가 이 함수들을 스텁으로 연결
 *
 * 실행:
 *   citcrun hello.exe
 */

/* Windows API 선언 — windows.h 대신 직접 선언 (미니멀) */
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
typedef const void *LPCVOID;
typedef unsigned long *LPDWORD;
typedef void *LPOVERLAPPED;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) void __stdcall ExitProcess(UINT uExitCode);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE hFile,
					       LPCVOID lpBuffer,
					       DWORD nNumberOfBytesToWrite,
					       LPDWORD lpNumberOfBytesWritten,
					       LPOVERLAPPED lpOverlapped);

/*
 * _start — 프로그램 엔트리포인트
 *
 * -nostdlib에서는 _start가 엔트리포인트.
 * 보통 Windows에서는 _mainCRTStartup → main() 체인이지만,
 * CRT 없이는 _start에서 직접 시작합니다.
 */
void _start(void)
{
	const char msg[] = "Hello from Windows .exe on CITC OS!\n";
	DWORD written;

	HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	WriteFile(hStdout, msg, sizeof(msg) - 1, &written, 0);

	ExitProcess(0);
}
