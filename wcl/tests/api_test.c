/*
 * api_test.c — CITC OS WCL API 테스트
 * ======================================
 *
 * Class 25에서 추가한 kernel32 API를 테스트합니다:
 *   VirtualAlloc/Free, HeapAlloc/Free,
 *   GetEnvironmentVariableA, SetEnvironmentVariableA,
 *   GetCommandLineA, GetCurrentProcessId,
 *   GetModuleHandleA
 *
 * 빌드:
 *   x86_64-w64-mingw32-gcc -nostdlib -o api_test.exe api_test.c \
 *       -lkernel32 -Wl,-e,_start
 *
 * 실행:
 *   citcrun api_test.exe
 */

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
typedef const char *LPCSTR;
typedef const void *LPCVOID;
typedef void *LPVOID;
typedef unsigned long *LPDWORD;
typedef void *LPOVERLAPPED;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define NULL ((void *)0)
#define TRUE  1
#define FALSE 0

/* 메모리 상수 */
#define MEM_COMMIT     0x00001000
#define MEM_RESERVE    0x00002000
#define MEM_RELEASE    0x00008000
#define PAGE_READWRITE 0x04
#define HEAP_ZERO_MEMORY 0x00000008

/* kernel32.dll 임포트 */
__declspec(dllimport) void __stdcall ExitProcess(UINT);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, LPCVOID, DWORD,
					       LPDWORD, LPOVERLAPPED);

/* 메모리 API */
__declspec(dllimport) LPVOID __stdcall VirtualAlloc(LPVOID, DWORD,
						    DWORD, DWORD);
__declspec(dllimport) BOOL __stdcall VirtualFree(LPVOID, DWORD, DWORD);
__declspec(dllimport) HANDLE __stdcall GetProcessHeap(void);
__declspec(dllimport) LPVOID __stdcall HeapAlloc(HANDLE, DWORD, DWORD);
__declspec(dllimport) BOOL __stdcall HeapFree(HANDLE, DWORD, LPVOID);

/* 환경/프로세스 API */
__declspec(dllimport) DWORD __stdcall GetEnvironmentVariableA(LPCSTR, char *,
							      DWORD);
__declspec(dllimport) BOOL __stdcall SetEnvironmentVariableA(LPCSTR, LPCSTR);
__declspec(dllimport) LPCSTR __stdcall GetCommandLineA(void);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);

/* 모듈 API */
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(LPCSTR);

/* === 유틸리티 (CRT 없이) === */

static void print(HANDLE out, const char *s)
{
	DWORD written;
	DWORD len = 0;

	while (s[len])
		len++;
	WriteFile(out, s, len, &written, NULL);
}

static void print_num(HANDLE out, DWORD num)
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
	WriteFile(out, rev, (DWORD)i, &written, NULL);
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

/* === 테스트 시작 === */

void _start(void)
{
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	int pass = 0;
	int fail = 0;

	print(out, "=== Win32 API Test (Class 25) ===\n\n");

	/* 1. VirtualAlloc + VirtualFree */
	print(out, "[1] VirtualAlloc(4096, MEM_COMMIT, PAGE_READWRITE)... ");
	void *mem = VirtualAlloc(NULL, 4096,
				 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (mem) {
		/* 할당된 메모리에 읽기/쓰기 */
		*(int *)mem = 0xDEADBEEF;
		if (*(int *)mem == (int)0xDEADBEEF) {
			print(out, "OK\n");
			pass++;
		} else {
			print(out, "FAIL (read mismatch)\n");
			fail++;
		}
	} else {
		print(out, "FAIL (NULL)\n");
		fail++;
	}

	/* 2. VirtualFree */
	print(out, "[2] VirtualFree... ");
	if (mem) {
		BOOL ok = VirtualFree(mem, 0, MEM_RELEASE);
		if (ok) {
			print(out, "OK\n");
			pass++;
		} else {
			print(out, "FAIL\n");
			fail++;
		}
	} else {
		print(out, "SKIP (no alloc)\n");
	}

	/* 3. GetProcessHeap + HeapAlloc */
	print(out, "[3] GetProcessHeap... ");
	HANDLE heap = GetProcessHeap();
	if (heap) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL (NULL)\n");
		fail++;
	}

	print(out, "[4] HeapAlloc(256, HEAP_ZERO_MEMORY)... ");
	char *hbuf = (char *)HeapAlloc(heap, HEAP_ZERO_MEMORY, 256);
	if (hbuf) {
		/* HEAP_ZERO_MEMORY: 0으로 초기화되었는지 확인 */
		int zeroed = 1;
		for (int i = 0; i < 256; i++) {
			if (hbuf[i] != 0) {
				zeroed = 0;
				break;
			}
		}
		if (zeroed) {
			print(out, "OK (zeroed)\n");
			pass++;
		} else {
			print(out, "FAIL (not zeroed)\n");
			fail++;
		}
	} else {
		print(out, "FAIL (NULL)\n");
		fail++;
	}

	/* 5. HeapFree */
	print(out, "[5] HeapFree... ");
	if (hbuf) {
		BOOL ok = HeapFree(heap, 0, hbuf);
		if (ok) {
			print(out, "OK\n");
			pass++;
		} else {
			print(out, "FAIL\n");
			fail++;
		}
	} else {
		print(out, "SKIP\n");
	}

	/* 6. SetEnvironmentVariableA + GetEnvironmentVariableA */
	print(out, "[6] SetEnvironmentVariableA(\"CITC_TEST\", \"hello\")... ");
	BOOL ok = SetEnvironmentVariableA("CITC_TEST", "hello");
	if (ok) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL\n");
		fail++;
	}

	print(out, "[7] GetEnvironmentVariableA(\"CITC_TEST\")... ");
	char env_buf[64];
	DWORD env_len = GetEnvironmentVariableA("CITC_TEST", env_buf, 64);
	if (env_len > 0 && str_eq(env_buf, "hello")) {
		print(out, "OK (\"");
		print(out, env_buf);
		print(out, "\")\n");
		pass++;
	} else {
		print(out, "FAIL\n");
		fail++;
	}

	/* 8. GetCommandLineA */
	print(out, "[8] GetCommandLineA... ");
	const char *cmdline = GetCommandLineA();
	if (cmdline && cmdline[0]) {
		print(out, "OK (\"");
		print(out, cmdline);
		print(out, "\")\n");
		pass++;
	} else {
		print(out, "FAIL (NULL or empty)\n");
		fail++;
	}

	/* 9. GetCurrentProcessId */
	print(out, "[9] GetCurrentProcessId... ");
	DWORD pid = GetCurrentProcessId();
	if (pid > 0) {
		print(out, "OK (pid=");
		print_num(out, pid);
		print(out, ")\n");
		pass++;
	} else {
		print(out, "FAIL (0)\n");
		fail++;
	}

	/* 10. GetModuleHandleA(NULL) */
	print(out, "[10] GetModuleHandleA(NULL)... ");
	HANDLE hmod = GetModuleHandleA(NULL);
	if (hmod) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL (NULL)\n");
		fail++;
	}

	/* 결과 요약 */
	print(out, "\n=== Result: ");
	print_num(out, (DWORD)pass);
	print(out, " passed, ");
	print_num(out, (DWORD)fail);
	print(out, " failed ===\n");

	ExitProcess(fail > 0 ? 1 : 0);
}
