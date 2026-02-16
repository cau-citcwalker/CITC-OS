/*
 * file_test.c — CITC OS WCL 파일 I/O 테스트
 * =============================================
 *
 * kernel32.dll의 파일 I/O 함수를 테스트합니다:
 *   CreateFileA, WriteFile, ReadFile, CloseHandle,
 *   GetFileSize, DeleteFileA, GetLastError
 *
 * CRT(C Runtime)를 사용하지 않습니다 — printf 대신 WriteFile로 출력.
 *
 * 빌드 (Linux에서 크로스 컴파일):
 *   x86_64-w64-mingw32-gcc -nostdlib -o file_test.exe file_test.c -lkernel32
 *
 * 실행:
 *   citcrun file_test.exe
 */

/* Windows API 선언 — windows.h 대신 직접 선언 (미니멀) */
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
typedef long LONG;
typedef const char *LPCSTR;
typedef const void *LPCVOID;
typedef void *LPVOID;
typedef unsigned long *LPDWORD;
typedef void *LPOVERLAPPED;

#define STD_OUTPUT_HANDLE  ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(DWORD*)-1)
#define GENERIC_READ    0x80000000UL
#define GENERIC_WRITE   0x40000000UL
#define CREATE_ALWAYS   2
#define OPEN_EXISTING   3
#define FILE_BEGIN      0
#define TRUE  1
#define FALSE 0

__declspec(dllimport) void __stdcall ExitProcess(UINT);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, LPCVOID, DWORD,
					       LPDWORD, LPOVERLAPPED);
__declspec(dllimport) BOOL __stdcall ReadFile(HANDLE, LPVOID, DWORD,
					      LPDWORD, LPOVERLAPPED);
__declspec(dllimport) HANDLE __stdcall CreateFileA(LPCSTR, DWORD, DWORD,
						   void *, DWORD, DWORD,
						   HANDLE);
__declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE);
__declspec(dllimport) DWORD __stdcall GetFileSize(HANDLE, LPDWORD);
__declspec(dllimport) DWORD __stdcall SetFilePointer(HANDLE, LONG,
						     LONG *, DWORD);
__declspec(dllimport) BOOL __stdcall DeleteFileA(LPCSTR);
__declspec(dllimport) DWORD __stdcall GetLastError(void);

/* 콘솔에 문자열 출력 (CRT 없이) */
static void print(HANDLE out, const char *s)
{
	DWORD written;
	DWORD len = 0;

	while (s[len])
		len++;
	WriteFile(out, s, len, &written, 0);
}

/* 숫자를 문자열로 변환 (CRT 없이 itoa 구현) */
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

	/* 뒤집기 */
	DWORD written;
	char rev[16];

	for (int j = 0; j < i; j++)
		rev[j] = buf[i - 1 - j];
	WriteFile(out, rev, (DWORD)i, &written, 0);
}

void _start(void)
{
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	const char test_data[] = "Hello from Win32 File I/O!\n";
	DWORD written, bytes_read;
	char read_buf[128];

	print(out, "=== Win32 File I/O Test ===\n\n");

	/* 1. 파일 생성 + 쓰기 */
	print(out, "[1] CreateFileA(\"test.txt\", WRITE, CREATE_ALWAYS)... ");
	HANDLE hFile = CreateFileA("test.txt",
				   GENERIC_WRITE,
				   0, 0,
				   CREATE_ALWAYS,
				   0, 0);
	if (hFile == INVALID_HANDLE_VALUE) {
		print(out, "FAIL!\n");
		ExitProcess(1);
	}
	print(out, "OK\n");

	/* 2. WriteFile */
	print(out, "[2] WriteFile(\"Hello from Win32 File I/O!\\n\")... ");
	BOOL ok = WriteFile(hFile, test_data, sizeof(test_data) - 1,
			    &written, 0);
	if (!ok) {
		print(out, "FAIL!\n");
		ExitProcess(1);
	}
	print(out, "OK (");
	print_num(out, written);
	print(out, " bytes)\n");

	/* 3. CloseHandle */
	print(out, "[3] CloseHandle... ");
	CloseHandle(hFile);
	print(out, "OK\n");

	/* 4. 다시 열기 (읽기 모드) */
	print(out, "[4] CreateFileA(\"test.txt\", READ, OPEN_EXISTING)... ");
	hFile = CreateFileA("test.txt",
			    GENERIC_READ,
			    0, 0,
			    OPEN_EXISTING,
			    0, 0);
	if (hFile == INVALID_HANDLE_VALUE) {
		print(out, "FAIL!\n");
		ExitProcess(1);
	}
	print(out, "OK\n");

	/* 5. GetFileSize */
	print(out, "[5] GetFileSize... ");
	DWORD size = GetFileSize(hFile, 0);
	print_num(out, size);
	print(out, " bytes\n");

	/* 6. ReadFile */
	print(out, "[6] ReadFile... ");
	ok = ReadFile(hFile, read_buf, sizeof(read_buf) - 1,
		      &bytes_read, 0);
	if (!ok) {
		print(out, "FAIL!\n");
		ExitProcess(1);
	}
	print(out, "OK (");
	print_num(out, bytes_read);
	print(out, " bytes): ");
	WriteFile(out, read_buf, bytes_read, &written, 0);

	/* 7. CloseHandle */
	print(out, "[7] CloseHandle... ");
	CloseHandle(hFile);
	print(out, "OK\n");

	/* 8. DeleteFileA */
	print(out, "[8] DeleteFileA(\"test.txt\")... ");
	ok = DeleteFileA("test.txt");
	if (!ok) {
		print(out, "FAIL!\n");
		ExitProcess(1);
	}
	print(out, "OK\n");

	/* 9. 삭제 확인 — 다시 열면 실패해야 함 */
	print(out, "[9] CreateFileA(OPEN_EXISTING) after delete... ");
	hFile = CreateFileA("test.txt",
			    GENERIC_READ,
			    0, 0,
			    OPEN_EXISTING,
			    0, 0);
	if (hFile == INVALID_HANDLE_VALUE) {
		print(out, "FAIL (expected!) error=");
		print_num(out, GetLastError());
		print(out, "\n");
	} else {
		print(out, "unexpected OK?!\n");
		CloseHandle(hFile);
	}

	print(out, "\n=== All tests passed! ===\n");
	ExitProcess(0);
}
