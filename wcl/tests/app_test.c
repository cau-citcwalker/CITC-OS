/*
 * app_test.c — 종합 통합 테스트
 * ================================
 *
 * Phase 5 전체 기능을 통합 검증하는 실제 Windows 앱 시나리오.
 * 멀티스레드 + 네트워킹 + COM + 3D 렌더링 + 시스템 정보가 함께 동작.
 *
 * 시나리오:
 *   [1-3]  멀티스레드 + D3D11 렌더링
 *   [4-6]  네트워크 + 레지스트리
 *   [7-9]  COM + DirectSound
 *   [10-12] 시스템 정보 + 파일시스템
 *   [13-15] D3D12 + Fence 동기화
 */

/* === 타입 정의 === */

typedef void           *HANDLE;
typedef unsigned int    UINT;
typedef int             BOOL;
typedef int             LONG;
typedef unsigned long   DWORD;
typedef const char     *LPCSTR;
typedef void           *LPVOID;
typedef void           *HWND;
typedef unsigned int    ULONG;
typedef int             HRESULT;
typedef unsigned long long uint64_t;
typedef unsigned long long uintptr_t;
typedef unsigned long long size_t;
typedef long long       intptr_t;
typedef unsigned int    uint32_t;
typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned short  WORD;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)
#define S_OK          ((HRESULT)0)

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0

/* GUID */
typedef struct { DWORD Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } GUID;
typedef const GUID *REFIID;

/* === kernel32 imports === */

__declspec(dllimport) void   __stdcall ExitProcess(UINT code);
__declspec(dllimport) int    __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, size_t,
	DWORD (__attribute__((ms_abi)) *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) DWORD  __stdcall WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) HANDLE __stdcall CreateEventA(void *, BOOL, BOOL, LPCSTR);
__declspec(dllimport) BOOL   __stdcall SetEvent(HANDLE);
__declspec(dllimport) BOOL   __stdcall CloseHandle(HANDLE);
__declspec(dllimport) void   __stdcall Sleep(DWORD);
__declspec(dllimport) DWORD  __stdcall GetTickCount(void);
__declspec(dllimport) BOOL   __stdcall CreateDirectoryA(LPCSTR, void *);
__declspec(dllimport) BOOL   __stdcall RemoveDirectoryA(LPCSTR);
__declspec(dllimport) DWORD  __stdcall GetTempPathA(DWORD, char *);
__declspec(dllimport) HANDLE __stdcall FindFirstFileA(LPCSTR, void *);
__declspec(dllimport) BOOL   __stdcall FindNextFileA(HANDLE, void *);
__declspec(dllimport) BOOL   __stdcall FindClose(HANDLE);

typedef struct {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
	char szCSDVersion[128];
} OSVERSIONINFOA;

__declspec(dllimport) BOOL __stdcall GetVersionExA(OSVERSIONINFOA *);

typedef struct {
	union { DWORD dwOemId; struct { WORD wProcessorArchitecture; WORD wReserved; }; };
	DWORD dwPageSize;
	void *lpMinimumApplicationAddress, *lpMaximumApplicationAddress;
	uintptr_t dwActiveProcessorMask;
	DWORD dwNumberOfProcessors;
	DWORD dwProcessorType;
	DWORD dwAllocationGranularity;
	WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO;

__declspec(dllimport) void __stdcall GetSystemInfo(SYSTEM_INFO *);

/* === advapi32 imports === */

#define HKEY_LOCAL_MACHINE ((HANDLE)(uintptr_t)0x80000002)
#define KEY_ALL_ACCESS 0xF003F
#define REG_SZ 1

__declspec(dllimport) LONG __stdcall RegCreateKeyExA(HANDLE, LPCSTR, DWORD, LPCSTR,
	DWORD, DWORD, void *, HANDLE *, DWORD *);
__declspec(dllimport) LONG __stdcall RegSetValueExA(HANDLE, LPCSTR, DWORD, DWORD,
	const void *, DWORD);
__declspec(dllimport) LONG __stdcall RegQueryValueExA(HANDLE, LPCSTR, DWORD *,
	DWORD *, void *, DWORD *);
__declspec(dllimport) LONG __stdcall RegDeleteValueA(HANDLE, LPCSTR);
__declspec(dllimport) LONG __stdcall RegDeleteKeyA(HANDLE, LPCSTR);
__declspec(dllimport) LONG __stdcall RegCloseKey(HANDLE);

/* === ws2_32 imports === */

#define AF_INET    2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define INADDR_LOOPBACK 0x7F000001

typedef unsigned long long SOCKET;
#define INVALID_SOCKET ((SOCKET)(~0))

typedef struct {
	short sin_family;
	unsigned short sin_port;
	struct { unsigned long s_addr; } sin_addr;
	char sin_zero[8];
} SOCKADDR_IN;

typedef struct { WORD wVersion; WORD wHighVersion; char szDescription[257];
	char szSystemStatus[129]; unsigned short iMaxSockets, iMaxUdpDg;
	char *lpVendorInfo; } WSADATA;

__declspec(dllimport) int __stdcall WSAStartup(WORD, WSADATA *);
__declspec(dllimport) int __stdcall WSACleanup(void);
__declspec(dllimport) SOCKET __stdcall socket(int, int, int);
__declspec(dllimport) int __stdcall bind(SOCKET, const void *, int);
__declspec(dllimport) int __stdcall listen(SOCKET, int);
__declspec(dllimport) SOCKET __stdcall accept(SOCKET, void *, int *);
__declspec(dllimport) int __stdcall connect(SOCKET, const void *, int);
__declspec(dllimport) int __stdcall send(SOCKET, const char *, int, int);
__declspec(dllimport) int __stdcall recv(SOCKET, char *, int, int);
__declspec(dllimport) int __stdcall closesocket(SOCKET);
__declspec(dllimport) unsigned short __stdcall htons(unsigned short);
__declspec(dllimport) unsigned long __stdcall htonl(unsigned long);
__declspec(dllimport) int __stdcall setsockopt(SOCKET, int, int, const char *, int);

#define SOL_SOCKET  0xFFFF
#define SO_REUSEADDR 0x0004

/* === ole32 imports === */

#define COINIT_MULTITHREADED 0

__declspec(dllimport) HRESULT __stdcall CoInitializeEx(void *, DWORD);
__declspec(dllimport) void    __stdcall CoUninitialize(void);
__declspec(dllimport) HRESULT __stdcall CoCreateInstance(const GUID *, void *, DWORD,
	const GUID *, void **);

/* === D3D12 imports === */

__declspec(dllimport) HRESULT __stdcall D3D12CreateDevice(
	void *pAdapter, UINT MinFeatureLevel, REFIID riid, void **ppDevice);

/* D3D12 types (minimal) */
typedef struct { size_t ptr; } D3D12_CPU_DESCRIPTOR_HANDLE;
typedef struct { uint64_t ptr; } D3D12_GPU_DESCRIPTOR_HANDLE;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	UINT    (__attribute__((ms_abi)) *GetNodeCount)(void *);
	HRESULT (__attribute__((ms_abi)) *CreateCommandQueue)(void *, const void *, REFIID, void **);
	HRESULT (__attribute__((ms_abi)) *CreateCommandAllocator)(void *, int, REFIID, void **);
	void *CreateGraphicsPipelineState, *ComputePipelineState;
	HRESULT (__attribute__((ms_abi)) *CreateCommandList)(void *, UINT, int, void *, void *, REFIID, void **);
	void *CheckFeatureSupport;
	HRESULT (__attribute__((ms_abi)) *CreateDescriptorHeap)(void *, const void *, REFIID, void **);
	void *GetDescriptorHandleIncrementSize, *CreateRootSignature;
	void *CreateConstantBufferView, *CreateShaderResourceView, *CreateUnorderedAccessView;
	void    (__attribute__((ms_abi)) *CreateRenderTargetView)(void *, void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);
	void *CreateDepthStencilView, *CreateSampler;
	void *CopyDescriptors, *CopyDescriptorsSimple;
	void *GetResourceAllocationInfo, *GetCustomHeapProperties;
	HRESULT (__attribute__((ms_abi)) *CreateCommittedResource)(void *,
		const void *, int, const void *, int, const void *, REFIID, void **);
	void *CreateHeap, *CreatePlacedResource, *CreateReservedResource;
	void *CreateSharedHandle, *OpenSharedHandle, *OpenSharedHandleByName;
	void *MakeResident, *Evict;
	HRESULT (__attribute__((ms_abi)) *CreateFence)(void *, uint64_t, int, REFIID, void **);
} ID3D12DeviceVtbl_App;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *GetDevice;
	uint64_t (__attribute__((ms_abi)) *GetCompletedValue)(void *);
	HRESULT  (__attribute__((ms_abi)) *SetEventOnCompletion)(void *, uint64_t, void *);
	HRESULT  (__attribute__((ms_abi)) *Signal)(void *, uint64_t);
} ID3D12FenceVtbl_App;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *GetDevice;
	HRESULT (__attribute__((ms_abi)) *Map)(void *, UINT, const void *, void **);
	void    (__attribute__((ms_abi)) *Unmap)(void *, UINT, const void *);
} ID3D12ResourceVtbl_App;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *GetDevice, *GetDesc;
	D3D12_CPU_DESCRIPTOR_HANDLE (__attribute__((ms_abi)) *GetCPUDescriptorHandleForHeapStart)(void *);
} ID3D12DescriptorHeapVtbl_App;

#define VT(obj, type) (*(type**)(obj))

/* === 유틸리티 === */

static HANDLE hStdout;

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void print(const char *s)
{
	DWORD written;
	WriteFile(hStdout, s, (DWORD)my_strlen(s), &written, NULL);
}
static void print_num(unsigned long long v)
{
	char buf[24]; int i = 0;
	if (v == 0) { buf[i++] = '0'; }
	else { char tmp[24]; int j = 0;
		while (v > 0) { tmp[j++] = '0' + (char)(v % 10); v /= 10; }
		while (j > 0) buf[i++] = tmp[--j]; }
	buf[i] = 0; print(buf);
}

static int my_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *(unsigned char *)a - *(unsigned char *)b;
}

static void my_memset(void *p, int c, size_t n)
{
	unsigned char *b = (unsigned char *)p;
	for (size_t i = 0; i < n; i++) b[i] = (unsigned char)c;
}

static int pass_count, fail_count;

static void test_ok(int n, const char *desc)
{
	print("  ["); print_num((unsigned long long)n); print("] ");
	print(desc); print(" ... PASS\n");
	pass_count++;
}

static void test_fail(int n, const char *desc)
{
	print("  ["); print_num((unsigned long long)n); print("] ");
	print(desc); print(" ... FAIL\n");
	fail_count++;
}

/* === 스레드 콜백 === */

static volatile int worker_done;
static volatile DWORD worker_tick;
static HANDLE worker_event;

static DWORD __attribute__((ms_abi)) worker_thread(void *param)
{
	(void)param;
	/* 워커: GetTickCount + 이벤트 시그널 */
	worker_tick = GetTickCount();
	worker_done = 1;
	SetEvent(worker_event);
	return 0;
}

/* 에코 서버 스레드 */
static volatile int server_ready;
static volatile unsigned short server_port;

static DWORD __attribute__((ms_abi)) echo_server_thread(void *param)
{
	(void)param;

	SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv == INVALID_SOCKET) return 1;

	/* SO_REUSEADDR: TIME_WAIT 포트 재사용 */
	int opt = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

	SOCKADDR_IN addr;
	my_memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(19999);

	if (bind(srv, (void *)&addr, sizeof(addr)) != 0) {
		closesocket(srv);
		return 1;
	}

	if (listen(srv, 1) != 0) {
		closesocket(srv);
		return 2;
	}

	server_port = 19999;
	server_ready = 1;

	SOCKET cli = accept(srv, NULL, NULL);
	if (cli != INVALID_SOCKET) {
		char buf[64];
		int n = recv(cli, buf, sizeof(buf), 0);
		if (n > 0)
			send(cli, buf, n, 0);
		closesocket(cli);
	}
	closesocket(srv);
	return 0;
}

/* === 메인 === */

void __attribute__((ms_abi)) _start(void)
{
	hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	print("\n=== App Integration Test (Class 55) ===\n\n");
	pass_count = 0;
	fail_count = 0;

	GUID iid_zero;
	my_memset(&iid_zero, 0, sizeof(iid_zero));

	/* ============================================================
	 * 시나리오 1: 멀티스레드 + 시간 API
	 * ============================================================ */
	print("--- Scenario 1: Multithreaded + Time ---\n");

	/* [1] 워커 스레드 + 이벤트 통신 */
	worker_done = 0;
	worker_tick = 0;
	worker_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	HANDLE hThread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);

	if (hThread && worker_event) {
		WaitForSingleObject(worker_event, 5000);
		WaitForSingleObject(hThread, 5000);
		if (worker_done && worker_tick > 0)
			test_ok(1, "Worker thread + Event + GetTickCount");
		else
			test_fail(1, "Worker thread + Event + GetTickCount");
		CloseHandle(hThread);
		CloseHandle(worker_event);
	} else {
		test_fail(1, "Worker thread + Event + GetTickCount");
	}

	/* [2] Sleep + 시간 측정 */
	{
		DWORD t0 = GetTickCount();
		Sleep(50);
		DWORD t1 = GetTickCount();
		DWORD diff = t1 - t0;
		if (diff >= 40)
			test_ok(2, "Sleep(50) + GetTickCount delta");
		else
			test_fail(2, "Sleep(50) + GetTickCount delta");
	}

	/* [3] GetVersionExA + GetSystemInfo */
	{
		OSVERSIONINFOA ver;
		my_memset(&ver, 0, sizeof(ver));
		ver.dwOSVersionInfoSize = sizeof(ver);
		GetVersionExA(&ver);

		SYSTEM_INFO si;
		my_memset(&si, 0, sizeof(si));
		GetSystemInfo(&si);

		if (ver.dwMajorVersion == 10 && si.dwNumberOfProcessors >= 1)
			test_ok(3, "GetVersionExA(10.x) + GetSystemInfo(cpus>=1)");
		else
			test_fail(3, "GetVersionExA(10.x) + GetSystemInfo(cpus>=1)");
	}

	/* ============================================================
	 * 시나리오 2: 네트워크 + 레지스트리
	 * ============================================================ */
	print("\n--- Scenario 2: Network + Registry ---\n");

	/* [4] WSAStartup + TCP 에코 */
	{
		WSADATA wsa;
		my_memset(&wsa, 0, sizeof(wsa));
		int r = WSAStartup(0x0202, &wsa);

		int echo_ok = 0;
		if (r == 0) {
			server_ready = 0;
			HANDLE hSrv = CreateThread(NULL, 0, echo_server_thread, NULL, 0, NULL);

			/* 서버 준비 대기 */
			for (int i = 0; i < 100 && !server_ready; i++)
				Sleep(10);

			if (server_ready) {
				SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
				SOCKADDR_IN sa;
				my_memset(&sa, 0, sizeof(sa));
				sa.sin_family = AF_INET;
				sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				sa.sin_port = htons(server_port);

				if (connect(c, (void *)&sa, sizeof(sa)) == 0) {
					send(c, "PING", 4, 0);
					char buf[16];
					my_memset(buf, 0, sizeof(buf));
					int n = recv(c, buf, sizeof(buf), 0);
					if (n == 4 && buf[0] == 'P' && buf[1] == 'I' &&
					    buf[2] == 'N' && buf[3] == 'G')
						echo_ok = 1;
				}
				closesocket(c);
			}
			if (hSrv) {
				WaitForSingleObject(hSrv, 5000);
				CloseHandle(hSrv);
			}
		}
		if (echo_ok)
			test_ok(4, "TCP echo (threaded server + PING)");
		else
			test_fail(4, "TCP echo (threaded server + PING)");
	}

	/* [5] RegCreateKeyExA + RegSetValueExA + RegQueryValueExA */
	{
		HANDLE hKey = NULL;
		DWORD disp;
		LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
			"SOFTWARE\\CitcAppTest", 0, NULL, 0,
			KEY_ALL_ACCESS, NULL, &hKey, &disp);

		int reg_ok = 0;
		if (r == 0 && hKey) {
			const char *val = "integration_ok";
			RegSetValueExA(hKey, "Status", 0, REG_SZ,
				(const void *)val, (DWORD)(my_strlen(val) + 1));

			char buf[64];
			my_memset(buf, 0, sizeof(buf));
			DWORD sz = sizeof(buf);
			DWORD type = 0;
			r = RegQueryValueExA(hKey, "Status", NULL, &type, buf, &sz);
			if (r == 0 && my_strcmp(buf, "integration_ok") == 0)
				reg_ok = 1;

			RegDeleteValueA(hKey, "Status");
			RegCloseKey(hKey);
			RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\CitcAppTest");
		}
		if (reg_ok)
			test_ok(5, "Registry write + read + cleanup");
		else
			test_fail(5, "Registry write + read + cleanup");
	}

	/* [6] WSACleanup */
	{
		int r = WSACleanup();
		if (r == 0)
			test_ok(6, "WSACleanup");
		else
			test_fail(6, "WSACleanup");
	}

	/* ============================================================
	 * 시나리오 3: COM 초기화
	 * ============================================================ */
	print("\n--- Scenario 3: COM Runtime ---\n");

	/* [7] CoInitializeEx */
	{
		HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		if (SUCCEEDED(hr))
			test_ok(7, "CoInitializeEx(COINIT_MULTITHREADED)");
		else
			test_fail(7, "CoInitializeEx(COINIT_MULTITHREADED)");
	}

	/* [8] CoCreateInstance(CLSID_DirectSound8) */
	{
		GUID clsid_ds8 = { 0x3901CC3F, 0x84B5, 0x4FA4,
			{ 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B } };
		GUID iid_ds8 = { 0xC50A7E93, 0xF395, 0x4834,
			{ 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66 } };
		void *pDS8 = NULL;
		HRESULT hr = CoCreateInstance(&clsid_ds8, NULL, 1, &iid_ds8, &pDS8);
		if (SUCCEEDED(hr) && pDS8)
			test_ok(8, "CoCreateInstance(CLSID_DirectSound8)");
		else
			test_fail(8, "CoCreateInstance(CLSID_DirectSound8)");
	}

	/* [9] CoUninitialize */
	{
		CoUninitialize();
		test_ok(9, "CoUninitialize");
	}

	/* ============================================================
	 * 시나리오 4: 파일시스템
	 * ============================================================ */
	print("\n--- Scenario 4: Filesystem ---\n");

	/* [10] GetTempPathA + CreateDirectoryA + RemoveDirectoryA */
	{
		char tmp[260];
		my_memset(tmp, 0, sizeof(tmp));
		DWORD n = GetTempPathA(sizeof(tmp), tmp);

		int fs_ok = 0;
		if (n > 0) {
			/* tmp에 서브디렉토리 생성 */
			char dir[300];
			my_memset(dir, 0, sizeof(dir));
			/* 수동 문자열 결합 */
			int i = 0;
			while (tmp[i]) { dir[i] = tmp[i]; i++; }
			const char *sub = "citc_app_test";
			int j = 0;
			while (sub[j]) { dir[i++] = sub[j++]; }
			dir[i] = 0;

			if (CreateDirectoryA(dir, NULL)) {
				if (RemoveDirectoryA(dir))
					fs_ok = 1;
			}
		}
		if (fs_ok)
			test_ok(10, "GetTempPath + CreateDir + RemoveDir");
		else
			test_fail(10, "GetTempPath + CreateDir + RemoveDir");
	}

	/* [11] FindFirstFileA */
	{
		/* WIN32_FIND_DATAA: 충분히 큰 버퍼 */
		char findData[592];
		my_memset(findData, 0, sizeof(findData));
		HANDLE hFind = FindFirstFileA("/tmp/*", findData);

		int find_ok = 0;
		if (hFind != (HANDLE)(uintptr_t)-1 && hFind != NULL) {
			/* 최소 1개 엔트리 찾음 */
			find_ok = 1;
			FindClose(hFind);
		}
		if (find_ok)
			test_ok(11, "FindFirstFileA(/tmp/*)");
		else
			test_fail(11, "FindFirstFileA(/tmp/*)");
	}

	/* [12] GetFileAttributes-style check via FindFirstFile */
	{
		char findData[592];
		my_memset(findData, 0, sizeof(findData));
		HANDLE h = FindFirstFileA("/tmp", findData);
		int attr_ok = 0;
		if (h != (HANDLE)(uintptr_t)-1 && h != NULL) {
			/* findData의 첫 DWORD가 파일 속성 */
			DWORD *attrs = (DWORD *)findData;
			if (*attrs != 0) /* 속성이 있음 */
				attr_ok = 1;
			FindClose(h);
		}
		if (attr_ok)
			test_ok(12, "FindFirstFileA(/tmp) attributes");
		else
			test_fail(12, "FindFirstFileA(/tmp) attributes");
	}

	/* ============================================================
	 * 시나리오 5: D3D12
	 * ============================================================ */
	print("\n--- Scenario 5: D3D12 + Fence ---\n");

	/* [13] D3D12CreateDevice */
	void **d3d12dev = NULL;
	{
		HRESULT hr = D3D12CreateDevice(NULL, 0, &iid_zero, (void **)&d3d12dev);
		if (SUCCEEDED(hr) && d3d12dev)
			test_ok(13, "D3D12CreateDevice");
		else
			test_fail(13, "D3D12CreateDevice");
	}

	/* [14] D3D12 CreateCommittedResource + Map/Write */
	if (d3d12dev) {
		ID3D12DeviceVtbl_App *dv = VT(d3d12dev, ID3D12DeviceVtbl_App);

		/* BUFFER resource */
		struct { int Type; UINT a, b, c, d; } bhp;
		my_memset(&bhp, 0, sizeof(bhp));
		bhp.Type = 2; /* UPLOAD */

		struct {
			int Dimension; uint64_t Alignment; uint64_t Width;
			UINT Height; unsigned short DepthOrArraySize, MipLevels;
			int Format; struct { UINT Count; UINT Quality; } SampleDesc;
			int Layout; int Flags;
		} brd;
		my_memset(&brd, 0, sizeof(brd));
		brd.Dimension = 1; /* BUFFER */
		brd.Width = 128;
		brd.Height = 1;
		brd.DepthOrArraySize = 1;
		brd.MipLevels = 1;
		brd.SampleDesc.Count = 1;

		void **bufRes = NULL;
		HRESULT hr = dv->CreateCommittedResource(d3d12dev, &bhp, 0, &brd, 1, NULL,
							  &iid_zero, (void **)&bufRes);

		int buf_ok = 0;
		if (SUCCEEDED(hr) && bufRes) {
			ID3D12ResourceVtbl_App *rv = VT(bufRes, ID3D12ResourceVtbl_App);
			void *pData = NULL;
			rv->Map(bufRes, 0, NULL, &pData);
			if (pData) {
				uint32_t *p = (uint32_t *)pData;
				p[0] = 0x12345678;
				rv->Unmap(bufRes, 0, NULL);

				void *pData2 = NULL;
				rv->Map(bufRes, 0, NULL, &pData2);
				if (pData2) {
					uint32_t *q = (uint32_t *)pData2;
					if (q[0] == 0x12345678)
						buf_ok = 1;
					rv->Unmap(bufRes, 0, NULL);
				}
			}
		}
		if (buf_ok)
			test_ok(14, "D3D12 Buffer Map/Write/Read");
		else
			test_fail(14, "D3D12 Buffer Map/Write/Read");
	} else {
		test_fail(14, "D3D12 Buffer Map/Write/Read (no device)");
	}

	/* [15] D3D12 Fence lifecycle */
	if (d3d12dev) {
		ID3D12DeviceVtbl_App *dv = VT(d3d12dev, ID3D12DeviceVtbl_App);
		void **fence = NULL;
		HRESULT hr = dv->CreateFence(d3d12dev, 0, 0, &iid_zero, (void **)&fence);

		int fence_ok = 0;
		if (SUCCEEDED(hr) && fence) {
			ID3D12FenceVtbl_App *fv = VT(fence, ID3D12FenceVtbl_App);
			uint64_t v0 = fv->GetCompletedValue(fence);
			fv->Signal(fence, 999);
			uint64_t v1 = fv->GetCompletedValue(fence);
			if (v0 == 0 && v1 == 999)
				fence_ok = 1;
		}
		if (fence_ok)
			test_ok(15, "D3D12 Fence(0) -> Signal(999) -> 999");
		else
			test_fail(15, "D3D12 Fence(0) -> Signal(999) -> 999");
	} else {
		test_fail(15, "D3D12 Fence (no device)");
	}

	/* === 결과 === */
	print("\n--- app_test: ");
	print_num((unsigned long long)pass_count);
	print("/");
	print_num((unsigned long long)(pass_count + fail_count));
	print(" PASS ---\n\n");

	ExitProcess(fail_count > 0 ? 1 : 0);
}
