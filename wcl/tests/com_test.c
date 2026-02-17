/*
 * com_test.c — CITC OS WCL COM 런타임 테스트
 * =============================================
 *
 * Class 50에서 구현한 ole32.dll COM API를 테스트:
 *   CoInitializeEx, CoCreateInstance, IsEqualGUID,
 *   CoTaskMemAlloc/Free, CoUninitialize
 *
 * 빌드:
 *   x86_64-w64-mingw32-gcc -nostdlib -o com_test.exe com_test.c \
 *       -lkernel32 -lole32 -Wl,-e,_start
 *
 * 실행:
 *   citcrun com_test.exe
 */

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
typedef const char *LPCSTR;
typedef const void *LPCVOID;
typedef unsigned long *LPDWORD;
typedef void *LPOVERLAPPED;
typedef int HRESULT;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define NULL ((void *)0)
#define S_OK     ((HRESULT)0)
#define S_FALSE  ((HRESULT)1)

/* GUID */
typedef struct {
	unsigned int Data1;
	unsigned short Data2;
	unsigned short Data3;
	unsigned char Data4[8];
} GUID;

typedef const GUID *REFIID;
typedef const GUID *REFCLSID;

/* COM 상수 */
#define COINIT_MULTITHREADED     0x0
#define CLSCTX_INPROC_SERVER     0x1
#define CLSCTX_ALL               0x17

/* CLSID_DirectSound8 */
static const GUID CLSID_DirectSound8 = {
	0x3901CC3F, 0x84B5, 0x4FA4,
	{ 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B }
};

/* 가짜 CLSID (등록 안 된 것) */
static const GUID CLSID_Fake = {
	0xDEADBEEF, 0x0000, 0x0000,
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

/* IID_IUnknown (비교 테스트용) */
static const GUID IID_IUnknown = {
	0x00000000, 0x0000, 0x0000,
	{ 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};

/* kernel32.dll */
__declspec(dllimport) void __stdcall ExitProcess(UINT);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, LPCVOID, DWORD,
					       LPDWORD, LPOVERLAPPED);

/* ole32.dll */
__declspec(dllimport) HRESULT __stdcall CoInitializeEx(void *, DWORD);
__declspec(dllimport) void __stdcall CoUninitialize(void);
__declspec(dllimport) HRESULT __stdcall CoCreateInstance(
	REFCLSID rclsid, void *pUnkOuter, DWORD dwClsContext,
	REFIID riid, void **ppv);
__declspec(dllimport) void *__stdcall CoTaskMemAlloc(unsigned long long cb);
__declspec(dllimport) void __stdcall CoTaskMemFree(void *pv);
__declspec(dllimport) int __stdcall IsEqualGUID(const GUID *, const GUID *);

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

static void print_hex(HANDLE out, unsigned int val)
{
	char buf[12] = "0x";
	const char *hex = "0123456789ABCDEF";

	for (int i = 7; i >= 0; i--)
		buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
	buf[10] = '\0';

	DWORD written;

	WriteFile(out, buf, 10, &written, NULL);
}

/* === 테스트 시작 === */

void _start(void)
{
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	int pass = 0;
	int fail = 0;

	print(out, "=== COM Runtime Test (Class 50) ===\n\n");

	/* 1. CoInitializeEx(COINIT_MULTITHREADED) → S_OK */
	print(out, "[1] CoInitializeEx(COINIT_MULTITHREADED)... ");
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

	if (hr == S_OK) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL (hr=");
		print_hex(out, (unsigned int)hr);
		print(out, ")\n");
		fail++;
	}

	/* 2. CoInitializeEx 재호출 → S_FALSE (이미 초기화) */
	print(out, "[2] CoInitializeEx again -> S_FALSE... ");
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

	if (hr == S_FALSE) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL (hr=");
		print_hex(out, (unsigned int)hr);
		print(out, ")\n");
		fail++;
	}

	/* 3. IsEqualGUID — 동일 GUID */
	print(out, "[3] IsEqualGUID(same) == TRUE... ");
	if (IsEqualGUID(&CLSID_DirectSound8, &CLSID_DirectSound8)) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL\n");
		fail++;
	}

	/* 4. IsEqualGUID — 다른 GUID */
	print(out, "[4] IsEqualGUID(different) == FALSE... ");
	if (!IsEqualGUID(&CLSID_DirectSound8, &IID_IUnknown)) {
		print(out, "OK\n");
		pass++;
	} else {
		print(out, "FAIL\n");
		fail++;
	}

	/* 5. CoTaskMemAlloc + 쓰기 + Free */
	print(out, "[5] CoTaskMemAlloc(256) + Free... ");
	{
		char *mem = (char *)CoTaskMemAlloc(256);

		if (mem) {
			/* 쓰기 테스트 */
			mem[0] = 'T';
			mem[1] = 'E';
			mem[2] = 'S';
			mem[3] = 'T';
			mem[4] = '\0';

			BOOL ok = (mem[0] == 'T' && mem[3] == 'T');

			CoTaskMemFree(mem);

			if (ok) {
				print(out, "OK\n");
				pass++;
			} else {
				print(out, "FAIL (data)\n");
				fail++;
			}
		} else {
			print(out, "FAIL (NULL)\n");
			fail++;
		}
	}

	/* 6. CoCreateInstance(CLSID_DirectSound8) → IDirectSound8 */
	print(out, "[6] CoCreateInstance(CLSID_DirectSound8)... ");
	{
		void *pDS8 = NULL;

		hr = CoCreateInstance(&CLSID_DirectSound8, NULL,
				      CLSCTX_ALL, &IID_IUnknown, &pDS8);
		if (hr == 0 && pDS8 != NULL) {
			print(out, "OK (pDS8=");
			print_hex(out, (unsigned int)(unsigned long long)pDS8);
			print(out, ")\n");
			pass++;

			/* Release — vtable[2] = Release */
			/* IUnknown Release 호출 */
			void **vtbl_ptr = *(void ***)pDS8;

			if (vtbl_ptr) {
				typedef unsigned long (__attribute__((ms_abi))
						      *release_fn)(void *);
				release_fn rel = (release_fn)vtbl_ptr[2];

				rel(pDS8);
			}
		} else {
			print(out, "FAIL (hr=");
			print_hex(out, (unsigned int)hr);
			print(out, ")\n");
			fail++;
		}
	}

	/* 7. CoCreateInstance(CLSID_Fake) → E_NOINTERFACE */
	print(out, "[7] CoCreateInstance(CLSID_Fake) -> fail... ");
	{
		void *pFake = NULL;

		hr = CoCreateInstance(&CLSID_Fake, NULL,
				      CLSCTX_ALL, &IID_IUnknown, &pFake);
		if (hr != 0 && pFake == NULL) {
			print(out, "OK (correctly rejected)\n");
			pass++;
		} else {
			print(out, "FAIL (should have failed)\n");
			fail++;
		}
	}

	/* 8. CoUninitialize → 정상 종료 */
	print(out, "[8] CoUninitialize... ");
	CoUninitialize();
	print(out, "OK\n");
	pass++;

	/* 결과 요약 */
	print(out, "\n=== Result: ");
	print_num(out, (DWORD)pass);
	print(out, " passed, ");
	print_num(out, (DWORD)fail);
	print(out, " failed ===\n");

	ExitProcess(fail > 0 ? 1 : 0);
}
