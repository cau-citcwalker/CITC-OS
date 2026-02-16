/*
 * reg_test.c — CITC OS WCL 레지스트리 테스트
 * =============================================
 *
 * Class 24에서 구현한 레지스트리 API를 테스트합니다:
 *   RegCreateKeyExA, RegSetValueExA, RegQueryValueExA, RegCloseKey
 *
 * 주의: /etc/citc-registry/ 디렉토리가 필요합니다.
 *   sudo mkdir -p /etc/citc-registry/HKLM/SOFTWARE
 *
 * 빌드:
 *   x86_64-w64-mingw32-gcc -nostdlib -o reg_test.exe reg_test.c \
 *       -lkernel32 -ladvapi32 -Wl,-e,_start
 *
 * 실행:
 *   citcrun reg_test.exe
 */

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
typedef const char *LPCSTR;
typedef const void *LPCVOID;
typedef unsigned long *LPDWORD;
typedef void *LPOVERLAPPED;
typedef unsigned char BYTE;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define NULL ((void *)0)
#define TRUE  1
#define FALSE 0

/* HKEY는 HANDLE과 동일 */
typedef HANDLE HKEY;

/* 미리 정의된 루트 키 핸들 */
#define HKEY_LOCAL_MACHINE ((HKEY)(unsigned long long)0x80000002)

/* 레지스트리 상수 */
#define REG_SZ          1
#define REG_DWORD       4
#define KEY_ALL_ACCESS  0xF003F
#define ERROR_SUCCESS   0

/* kernel32.dll 임포트 */
__declspec(dllimport) void __stdcall ExitProcess(UINT);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, LPCVOID, DWORD,
					       LPDWORD, LPOVERLAPPED);
__declspec(dllimport) DWORD __stdcall GetLastError(void);

/* advapi32.dll 임포트 */
__declspec(dllimport) DWORD __stdcall RegCreateKeyExA(
	HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPCSTR lpClass,
	DWORD dwOptions, DWORD samDesired, void *lpSecurityAttributes,
	HKEY *phkResult, DWORD *lpdwDisposition);

__declspec(dllimport) DWORD __stdcall RegSetValueExA(
	HKEY hKey, LPCSTR lpValueName, DWORD Reserved,
	DWORD dwType, const BYTE *lpData, DWORD cbData);

__declspec(dllimport) DWORD __stdcall RegQueryValueExA(
	HKEY hKey, LPCSTR lpValueName, DWORD *lpReserved,
	DWORD *lpType, BYTE *lpData, DWORD *lpcbData);

__declspec(dllimport) DWORD __stdcall RegCloseKey(HKEY hKey);

/* === 유틸리티 === */

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
	DWORD ret;

	print(out, "=== Registry API Test (Class 24) ===\n\n");

	/* 1. RegCreateKeyExA — 키 생성 */
	print(out, "[1] RegCreateKeyExA(HKLM\\SOFTWARE\\CitcTest)... ");
	HKEY hKey = NULL;
	DWORD disposition = 0;

	ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
			      "SOFTWARE\\CitcTest",
			      0, NULL, 0,
			      KEY_ALL_ACCESS, NULL,
			      &hKey, &disposition);
	if (ret == ERROR_SUCCESS && hKey) {
		if (disposition == 1)
			print(out, "OK (created)\n");
		else
			print(out, "OK (opened existing)\n");
		pass++;
	} else {
		print(out, "FAIL (error=");
		print_num(out, ret);
		print(out, ")\n");
		fail++;
	}

	/* 2. RegSetValueExA — REG_SZ 값 쓰기 */
	print(out, "[2] RegSetValueExA(\"TestStr\", \"Hello Registry!\")... ");
	const char *test_str = "Hello Registry!";
	DWORD str_len = 0;

	while (test_str[str_len])
		str_len++;
	str_len++; /* 널 종료 포함 */

	ret = RegSetValueExA(hKey, "TestStr", 0, REG_SZ,
			     (const BYTE *)test_str, str_len);
	if (ret == ERROR_SUCCESS) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL (error=");
		print_num(out, ret);
		print(out, ")\n");
		fail++;
	}

	/* 3. RegSetValueExA — REG_DWORD 값 쓰기 */
	print(out, "[3] RegSetValueExA(\"TestDword\", 42)... ");
	DWORD test_dword = 42;

	ret = RegSetValueExA(hKey, "TestDword", 0, REG_DWORD,
			     (const BYTE *)&test_dword, sizeof(test_dword));
	if (ret == ERROR_SUCCESS) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL (error=");
		print_num(out, ret);
		print(out, ")\n");
		fail++;
	}

	/* 4. RegQueryValueExA — REG_SZ 읽기 */
	print(out, "[4] RegQueryValueExA(\"TestStr\")... ");
	char read_buf[128];
	DWORD read_len = sizeof(read_buf);
	DWORD read_type = 0;

	for (int i = 0; i < 128; i++)
		read_buf[i] = 0;

	ret = RegQueryValueExA(hKey, "TestStr", NULL,
			       &read_type, (BYTE *)read_buf, &read_len);
	if (ret == ERROR_SUCCESS && read_type == REG_SZ &&
	    str_eq(read_buf, "Hello Registry!")) {
		print(out, "OK (\"");
		print(out, read_buf);
		print(out, "\")\n");
		pass++;
	} else {
		print(out, "FAIL (error=");
		print_num(out, ret);
		print(out, " type=");
		print_num(out, read_type);
		print(out, ")\n");
		fail++;
	}

	/* 5. RegQueryValueExA — REG_DWORD 읽기 */
	print(out, "[5] RegQueryValueExA(\"TestDword\")... ");
	DWORD read_dword = 0;

	read_len = sizeof(read_dword);
	read_type = 0;

	ret = RegQueryValueExA(hKey, "TestDword", NULL,
			       &read_type, (BYTE *)&read_dword, &read_len);
	if (ret == ERROR_SUCCESS && read_type == REG_DWORD &&
	    read_dword == 42) {
		print(out, "OK (value=");
		print_num(out, read_dword);
		print(out, ")\n");
		pass++;
	} else {
		print(out, "FAIL (error=");
		print_num(out, ret);
		print(out, " value=");
		print_num(out, read_dword);
		print(out, ")\n");
		fail++;
	}

	/* 6. RegCloseKey */
	print(out, "[6] RegCloseKey... ");
	ret = RegCloseKey(hKey);
	if (ret == ERROR_SUCCESS) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL\n");
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
