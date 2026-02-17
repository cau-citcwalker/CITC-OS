/*
 * thread_test.c — CITC OS WCL Threading & Synchronization 테스트
 * ================================================================
 *
 * Class 48에서 추가한 스레딩/동기화 API를 테스트합니다:
 *   CreateThread, WaitForSingleObject, WaitForMultipleObjects,
 *   CreateEventA, SetEvent, ResetEvent,
 *   CreateMutexA, ReleaseMutex,
 *   InitializeCriticalSection, EnterCriticalSection,
 *   LeaveCriticalSection, DeleteCriticalSection,
 *   InterlockedIncrement, InterlockedDecrement,
 *   TlsAlloc, TlsSetValue, TlsGetValue, TlsFree,
 *   Sleep, GetExitCodeThread
 *
 * 빌드:
 *   x86_64-w64-mingw32-gcc -nostdlib -o thread_test.exe thread_test.c \
 *       -lkernel32 -Wl,-e,_start
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
#define INFINITE ((DWORD)-1)
#define WAIT_OBJECT_0   0x00000000
#define WAIT_TIMEOUT    0x00000102
#define STILL_ACTIVE    259

/* kernel32.dll 임포트 — 기본 */
__declspec(dllimport) void __stdcall ExitProcess(UINT);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, LPCVOID, DWORD,
					       LPDWORD, LPOVERLAPPED);

/* 스레딩 */
typedef DWORD (__stdcall *LPTHREAD_START_ROUTINE)(void *);
__declspec(dllimport) HANDLE __stdcall CreateThread(
	void *, DWORD, LPTHREAD_START_ROUTINE, void *, DWORD, DWORD *);
__declspec(dllimport) void __stdcall ExitThread(DWORD);
__declspec(dllimport) BOOL __stdcall GetExitCodeThread(HANDLE, DWORD *);

/* 대기 */
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) DWORD __stdcall WaitForMultipleObjects(
	DWORD, const HANDLE *, BOOL, DWORD);

/* Event */
__declspec(dllimport) HANDLE __stdcall CreateEventA(void *, BOOL, BOOL, LPCSTR);
__declspec(dllimport) BOOL __stdcall SetEvent(HANDLE);
__declspec(dllimport) BOOL __stdcall ResetEvent(HANDLE);

/* Mutex */
__declspec(dllimport) HANDLE __stdcall CreateMutexA(void *, BOOL, LPCSTR);
__declspec(dllimport) BOOL __stdcall ReleaseMutex(HANDLE);

/* Critical Section */
typedef struct {
	void *DebugInfo;
	long LockCount;
	long RecursionCount;
	void *OwningThread;
	void *LockSemaphore;
	unsigned long long SpinCount;
} CRITICAL_SECTION;

__declspec(dllimport) void __stdcall InitializeCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void __stdcall EnterCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void __stdcall LeaveCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void __stdcall DeleteCriticalSection(CRITICAL_SECTION *);

/*
 * Interlocked — 컴파일러 인트린식 (DLL 임포트 아님)
 *
 * 실제 Windows에서도 이 함수들은 winnt.h에서 인라인으로 정의.
 * MinGW의 kernel32 import library에는 포함되지 않음.
 * GCC __sync builtins를 직접 사용.
 */
static __inline__ long InterlockedIncrement(volatile long *addend)
{
	return __sync_add_and_fetch(addend, 1);
}

static __inline__ long InterlockedDecrement(volatile long *addend)
{
	return __sync_sub_and_fetch(addend, 1);
}

static __inline__ long InterlockedExchange(volatile long *target, long value)
{
	return __sync_lock_test_and_set(target, value);
}

static __inline__ long InterlockedCompareExchange(volatile long *dest,
						  long exchange, long comparand)
{
	return __sync_val_compare_and_swap(dest, comparand, exchange);
}

/* Sleep */
__declspec(dllimport) void __stdcall Sleep(DWORD);

/* TLS */
__declspec(dllimport) DWORD __stdcall TlsAlloc(void);
__declspec(dllimport) void *__stdcall TlsGetValue(DWORD);
__declspec(dllimport) BOOL __stdcall TlsSetValue(DWORD, void *);
__declspec(dllimport) BOOL __stdcall TlsFree(DWORD);

/* --- 헬퍼 --- */

static HANDLE hStdOut;

static int my_strlen(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	return n;
}

static void print(const char *s)
{
	DWORD written;
	WriteFile(hStdOut, s, my_strlen(s), &written, NULL);
}

static void print_num(int n)
{
	char buf[16];
	int i = 0;
	if (n < 0) { print("-"); n = -n; }
	if (n == 0) { print("0"); return; }
	while (n > 0) {
		buf[i++] = '0' + (n % 10);
		n /= 10;
	}
	char rev[16];
	for (int j = 0; j < i; j++) rev[j] = buf[i - 1 - j];
	rev[i] = '\0';
	print(rev);
}

static int pass_count = 0;
static int fail_count = 0;

static void check(int test_num, const char *name, int condition)
{
	print("  [");
	print_num(test_num);
	print("] ");
	print(name);
	if (condition) {
		print(" ... PASS\n");
		pass_count++;
	} else {
		print(" ... FAIL\n");
		fail_count++;
	}
}

/* ============================================================
 * 스레드 함수들
 * ============================================================ */

static volatile long g_shared_value = 0;

/* [1] 단순 스레드: 값 설정 후 종료 */
static DWORD __stdcall thread_set_value(void *param)
{
	long *p = (long *)param;
	*p = 42;
	return 0;
}

/* [2] 이벤트 대기 후 시그널 */
static volatile long g_event_flag = 0;

static DWORD __stdcall thread_signal_event(void *param)
{
	HANDLE hEvent = (HANDLE)param;
	Sleep(50); /* 50ms 대기 후 시그널 */
	g_event_flag = 1;
	SetEvent(hEvent);
	return 0;
}

/* [4] 뮤텍스 카운터 증가 */
static volatile long g_mutex_counter = 0;
static HANDLE g_test_mutex = NULL;
#define MUTEX_ITERATIONS 1000

static DWORD __stdcall thread_mutex_inc(void *param)
{
	(void)param;
	for (int i = 0; i < MUTEX_ITERATIONS; i++) {
		WaitForSingleObject(g_test_mutex, INFINITE);
		g_mutex_counter++;
		ReleaseMutex(g_test_mutex);
	}
	return 0;
}

/* [5] Critical Section 카운터 증가 */
static volatile long g_cs_counter = 0;
static CRITICAL_SECTION g_test_cs;
#define CS_ITERATIONS 1000

static DWORD __stdcall thread_cs_inc(void *param)
{
	(void)param;
	for (int i = 0; i < CS_ITERATIONS; i++) {
		EnterCriticalSection(&g_test_cs);
		g_cs_counter++;
		LeaveCriticalSection(&g_test_cs);
	}
	return 0;
}

/* [6] WaitForMultipleObjects: 여러 이벤트 시그널 */
static DWORD __stdcall thread_signal_multi(void *param)
{
	HANDLE *events = (HANDLE *)param;
	Sleep(30);
	SetEvent(events[0]);
	Sleep(30);
	SetEvent(events[1]);
	return 0;
}

/* [7] Interlocked 카운터 증가 */
static volatile long g_interlocked_counter = 0;
#define INTERLOCKED_ITERATIONS 10000

static DWORD __stdcall thread_interlocked_inc(void *param)
{
	(void)param;
	for (int i = 0; i < INTERLOCKED_ITERATIONS; i++)
		InterlockedIncrement(&g_interlocked_counter);
	return 0;
}

/* [8] TLS: 스레드별 독립 값 */
static DWORD g_tls_index = 0;
static volatile long g_tls_check = 0;

static DWORD __stdcall thread_tls_test(void *param)
{
	long val = (long)(unsigned long long)param;
	TlsSetValue(g_tls_index, (void *)(unsigned long long)val);
	Sleep(20); /* 다른 스레드가 자기 값을 설정할 시간 */
	void *got = TlsGetValue(g_tls_index);
	if ((long)(unsigned long long)got == val)
		InterlockedIncrement(&g_tls_check);
	return 0;
}

/* [10] 종료 코드 확인 */
static DWORD __stdcall thread_exit_code(void *param)
{
	(void)param;
	return 77;
}

/* ============================================================
 * main
 * ============================================================ */
void __stdcall _start(void)
{
	hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	print("=== thread_test: Win32 Threading & Sync ===\n\n");

	/* [1] CreateThread + WaitForSingleObject */
	{
		volatile long val = 0;
		HANDLE h = CreateThread(NULL, 0, thread_set_value,
					(void *)&val, 0, NULL);
		check(1, "CreateThread", h != NULL);

		if (h) {
			WaitForSingleObject(h, INFINITE);
			check(1, "Thread set value=42", val == 42);
		}
	}

	/* [2] CreateEvent (manual reset) + SetEvent */
	{
		HANDLE hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		check(2, "CreateEvent(manual)", hEvent != NULL);

		if (hEvent) {
			g_event_flag = 0;
			HANDLE ht = CreateThread(NULL, 0,
						 thread_signal_event,
						 hEvent, 0, NULL);
			DWORD ret = WaitForSingleObject(hEvent, 5000);
			check(2, "WaitForSingleObject(event)",
			      ret == WAIT_OBJECT_0 && g_event_flag == 1);

			if (ht) WaitForSingleObject(ht, INFINITE);
		}
	}

	/* [3] CreateEvent (auto reset) */
	{
		HANDLE hEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
		check(3, "CreateEvent(auto)", hEvent != NULL);

		if (hEvent) {
			SetEvent(hEvent);
			/* 첫 Wait는 성공 */
			DWORD r1 = WaitForSingleObject(hEvent, 0);
			/* 두 번째 Wait는 타임아웃 (auto reset) */
			DWORD r2 = WaitForSingleObject(hEvent, 0);
			check(3, "Auto-reset: first=OK, second=TIMEOUT",
			      r1 == WAIT_OBJECT_0 && r2 == WAIT_TIMEOUT);
		}
	}

	/* [4] Mutex + 두 스레드 카운터 */
	{
		g_mutex_counter = 0;
		g_test_mutex = CreateMutexA(NULL, FALSE, NULL);
		check(4, "CreateMutexA", g_test_mutex != NULL);

		if (g_test_mutex) {
			HANDLE t1 = CreateThread(NULL, 0, thread_mutex_inc,
						 NULL, 0, NULL);
			HANDLE t2 = CreateThread(NULL, 0, thread_mutex_inc,
						 NULL, 0, NULL);
			if (t1) WaitForSingleObject(t1, INFINITE);
			if (t2) WaitForSingleObject(t2, INFINITE);

			check(4, "Mutex counter == 2000",
			      g_mutex_counter == 2 * MUTEX_ITERATIONS);
		}
	}

	/* [5] Critical Section + 두 스레드 카운터 */
	{
		g_cs_counter = 0;
		InitializeCriticalSection(&g_test_cs);

		HANDLE t1 = CreateThread(NULL, 0, thread_cs_inc,
					 NULL, 0, NULL);
		HANDLE t2 = CreateThread(NULL, 0, thread_cs_inc,
					 NULL, 0, NULL);
		if (t1) WaitForSingleObject(t1, INFINITE);
		if (t2) WaitForSingleObject(t2, INFINITE);

		check(5, "CriticalSection counter == 2000",
		      g_cs_counter == 2 * CS_ITERATIONS);

		DeleteCriticalSection(&g_test_cs);
	}

	/* [6] WaitForMultipleObjects (WaitAll) */
	{
		HANDLE events[2];
		events[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
		events[1] = CreateEventA(NULL, TRUE, FALSE, NULL);

		check(6, "Create 2 events",
		      events[0] != NULL && events[1] != NULL);

		HANDLE ht = CreateThread(NULL, 0, thread_signal_multi,
					 events, 0, NULL);

		DWORD ret = WaitForMultipleObjects(2, events, TRUE, 5000);
		check(6, "WaitForMultipleObjects(WaitAll)",
		      ret == WAIT_OBJECT_0);

		if (ht) WaitForSingleObject(ht, INFINITE);
	}

	/* [7] InterlockedIncrement */
	{
		g_interlocked_counter = 0;

		HANDLE t1 = CreateThread(NULL, 0, thread_interlocked_inc,
					 NULL, 0, NULL);
		HANDLE t2 = CreateThread(NULL, 0, thread_interlocked_inc,
					 NULL, 0, NULL);
		if (t1) WaitForSingleObject(t1, INFINITE);
		if (t2) WaitForSingleObject(t2, INFINITE);

		check(7, "Interlocked counter == 20000",
		      g_interlocked_counter == 2 * INTERLOCKED_ITERATIONS);
	}

	/* [8] TLS */
	{
		g_tls_index = TlsAlloc();
		check(8, "TlsAlloc", g_tls_index != (DWORD)-1);

		if (g_tls_index != (DWORD)-1) {
			g_tls_check = 0;

			HANDLE t1 = CreateThread(NULL, 0, thread_tls_test,
						 (void *)100, 0, NULL);
			HANDLE t2 = CreateThread(NULL, 0, thread_tls_test,
						 (void *)200, 0, NULL);
			if (t1) WaitForSingleObject(t1, INFINITE);
			if (t2) WaitForSingleObject(t2, INFINITE);

			check(8, "TLS per-thread values",
			      g_tls_check == 2);

			TlsFree(g_tls_index);
		}
	}

	/* [9] Sleep */
	{
		/* 간단히 Sleep이 크래시하지 않는지 확인 */
		Sleep(50);
		check(9, "Sleep(50) no crash", 1);
	}

	/* [10] GetExitCodeThread */
	{
		HANDLE h = CreateThread(NULL, 0, thread_exit_code,
					NULL, 0, NULL);
		check(10, "CreateThread for exit code", h != NULL);

		if (h) {
			WaitForSingleObject(h, INFINITE);
			DWORD code = 0;
			BOOL ok = GetExitCodeThread(h, &code);
			check(10, "ExitCode == 77",
			      ok && code == 77);
		}
	}

	/* 결과 요약 */
	print("\n=== Results: ");
	print_num(pass_count);
	print(" passed, ");
	print_num(fail_count);
	print(" failed ===\n");

	if (fail_count == 0)
		print("ALL PASS\n");

	ExitProcess(fail_count);
}
