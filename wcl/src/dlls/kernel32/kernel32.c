/*
 * kernel32.c — CITC OS kernel32.dll 구현
 * ========================================
 *
 * Windows의 kernel32.dll은 가장 핵심적인 Win32 API DLL입니다.
 * 파일 I/O, 프로세스 관리, 메모리 관리, 동기화 등
 * 거의 모든 Windows 프로그램이 이 DLL을 사용합니다.
 *
 * Class 23 리팩터:
 *   기존에는 kernel32이 직접 POSIX를 호출했지만,
 *   이제는 NT 네이티브 API(ntdll)를 호출합니다.
 *
 *   Win32 앱 → kernel32.dll → ntdll.dll → POSIX
 *
 *   이것은 실제 Windows의 아키텍처와 동일합니다:
 *     CreateFileA → NtCreateFile → syscall → 커널
 *
 * 각 Win32 함수의 구현 패턴:
 *   1. __attribute__((ms_abi)) — Windows 호출 규약
 *   2. Windows 인수 정리/검증
 *   3. Nt* 함수 호출
 *   4. NTSTATUS → Win32 에러 변환 + SetLastError
 *   5. Win32 형식의 반환값
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include "../../../include/win32.h"
#include "../../ntemu/ntdll.h"
#include "../../ntemu/object_manager.h"
#include "../../ntemu/registry.h"
#include "kernel32.h"

/* ============================================================
 * Win32 에러 코드 시스템
 * ============================================================
 *
 * __thread 변수로 TEB의 LastErrorValue를 흉내냄.
 */
static __thread uint32_t last_error = 0;

/* ============================================================
 * kernel32 초기화
 * ============================================================ */
void kernel32_init(void)
{
	/* NT 서브시스템 초기화 (Object Manager 포함) */
	ntdll_init();
	/* 레지스트리 서브시스템 초기화 */
	reg_init();
}

/* ============================================================
 * Win32 API 함수 구현
 * ============================================================
 *
 * 모든 함수에 __attribute__((ms_abi))가 필요합니다.
 *
 * 이유: Windows .exe는 Microsoft x64 호출 규약으로 컴파일됨.
 *   .exe 코드: call [IAT]  → 인수가 RCX, RDX, R8, R9에 있음
 *   ms_abi:   GCC가 RCX에서 인수를 받아 처리
 */

/* --- GetLastError / SetLastError --- */

__attribute__((ms_abi))
static uint32_t k32_GetLastError(void)
{
	return last_error;
}

__attribute__((ms_abi))
static void k32_SetLastError(uint32_t code)
{
	last_error = code;
}

/* --- GetStdHandle --- */

/*
 * GetStdHandle — 표준 입출력 핸들 반환
 *
 * STD_INPUT_HANDLE  = (DWORD)-10 → 인덱스 0 → HANDLE 0x100
 * STD_OUTPUT_HANDLE = (DWORD)-11 → 인덱스 1 → HANDLE 0x101
 * STD_ERROR_HANDLE  = (DWORD)-12 → 인덱스 2 → HANDLE 0x102
 */
__attribute__((ms_abi))
static HANDLE k32_GetStdHandle(uint32_t std_handle)
{
	int idx;

	switch (std_handle) {
	case (uint32_t)-10: idx = 0; break;
	case (uint32_t)-11: idx = 1; break;
	case (uint32_t)-12: idx = 2; break;
	default:
		last_error = ERROR_INVALID_HANDLE;
		return INVALID_HANDLE_VALUE;
	}

	return (HANDLE)(uintptr_t)(idx + OB_HANDLE_OFFSET);
}

/* --- ExitProcess --- */

__attribute__((ms_abi))
static void k32_ExitProcess(uint32_t exit_code)
{
	printf("\n>>> Process exit (code: %u) <<<\n", exit_code);
	_exit((int)exit_code);
}

/* --- CreateFileA --- */

/*
 * CreateFileA — 파일 열기/생성 (ANSI 버전)
 *
 * 이제 ntdll의 nt_create_file()을 호출하는 얇은 래퍼.
 * NTSTATUS를 Win32 에러 코드로 변환하여 SetLastError.
 */
__attribute__((ms_abi))
static HANDLE k32_CreateFileA(const char *filename,
			      uint32_t desired_access,
			      uint32_t share_mode,
			      void *security_attributes,
			      uint32_t creation_disposition,
			      uint32_t flags_and_attributes,
			      HANDLE template_file)
{
	(void)share_mode;
	(void)security_attributes;
	(void)flags_and_attributes;
	(void)template_file;

	if (!filename) {
		last_error = ERROR_PATH_NOT_FOUND;
		return INVALID_HANDLE_VALUE;
	}

	HANDLE h;
	NTSTATUS status = nt_create_file(&h, desired_access,
					 filename, creation_disposition);

	if (!NT_SUCCESS(status)) {
		last_error = nt_status_to_win32(status);
		return INVALID_HANDLE_VALUE;
	}

	return h;
}

/* --- WriteFile --- */

__attribute__((ms_abi))
static int32_t k32_WriteFile(HANDLE handle, const void *buf,
			     uint32_t bytes_to_write,
			     uint32_t *bytes_written,
			     void *overlapped)
{
	(void)overlapped;

	NTSTATUS status = nt_write_file(handle, buf,
					bytes_to_write, bytes_written);

	if (!NT_SUCCESS(status)) {
		last_error = nt_status_to_win32(status);
		return FALSE;
	}
	return TRUE;
}

/* --- ReadFile --- */

__attribute__((ms_abi))
static int32_t k32_ReadFile(HANDLE handle, void *buf,
			    uint32_t bytes_to_read,
			    uint32_t *bytes_read,
			    void *overlapped)
{
	(void)overlapped;

	NTSTATUS status = nt_read_file(handle, buf,
				       bytes_to_read, bytes_read);

	if (!NT_SUCCESS(status)) {
		last_error = nt_status_to_win32(status);
		return FALSE;
	}
	return TRUE;
}

/* --- CloseHandle --- */

__attribute__((ms_abi))
static int32_t k32_CloseHandle(HANDLE handle)
{
	NTSTATUS status = nt_close(handle);

	if (!NT_SUCCESS(status)) {
		last_error = nt_status_to_win32(status);
		return FALSE;
	}
	return TRUE;
}

/* --- GetFileSize --- */

__attribute__((ms_abi))
static uint32_t k32_GetFileSize(HANDLE handle, uint32_t *size_high)
{
	uint64_t size;
	NTSTATUS status = nt_query_file_size(handle, &size);

	if (!NT_SUCCESS(status)) {
		last_error = nt_status_to_win32(status);
		return INVALID_FILE_SIZE;
	}

	if (size_high)
		*size_high = (uint32_t)(size >> 32);

	return (uint32_t)(size & 0xFFFFFFFF);
}

/* --- SetFilePointer --- */

__attribute__((ms_abi))
static uint32_t k32_SetFilePointer(HANDLE handle, int32_t distance,
				   int32_t *distance_high,
				   uint32_t move_method)
{
	(void)distance_high;

	int whence;

	switch (move_method) {
	case FILE_BEGIN:   whence = 0; break; /* SEEK_SET */
	case FILE_CURRENT: whence = 1; break; /* SEEK_CUR */
	case FILE_END:     whence = 2; break; /* SEEK_END */
	default:
		last_error = ERROR_GEN_FAILURE;
		return INVALID_SET_FILE_POINTER;
	}

	/*
	 * nt_set_file_position은 결과 위치를 반환하지 않으므로,
	 * 여기서는 직접 ob_ref_handle로 fd를 얻어 lseek 호출.
	 * (NT 계층에서는 위치를 반환하는 별도 API가 있지만 단순화)
	 */
	struct ob_entry *entry = ob_ref_handle(handle);

	if (!entry) {
		last_error = ERROR_INVALID_HANDLE;
		return INVALID_SET_FILE_POINTER;
	}

	off_t result = lseek(entry->fd, distance, whence);

	if (result < 0) {
		last_error = ERROR_GEN_FAILURE;
		return INVALID_SET_FILE_POINTER;
	}

	return (uint32_t)result;
}

/* --- DeleteFileA --- */

__attribute__((ms_abi))
static int32_t k32_DeleteFileA(const char *filename)
{
	if (!filename) {
		last_error = ERROR_PATH_NOT_FOUND;
		return FALSE;
	}

	NTSTATUS status = nt_delete_file(filename);

	if (!NT_SUCCESS(status)) {
		last_error = nt_status_to_win32(status);
		return FALSE;
	}
	return TRUE;
}

/* ============================================================
 * 메모리 관리 API (Class 25)
 * ============================================================
 *
 * VirtualAlloc/Free → mmap/munmap
 * HeapAlloc/Free    → malloc/free (Heap은 단순 래퍼)
 *
 * Windows 메모리 모델:
 *   VirtualAlloc: 페이지 단위 할당 (커널 직접)
 *   HeapAlloc:    바이트 단위 할당 (유저 모드 힙 관리자)
 *   malloc:       C 런타임 (HeapAlloc의 래퍼)
 *
 * Linux 대응:
 *   VirtualAlloc → mmap (MAP_ANONYMOUS)
 *   HeapAlloc    → malloc (glibc 힙)
 */

/* Windows 보호 플래그 → POSIX mmap prot */
static int page_prot_to_mmap(uint32_t protect)
{
	switch (protect) {
	case PAGE_NOACCESS:           return PROT_NONE;
	case PAGE_READONLY:           return PROT_READ;
	case PAGE_READWRITE:          return PROT_READ | PROT_WRITE;
	case PAGE_EXECUTE:            return PROT_EXEC;
	case PAGE_EXECUTE_READ:       return PROT_READ | PROT_EXEC;
	case PAGE_EXECUTE_READWRITE:  return PROT_READ | PROT_WRITE | PROT_EXEC;
	default:                      return PROT_READ | PROT_WRITE;
	}
}

__attribute__((ms_abi))
static void *k32_VirtualAlloc(void *address, size_t size,
			      uint32_t alloc_type, uint32_t protect)
{
	(void)alloc_type; /* MEM_COMMIT/RESERVE 구분 생략 */

	int prot = page_prot_to_mmap(protect);
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	if (address)
		flags |= MAP_FIXED;

	void *result = mmap(address, size, prot, flags, -1, 0);

	if (result == MAP_FAILED) {
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	return result;
}

__attribute__((ms_abi))
static int32_t k32_VirtualFree(void *address, size_t size,
			       uint32_t free_type)
{
	if (free_type == MEM_RELEASE)
		size = 0; /* MEM_RELEASE: size는 0이어야 함 */

	/* munmap은 정확한 크기가 필요 — 페이지 단위로 올림 */
	if (size == 0)
		size = 4096; /* 최소 1페이지 */

	if (munmap(address, size) < 0) {
		last_error = ERROR_GEN_FAILURE;
		return FALSE;
	}

	return TRUE;
}

/* 프로세스 힙 (단순히 malloc/free 래핑) */
static void *process_heap = (void *)(uintptr_t)0xDEAD0001;

__attribute__((ms_abi))
static HANDLE k32_GetProcessHeap(void)
{
	return process_heap;
}

__attribute__((ms_abi))
static void *k32_HeapAlloc(HANDLE heap, uint32_t flags, size_t size)
{
	(void)heap;

	void *ptr = malloc(size);

	if (!ptr) {
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	if (flags & HEAP_ZERO_MEMORY)
		memset(ptr, 0, size);

	return ptr;
}

__attribute__((ms_abi))
static int32_t k32_HeapFree(HANDLE heap, uint32_t flags, void *ptr)
{
	(void)heap;
	(void)flags;

	free(ptr);
	return TRUE;
}

/* ============================================================
 * 프로세스/스레드 정보 API (Class 25)
 * ============================================================ */

__attribute__((ms_abi))
static uint32_t k32_GetCurrentProcessId(void)
{
	return (uint32_t)getpid();
}

__attribute__((ms_abi))
static uint32_t k32_GetCurrentThreadId(void)
{
	return (uint32_t)pthread_self();
}

__attribute__((ms_abi))
static HANDLE k32_GetCurrentProcess(void)
{
	/* 현재 프로세스 가상 핸들 = (HANDLE)-1 */
	return (HANDLE)(uintptr_t)-1;
}

/* ============================================================
 * 환경변수 API (Class 25)
 * ============================================================ */

__attribute__((ms_abi))
static uint32_t k32_GetEnvironmentVariableA(const char *name,
					    char *buffer,
					    uint32_t size)
{
	if (!name) {
		last_error = ERROR_INVALID_PARAMETER;
		return 0;
	}

	const char *value = getenv(name);

	if (!value) {
		last_error = ERROR_FILE_NOT_FOUND; /* ERROR_ENVVAR_NOT_FOUND */
		return 0;
	}

	uint32_t len = (uint32_t)strlen(value);

	if (len + 1 > size) {
		/* 버퍼 부족: 필요한 크기(널 포함) 반환 */
		last_error = ERROR_GEN_FAILURE;
		return len + 1;
	}

	memcpy(buffer, value, len + 1);
	return len;
}

__attribute__((ms_abi))
static int32_t k32_SetEnvironmentVariableA(const char *name,
					   const char *value)
{
	if (!name) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	int ret;

	if (value)
		ret = setenv(name, value, 1);
	else
		ret = unsetenv(name);

	if (ret < 0) {
		last_error = ERROR_GEN_FAILURE;
		return FALSE;
	}

	return TRUE;
}

/* ============================================================
 * 명령줄 API (Class 25)
 * ============================================================ */

/* 전역 명령줄 (citcrun이 설정) */
static char saved_cmdline[1024] = "program.exe";

void kernel32_set_cmdline(const char *cmdline)
{
	if (cmdline)
		snprintf(saved_cmdline, sizeof(saved_cmdline), "%s", cmdline);
}

__attribute__((ms_abi))
static const char *k32_GetCommandLineA(void)
{
	return saved_cmdline;
}

/* ============================================================
 * 모듈 API (Class 25)
 * ============================================================ */

__attribute__((ms_abi))
static HANDLE k32_GetModuleHandleA(const char *module_name)
{
	if (!module_name) {
		/* NULL → 현재 실행 중인 모듈의 베이스 주소 */
		/* 실제로는 PE 이미지 베이스가 필요하지만 스텁으로 비-NULL 반환 */
		return (HANDLE)(uintptr_t)0x00400000;
	}

	/* 다른 모듈 조회: 미구현 */
	last_error = ERROR_FILE_NOT_FOUND;
	return NULL;
}

__attribute__((ms_abi))
static uint32_t k32_GetModuleFileNameA(HANDLE module, char *filename,
				       uint32_t size)
{
	(void)module;

	/* /proc/self/exe 읽기 */
	ssize_t len = readlink("/proc/self/exe", filename, size - 1);

	if (len < 0) {
		last_error = ERROR_GEN_FAILURE;
		return 0;
	}

	filename[len] = '\0';
	return (uint32_t)len;
}

/* ============================================================
 * 스레딩 API (Class 48)
 * ============================================================
 *
 * Win32 스레드 → pthread 래퍼.
 * ms_abi 콜백을 sysv_abi에서 호출하기 위한 wrapper 사용.
 */

/* --- 내부 구조체 --- */

struct win32_thread {
	pthread_t       pt;
	DWORD           exit_code;
	int             finished;
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	LPTHREAD_START_ROUTINE start_addr;
	void           *param;
};

struct win32_event {
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	int             signaled;
	int             manual_reset;
};

struct win32_mutex {
	pthread_mutex_t pm;
};

/* --- 스레드 wrapper --- */

static void *thread_wrapper(void *arg)
{
	struct win32_thread *thr = arg;

	/* ms_abi 콜백 호출 */
	DWORD result = thr->start_addr(thr->param);

	pthread_mutex_lock(&thr->lock);
	thr->exit_code = result;
	thr->finished = 1;
	pthread_cond_broadcast(&thr->cond);
	pthread_mutex_unlock(&thr->lock);

	return NULL;
}

__attribute__((ms_abi))
static HANDLE k32_CreateThread(void *security, size_t stack_size,
			       LPTHREAD_START_ROUTINE start_addr,
			       void *param, DWORD flags, DWORD *thread_id)
{
	(void)security;
	(void)stack_size;
	(void)flags;

	struct win32_thread *thr = calloc(1, sizeof(*thr));

	if (!thr) {
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	pthread_mutex_init(&thr->lock, NULL);
	pthread_cond_init(&thr->cond, NULL);
	thr->start_addr = start_addr;
	thr->param = param;

	HANDLE h = ob_create_handle_ex(OB_THREAD, thr);

	if (h == INVALID_HANDLE_VALUE) {
		free(thr);
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	if (pthread_create(&thr->pt, NULL, thread_wrapper, thr) != 0) {
		ob_close_handle(h);
		free(thr);
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	if (thread_id)
		*thread_id = (DWORD)(uintptr_t)thr->pt;

	return h;
}

__attribute__((ms_abi))
static void k32_ExitThread(DWORD exit_code)
{
	/*
	 * ExitThread은 현재 스레드를 종료.
	 * wrapper에서 exit_code를 설정하므로 여기서는 pthread_exit만.
	 * 주의: 이 함수에서 스레드 구조체를 찾기 어려우므로
	 * exit_code 설정은 wrapper에 의존.
	 */
	(void)exit_code;
	pthread_exit(NULL);
}

__attribute__((ms_abi))
static BOOL k32_GetExitCodeThread(HANDLE hThread, DWORD *lpExitCode)
{
	struct ob_entry *e = ob_ref_handle(hThread);

	if (!e || e->type != OB_THREAD || !lpExitCode) {
		last_error = ERROR_INVALID_HANDLE;
		return FALSE;
	}

	struct win32_thread *thr = e->extra;

	pthread_mutex_lock(&thr->lock);

	if (thr->finished)
		*lpExitCode = thr->exit_code;
	else
		*lpExitCode = 259; /* STILL_ACTIVE */

	pthread_mutex_unlock(&thr->lock);
	return TRUE;
}

/* --- WaitForSingleObject / WaitForMultipleObjects --- */

/*
 * 내부: 타입별 대기. ms==INFINITE → 무한 대기.
 * 반환: WAIT_OBJECT_0 성공, WAIT_TIMEOUT 타임아웃.
 */
static DWORD wait_on_thread(struct win32_thread *thr, DWORD ms)
{
	pthread_mutex_lock(&thr->lock);

	if (thr->finished) {
		pthread_mutex_unlock(&thr->lock);
		return WAIT_OBJECT_0;
	}

	if (ms == INFINITE) {
		while (!thr->finished)
			pthread_cond_wait(&thr->cond, &thr->lock);
		pthread_mutex_unlock(&thr->lock);
		return WAIT_OBJECT_0;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	while (!thr->finished) {
		if (pthread_cond_timedwait(&thr->cond, &thr->lock, &ts) == ETIMEDOUT)
			break;
	}

	DWORD ret = thr->finished ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
	pthread_mutex_unlock(&thr->lock);
	return ret;
}

static DWORD wait_on_event(struct win32_event *ev, DWORD ms)
{
	pthread_mutex_lock(&ev->lock);

	if (ev->signaled) {
		if (!ev->manual_reset)
			ev->signaled = 0;
		pthread_mutex_unlock(&ev->lock);
		return WAIT_OBJECT_0;
	}

	if (ms == 0) {
		pthread_mutex_unlock(&ev->lock);
		return WAIT_TIMEOUT;
	}

	if (ms == INFINITE) {
		while (!ev->signaled)
			pthread_cond_wait(&ev->cond, &ev->lock);
	} else {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += ms / 1000;
		ts.tv_nsec += (ms % 1000) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		while (!ev->signaled) {
			if (pthread_cond_timedwait(&ev->cond, &ev->lock, &ts) == ETIMEDOUT)
				break;
		}
	}

	DWORD ret = ev->signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
	if (ret == WAIT_OBJECT_0 && !ev->manual_reset)
		ev->signaled = 0;
	pthread_mutex_unlock(&ev->lock);
	return ret;
}

static DWORD wait_on_mutex(struct win32_mutex *m, DWORD ms)
{
	if (ms == INFINITE) {
		pthread_mutex_lock(&m->pm);
		return WAIT_OBJECT_0;
	}

	if (ms == 0) {
		if (pthread_mutex_trylock(&m->pm) == 0)
			return WAIT_OBJECT_0;
		return WAIT_TIMEOUT;
	}

	/* timed mutex lock: 폴링 방식 (단순화) */
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	while (1) {
		if (pthread_mutex_trylock(&m->pm) == 0)
			return WAIT_OBJECT_0;

		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		if (now.tv_sec > ts.tv_sec ||
		    (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))
			return WAIT_TIMEOUT;

		usleep(1000); /* 1ms sleep */
	}
}

__attribute__((ms_abi))
static DWORD k32_WaitForSingleObject(HANDLE hHandle, DWORD ms)
{
	struct ob_entry *e = ob_ref_handle(hHandle);

	if (!e) {
		last_error = ERROR_INVALID_HANDLE;
		return WAIT_FAILED;
	}

	switch (e->type) {
	case OB_THREAD:
		return wait_on_thread(e->extra, ms);
	case OB_EVENT:
		return wait_on_event(e->extra, ms);
	case OB_MUTEX:
		return wait_on_mutex(e->extra, ms);
	default:
		last_error = ERROR_INVALID_HANDLE;
		return WAIT_FAILED;
	}
}

__attribute__((ms_abi))
static DWORD k32_WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles,
					 BOOL bWaitAll, DWORD ms)
{
	if (!lpHandles || nCount == 0 || nCount > 64) {
		last_error = ERROR_INVALID_PARAMETER;
		return WAIT_FAILED;
	}

	if (bWaitAll) {
		/* WaitAll: 모든 핸들이 시그널 될 때까지 순차 대기 */
		for (DWORD i = 0; i < nCount; i++) {
			DWORD ret = k32_WaitForSingleObject(lpHandles[i], ms);
			if (ret != WAIT_OBJECT_0)
				return ret;
		}
		return WAIT_OBJECT_0;
	}

	/* WaitAny: 폴링으로 아무 하나 시그널 체크 */
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += ms / 1000;
	deadline.tv_nsec += (ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	while (1) {
		for (DWORD i = 0; i < nCount; i++) {
			DWORD ret = k32_WaitForSingleObject(lpHandles[i], 0);
			if (ret == WAIT_OBJECT_0)
				return WAIT_OBJECT_0 + i;
		}

		if (ms != INFINITE) {
			struct timespec now;
			clock_gettime(CLOCK_REALTIME, &now);
			if (now.tv_sec > deadline.tv_sec ||
			    (now.tv_sec == deadline.tv_sec &&
			     now.tv_nsec >= deadline.tv_nsec))
				return WAIT_TIMEOUT;
		}

		usleep(1000);
	}
}

/* --- Event --- */

__attribute__((ms_abi))
static HANDLE k32_CreateEventA(void *security, BOOL manual_reset,
			       BOOL initial_state, const char *name)
{
	(void)security;
	(void)name;

	struct win32_event *ev = calloc(1, sizeof(*ev));

	if (!ev) {
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	pthread_mutex_init(&ev->lock, NULL);
	pthread_cond_init(&ev->cond, NULL);
	ev->signaled = initial_state ? 1 : 0;
	ev->manual_reset = manual_reset ? 1 : 0;

	HANDLE h = ob_create_handle_ex(OB_EVENT, ev);

	if (h == INVALID_HANDLE_VALUE) {
		free(ev);
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	return h;
}

__attribute__((ms_abi))
static BOOL k32_SetEvent(HANDLE hEvent)
{
	struct ob_entry *e = ob_ref_handle(hEvent);

	if (!e || e->type != OB_EVENT) {
		last_error = ERROR_INVALID_HANDLE;
		return FALSE;
	}

	struct win32_event *ev = e->extra;

	pthread_mutex_lock(&ev->lock);
	ev->signaled = 1;
	if (ev->manual_reset)
		pthread_cond_broadcast(&ev->cond);
	else
		pthread_cond_signal(&ev->cond);
	pthread_mutex_unlock(&ev->lock);

	return TRUE;
}

__attribute__((ms_abi))
static BOOL k32_ResetEvent(HANDLE hEvent)
{
	struct ob_entry *e = ob_ref_handle(hEvent);

	if (!e || e->type != OB_EVENT) {
		last_error = ERROR_INVALID_HANDLE;
		return FALSE;
	}

	struct win32_event *ev = e->extra;

	pthread_mutex_lock(&ev->lock);
	ev->signaled = 0;
	pthread_mutex_unlock(&ev->lock);

	return TRUE;
}

/* --- Mutex --- */

__attribute__((ms_abi))
static HANDLE k32_CreateMutexA(void *security, BOOL initial_owner,
			       const char *name)
{
	(void)security;
	(void)name;

	struct win32_mutex *m = calloc(1, sizeof(*m));

	if (!m) {
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m->pm, &attr);
	pthread_mutexattr_destroy(&attr);

	if (initial_owner)
		pthread_mutex_lock(&m->pm);

	HANDLE h = ob_create_handle_ex(OB_MUTEX, m);

	if (h == INVALID_HANDLE_VALUE) {
		pthread_mutex_destroy(&m->pm);
		free(m);
		last_error = ERROR_GEN_FAILURE;
		return NULL;
	}

	return h;
}

__attribute__((ms_abi))
static BOOL k32_ReleaseMutex(HANDLE hMutex)
{
	struct ob_entry *e = ob_ref_handle(hMutex);

	if (!e || e->type != OB_MUTEX) {
		last_error = ERROR_INVALID_HANDLE;
		return FALSE;
	}

	struct win32_mutex *m = e->extra;
	pthread_mutex_unlock(&m->pm);
	return TRUE;
}

/* --- Critical Section --- */

__attribute__((ms_abi))
static void k32_InitializeCriticalSection(CRITICAL_SECTION *cs)
{
	if (!cs) return;

	pthread_mutex_t *pm = calloc(1, sizeof(pthread_mutex_t));

	if (!pm) return;

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(pm, &attr);
	pthread_mutexattr_destroy(&attr);

	memset(cs, 0, sizeof(*cs));
	cs->LockSemaphore = pm;
}

__attribute__((ms_abi))
static void k32_EnterCriticalSection(CRITICAL_SECTION *cs)
{
	if (!cs || !cs->LockSemaphore) return;
	pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
}

__attribute__((ms_abi))
static void k32_LeaveCriticalSection(CRITICAL_SECTION *cs)
{
	if (!cs || !cs->LockSemaphore) return;
	pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

__attribute__((ms_abi))
static void k32_DeleteCriticalSection(CRITICAL_SECTION *cs)
{
	if (!cs || !cs->LockSemaphore) return;

	pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
	free(cs->LockSemaphore);
	cs->LockSemaphore = NULL;
}

/* --- Interlocked --- */

__attribute__((ms_abi))
static long k32_InterlockedIncrement(volatile long *addend)
{
	return __sync_add_and_fetch(addend, 1);
}

__attribute__((ms_abi))
static long k32_InterlockedDecrement(volatile long *addend)
{
	return __sync_sub_and_fetch(addend, 1);
}

__attribute__((ms_abi))
static long k32_InterlockedExchange(volatile long *target, long value)
{
	return __sync_lock_test_and_set(target, value);
}

__attribute__((ms_abi))
static long k32_InterlockedCompareExchange(volatile long *dest,
					   long exchange, long comparand)
{
	return __sync_val_compare_and_swap(dest, comparand, exchange);
}

/* --- Sleep --- */

__attribute__((ms_abi))
static void k32_Sleep(DWORD ms)
{
	if (ms == 0) {
		sched_yield();
		return;
	}

	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* --- TLS (Thread-Local Storage) --- */

#define TLS_MAX_SLOTS 64

static pthread_key_t tls_keys[TLS_MAX_SLOTS];
static int tls_used[TLS_MAX_SLOTS];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((ms_abi))
static DWORD k32_TlsAlloc(void)
{
	pthread_mutex_lock(&tls_lock);

	for (int i = 0; i < TLS_MAX_SLOTS; i++) {
		if (!tls_used[i]) {
			if (pthread_key_create(&tls_keys[i], NULL) == 0) {
				tls_used[i] = 1;
				pthread_mutex_unlock(&tls_lock);
				return (DWORD)i;
			}
		}
	}

	pthread_mutex_unlock(&tls_lock);
	last_error = ERROR_GEN_FAILURE;
	return (DWORD)-1; /* TLS_OUT_OF_INDEXES */
}

__attribute__((ms_abi))
static void *k32_TlsGetValue(DWORD index)
{
	if (index >= TLS_MAX_SLOTS || !tls_used[index]) {
		last_error = ERROR_INVALID_PARAMETER;
		return NULL;
	}

	last_error = 0;
	return pthread_getspecific(tls_keys[index]);
}

__attribute__((ms_abi))
static BOOL k32_TlsSetValue(DWORD index, void *value)
{
	if (index >= TLS_MAX_SLOTS || !tls_used[index]) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	pthread_setspecific(tls_keys[index], value);
	return TRUE;
}

__attribute__((ms_abi))
static BOOL k32_TlsFree(DWORD index)
{
	if (index >= TLS_MAX_SLOTS) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	pthread_mutex_lock(&tls_lock);

	if (tls_used[index]) {
		pthread_key_delete(tls_keys[index]);
		tls_used[index] = 0;
	}

	pthread_mutex_unlock(&tls_lock);
	return TRUE;
}

/* ============================================================
 * 시간 API (Class 52)
 * ============================================================
 *
 * 게임 루프와 성능 측정에 필수적인 API.
 *
 *   GetTickCount     → clock_gettime(MONOTONIC) → ms
 *   QPC/QPF          → clock_gettime(MONOTONIC) → ns, freq=1GHz
 *   GetSystemTimeAsFileTime → REALTIME → FILETIME
 */

__attribute__((ms_abi))
static DWORD k32_GetTickCount(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

__attribute__((ms_abi))
static uint64_t k32_GetTickCount64(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

__attribute__((ms_abi))
static BOOL k32_QueryPerformanceCounter(LARGE_INTEGER *lpPC)
{
	if (!lpPC)
		return FALSE;

	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	lpPC->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	return TRUE;
}

__attribute__((ms_abi))
static BOOL k32_QueryPerformanceFrequency(LARGE_INTEGER *lpFreq)
{
	if (!lpFreq)
		return FALSE;

	lpFreq->QuadPart = 1000000000LL; /* 1 GHz (ns precision) */
	return TRUE;
}

/*
 * FILETIME: 100ns intervals since 1601-01-01.
 * Unix epoch (1970-01-01) = 11644473600 seconds after FILETIME epoch.
 */
#define FILETIME_UNIX_DIFF 116444736000000000ULL

__attribute__((ms_abi))
static void k32_GetSystemTimeAsFileTime(FILETIME *lpFT)
{
	if (!lpFT)
		return;

	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);

	uint64_t ft = FILETIME_UNIX_DIFF
		      + (uint64_t)ts.tv_sec * 10000000ULL
		      + (uint64_t)ts.tv_nsec / 100;

	lpFT->dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF);
	lpFT->dwHighDateTime = (DWORD)(ft >> 32);
}

/* ============================================================
 * 파일 시스템 확장 (Class 52)
 * ============================================================ */

__attribute__((ms_abi))
static BOOL k32_CreateDirectoryA(const char *lpPathName, void *lpSecurity)
{
	(void)lpSecurity;

	if (!lpPathName) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	if (mkdir(lpPathName, 0755) < 0) {
		if (errno == EEXIST)
			last_error = ERROR_ALREADY_EXISTS;
		else
			last_error = ERROR_PATH_NOT_FOUND;
		return FALSE;
	}

	return TRUE;
}

__attribute__((ms_abi))
static BOOL k32_RemoveDirectoryA(const char *lpPathName)
{
	if (!lpPathName) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	if (rmdir(lpPathName) < 0) {
		last_error = ERROR_PATH_NOT_FOUND;
		return FALSE;
	}

	return TRUE;
}

__attribute__((ms_abi))
static DWORD k32_GetTempPathA(DWORD nBufferLength, char *lpBuffer)
{
	const char *tmp = "/tmp/";
	DWORD len = 5;

	if (!lpBuffer || nBufferLength < len + 1)
		return len + 1;

	memcpy(lpBuffer, tmp, len + 1);
	return len;
}

__attribute__((ms_abi))
static DWORD k32_GetCurrentDirectoryA(DWORD nBufferLength, char *lpBuffer)
{
	if (!lpBuffer || nBufferLength == 0) {
		char tmp[MAX_PATH];

		if (getcwd(tmp, sizeof(tmp)))
			return (DWORD)strlen(tmp) + 1;
		return 0;
	}

	if (!getcwd(lpBuffer, nBufferLength)) {
		last_error = ERROR_GEN_FAILURE;
		return 0;
	}

	return (DWORD)strlen(lpBuffer);
}

__attribute__((ms_abi))
static BOOL k32_SetCurrentDirectoryA(const char *lpPathName)
{
	if (!lpPathName) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	if (chdir(lpPathName) < 0) {
		last_error = ERROR_PATH_NOT_FOUND;
		return FALSE;
	}

	return TRUE;
}

/* ============================================================
 * FindFirstFileA / FindNextFileA / FindClose
 * ============================================================
 *
 * Windows 파일 검색 API → opendir + readdir + fnmatch.
 *
 * 내부 상태:
 *   핸들의 extra → find_state (DIR*, 패턴, 디렉토리 경로)
 */

struct find_state {
	DIR *dir;
	char pattern[MAX_PATH];   /* 파일 패턴 (예: "*.exe") */
	char dirpath[MAX_PATH];   /* 디렉토리 경로 */
};

static void fill_find_data(WIN32_FIND_DATAA *lpFD, const char *dirpath,
			   const char *name)
{
	memset(lpFD, 0, sizeof(*lpFD));
	snprintf(lpFD->cFileName, MAX_PATH, "%s", name);

	char full[MAX_PATH * 2];

	snprintf(full, sizeof(full), "%.259s/%.259s", dirpath, name);

	struct stat st;

	if (stat(full, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			lpFD->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
		else
			lpFD->dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
		lpFD->nFileSizeLow = (DWORD)(st.st_size & 0xFFFFFFFF);
		lpFD->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
	}
}

__attribute__((ms_abi))
static HANDLE k32_FindFirstFileA(const char *lpFileName,
				 WIN32_FIND_DATAA *lpFindFileData)
{
	if (!lpFileName || !lpFindFileData) {
		last_error = ERROR_INVALID_PARAMETER;
		return INVALID_HANDLE_VALUE;
	}

	/* 경로와 패턴 분리: "dir/pattern" → dir, pattern */
	char path_copy[MAX_PATH];

	snprintf(path_copy, sizeof(path_copy), "%s", lpFileName);

	/* 백슬래시 → 슬래시 */
	for (char *p = path_copy; *p; p++)
		if (*p == '\\') *p = '/';

	/* 마지막 슬래시 찾기 */
	char *slash = NULL;

	for (char *p = path_copy; *p; p++)
		if (*p == '/') slash = p;

	char dirpath[MAX_PATH];
	char pattern[MAX_PATH];

	if (slash) {
		if (slash == path_copy) {
			/* 루트 경로: "/tmp" → dir="/", pattern="tmp" */
			snprintf(dirpath, sizeof(dirpath), "/");
			snprintf(pattern, sizeof(pattern), "%s", slash + 1);
		} else {
			*slash = '\0';
			snprintf(dirpath, sizeof(dirpath), "%s", path_copy);
			snprintf(pattern, sizeof(pattern), "%s", slash + 1);
		}
	} else {
		snprintf(dirpath, sizeof(dirpath), ".");
		snprintf(pattern, sizeof(pattern), "%s", path_copy);
	}

	DIR *dir = opendir(dirpath);

	if (!dir) {
		last_error = ERROR_PATH_NOT_FOUND;
		return INVALID_HANDLE_VALUE;
	}

	/* 첫 번째 매치 찾기 */
	struct dirent *de;

	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;

		if (fnmatch(pattern, de->d_name, 0) == 0) {
			fill_find_data(lpFindFileData, dirpath, de->d_name);

			/* find_state 할당 */
			struct find_state *fs = calloc(1, sizeof(*fs));

			if (!fs) {
				closedir(dir);
				last_error = ERROR_GEN_FAILURE;
				return INVALID_HANDLE_VALUE;
			}

			fs->dir = dir;
			snprintf(fs->pattern, MAX_PATH, "%s", pattern);
			snprintf(fs->dirpath, MAX_PATH, "%s", dirpath);

			HANDLE h = ob_create_handle_ex(OB_FILE, fs);

			if (h == INVALID_HANDLE_VALUE) {
				closedir(dir);
				free(fs);
				last_error = ERROR_GEN_FAILURE;
				return INVALID_HANDLE_VALUE;
			}

			return h;
		}
	}

	closedir(dir);
	last_error = ERROR_FILE_NOT_FOUND;
	return INVALID_HANDLE_VALUE;
}

__attribute__((ms_abi))
static BOOL k32_FindNextFileA(HANDLE hFindFile, WIN32_FIND_DATAA *lpFindFileData)
{
	if (!lpFindFileData) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	struct ob_entry *entry = ob_ref_handle(hFindFile);

	if (!entry || !entry->extra) {
		last_error = ERROR_INVALID_HANDLE;
		return FALSE;
	}

	struct find_state *fs = (struct find_state *)entry->extra;
	struct dirent *de;

	while ((de = readdir(fs->dir)) != NULL) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;

		if (fnmatch(fs->pattern, de->d_name, 0) == 0) {
			fill_find_data(lpFindFileData, fs->dirpath,
				       de->d_name);
			return TRUE;
		}
	}

	last_error = ERROR_FILE_NOT_FOUND;
	return FALSE;
}

__attribute__((ms_abi))
static BOOL k32_FindClose(HANDLE hFindFile)
{
	struct ob_entry *entry = ob_ref_handle(hFindFile);

	if (!entry || !entry->extra) {
		last_error = ERROR_INVALID_HANDLE;
		return FALSE;
	}

	struct find_state *fs = (struct find_state *)entry->extra;

	closedir(fs->dir);
	free(fs);
	ob_close_handle(hFindFile);
	return TRUE;
}

/* ============================================================
 * 파일 속성
 * ============================================================ */

__attribute__((ms_abi))
static DWORD k32_GetFileAttributesA(const char *lpFileName)
{
	if (!lpFileName)
		return INVALID_FILE_ATTRIBUTES;

	struct stat st;

	if (stat(lpFileName, &st) < 0)
		return INVALID_FILE_ATTRIBUTES;

	if (S_ISDIR(st.st_mode))
		return FILE_ATTRIBUTE_DIRECTORY;

	return FILE_ATTRIBUTE_ARCHIVE;
}

__attribute__((ms_abi))
static DWORD k32_GetFileType(HANDLE hFile)
{
	struct ob_entry *entry = ob_ref_handle(hFile);

	if (!entry)
		return FILE_TYPE_UNKNOWN;

	if (entry->type == OB_CONSOLE)
		return FILE_TYPE_CHAR;

	return FILE_TYPE_DISK;
}

/* ============================================================
 * 시스템 정보 (Class 52)
 * ============================================================ */

__attribute__((ms_abi))
static void k32_GetSystemInfo(SYSTEM_INFO *lpSI)
{
	if (!lpSI)
		return;

	memset(lpSI, 0, sizeof(*lpSI));
	lpSI->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
	lpSI->dwPageSize = 4096;
	lpSI->lpMinimumApplicationAddress = (void *)0x10000;
	lpSI->lpMaximumApplicationAddress = (void *)0x7FFFFFFEFFFF;
	lpSI->dwNumberOfProcessors = (DWORD)sysconf(_SC_NPROCESSORS_ONLN);
	if (lpSI->dwNumberOfProcessors == 0)
		lpSI->dwNumberOfProcessors = 1;
	lpSI->dwActiveProcessorMask = (1UL << lpSI->dwNumberOfProcessors) - 1;
	lpSI->dwProcessorType = 8664; /* AMD64 */
	lpSI->dwAllocationGranularity = 65536;
	lpSI->wProcessorLevel = 6;
}

__attribute__((ms_abi))
static BOOL k32_GlobalMemoryStatusEx(MEMORYSTATUSEX *lpBuffer)
{
	if (!lpBuffer)
		return FALSE;

	struct sysinfo si;

	sysinfo(&si);

	lpBuffer->dwLength = sizeof(MEMORYSTATUSEX);
	lpBuffer->ullTotalPhys = (uint64_t)si.totalram * si.mem_unit;
	lpBuffer->ullAvailPhys = (uint64_t)si.freeram * si.mem_unit;
	lpBuffer->ullTotalPageFile = (uint64_t)si.totalswap * si.mem_unit;
	lpBuffer->ullAvailPageFile = (uint64_t)si.freeswap * si.mem_unit;
	lpBuffer->ullTotalVirtual = 0x7FFFFFFFFFFF; /* 128 TB */
	lpBuffer->ullAvailVirtual = 0x7FFFFFFFFFFF;

	if (lpBuffer->ullTotalPhys > 0)
		lpBuffer->dwMemoryLoad = (DWORD)(
			(lpBuffer->ullTotalPhys - lpBuffer->ullAvailPhys)
			* 100 / lpBuffer->ullTotalPhys);
	else
		lpBuffer->dwMemoryLoad = 0;

	return TRUE;
}

__attribute__((ms_abi))
static BOOL k32_GetVersionExA(OSVERSIONINFOA *lpVersionInfo)
{
	if (!lpVersionInfo)
		return FALSE;

	memset(lpVersionInfo, 0, sizeof(*lpVersionInfo));
	lpVersionInfo->dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
	lpVersionInfo->dwMajorVersion = 10;  /* Windows 10 */
	lpVersionInfo->dwMinorVersion = 0;
	lpVersionInfo->dwBuildNumber = 19041;
	lpVersionInfo->dwPlatformId = 2;     /* VER_PLATFORM_WIN32_NT */
	return TRUE;
}

__attribute__((ms_abi))
static BOOL k32_GetComputerNameA(char *lpBuffer, DWORD *nSize)
{
	if (!lpBuffer || !nSize) {
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	char hostname[256];

	if (gethostname(hostname, sizeof(hostname)) < 0) {
		last_error = ERROR_GEN_FAILURE;
		return FALSE;
	}

	DWORD len = (DWORD)strlen(hostname);

	if (len >= *nSize) {
		*nSize = len + 1;
		last_error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	memcpy(lpBuffer, hostname, len + 1);
	*nSize = len;
	return TRUE;
}

__attribute__((ms_abi))
static UINT k32_GetSystemDirectoryA(char *lpBuffer, UINT uSize)
{
	const char *dir = "C:\\Windows\\System32";
	UINT len = 19;

	if (!lpBuffer || uSize < len + 1)
		return len + 1;

	memcpy(lpBuffer, dir, len + 1);
	return len;
}

__attribute__((ms_abi))
static UINT k32_GetWindowsDirectoryA(char *lpBuffer, UINT uSize)
{
	const char *dir = "C:\\Windows";
	UINT len = 10;

	if (!lpBuffer || uSize < len + 1)
		return len + 1;

	memcpy(lpBuffer, dir, len + 1);
	return len;
}

/* ============================================================
 * 스텁 테이블 — DLL 함수 이름 → 구현 매핑
 * ============================================================ */
struct stub_entry kernel32_stub_table[] = {
	/* 프로세스 관리 */
	{ "kernel32.dll", "ExitProcess",    (void *)k32_ExitProcess },
	{ "kernel32.dll", "GetCurrentProcess", (void *)k32_GetCurrentProcess },
	{ "kernel32.dll", "GetCurrentProcessId", (void *)k32_GetCurrentProcessId },
	{ "kernel32.dll", "GetCurrentThreadId", (void *)k32_GetCurrentThreadId },

	/* 콘솔/표준 핸들 */
	{ "kernel32.dll", "GetStdHandle",   (void *)k32_GetStdHandle },

	/* 파일 I/O */
	{ "kernel32.dll", "CreateFileA",    (void *)k32_CreateFileA },
	{ "kernel32.dll", "WriteFile",      (void *)k32_WriteFile },
	{ "kernel32.dll", "ReadFile",       (void *)k32_ReadFile },
	{ "kernel32.dll", "CloseHandle",    (void *)k32_CloseHandle },
	{ "kernel32.dll", "GetFileSize",    (void *)k32_GetFileSize },
	{ "kernel32.dll", "SetFilePointer", (void *)k32_SetFilePointer },
	{ "kernel32.dll", "DeleteFileA",    (void *)k32_DeleteFileA },

	/* 메모리 관리 */
	{ "kernel32.dll", "VirtualAlloc",   (void *)k32_VirtualAlloc },
	{ "kernel32.dll", "VirtualFree",    (void *)k32_VirtualFree },
	{ "kernel32.dll", "GetProcessHeap", (void *)k32_GetProcessHeap },
	{ "kernel32.dll", "HeapAlloc",      (void *)k32_HeapAlloc },
	{ "kernel32.dll", "HeapFree",       (void *)k32_HeapFree },

	/* 환경변수/명령줄 */
	{ "kernel32.dll", "GetEnvironmentVariableA", (void *)k32_GetEnvironmentVariableA },
	{ "kernel32.dll", "SetEnvironmentVariableA", (void *)k32_SetEnvironmentVariableA },
	{ "kernel32.dll", "GetCommandLineA", (void *)k32_GetCommandLineA },

	/* 모듈 */
	{ "kernel32.dll", "GetModuleHandleA", (void *)k32_GetModuleHandleA },
	{ "kernel32.dll", "GetModuleFileNameA", (void *)k32_GetModuleFileNameA },

	/* 에러 처리 */
	{ "kernel32.dll", "GetLastError",   (void *)k32_GetLastError },
	{ "kernel32.dll", "SetLastError",   (void *)k32_SetLastError },

	/* 스레딩 */
	{ "kernel32.dll", "CreateThread",   (void *)k32_CreateThread },
	{ "kernel32.dll", "ExitThread",     (void *)k32_ExitThread },
	{ "kernel32.dll", "GetExitCodeThread", (void *)k32_GetExitCodeThread },

	/* 동기화 - Event */
	{ "kernel32.dll", "CreateEventA",   (void *)k32_CreateEventA },
	{ "kernel32.dll", "SetEvent",       (void *)k32_SetEvent },
	{ "kernel32.dll", "ResetEvent",     (void *)k32_ResetEvent },

	/* 동기화 - Mutex */
	{ "kernel32.dll", "CreateMutexA",   (void *)k32_CreateMutexA },
	{ "kernel32.dll", "ReleaseMutex",   (void *)k32_ReleaseMutex },

	/* 대기 */
	{ "kernel32.dll", "WaitForSingleObject", (void *)k32_WaitForSingleObject },
	{ "kernel32.dll", "WaitForMultipleObjects", (void *)k32_WaitForMultipleObjects },

	/* Critical Section */
	{ "kernel32.dll", "InitializeCriticalSection", (void *)k32_InitializeCriticalSection },
	{ "kernel32.dll", "EnterCriticalSection", (void *)k32_EnterCriticalSection },
	{ "kernel32.dll", "LeaveCriticalSection", (void *)k32_LeaveCriticalSection },
	{ "kernel32.dll", "DeleteCriticalSection", (void *)k32_DeleteCriticalSection },

	/* Interlocked */
	{ "kernel32.dll", "InterlockedIncrement", (void *)k32_InterlockedIncrement },
	{ "kernel32.dll", "InterlockedDecrement", (void *)k32_InterlockedDecrement },
	{ "kernel32.dll", "InterlockedExchange", (void *)k32_InterlockedExchange },
	{ "kernel32.dll", "InterlockedCompareExchange", (void *)k32_InterlockedCompareExchange },

	/* Sleep */
	{ "kernel32.dll", "Sleep",          (void *)k32_Sleep },

	/* TLS */
	{ "kernel32.dll", "TlsAlloc",      (void *)k32_TlsAlloc },
	{ "kernel32.dll", "TlsGetValue",   (void *)k32_TlsGetValue },
	{ "kernel32.dll", "TlsSetValue",   (void *)k32_TlsSetValue },
	{ "kernel32.dll", "TlsFree",       (void *)k32_TlsFree },

	/* 시간 */
	{ "kernel32.dll", "GetTickCount",   (void *)k32_GetTickCount },
	{ "kernel32.dll", "GetTickCount64", (void *)k32_GetTickCount64 },
	{ "kernel32.dll", "QueryPerformanceCounter", (void *)k32_QueryPerformanceCounter },
	{ "kernel32.dll", "QueryPerformanceFrequency", (void *)k32_QueryPerformanceFrequency },
	{ "kernel32.dll", "GetSystemTimeAsFileTime", (void *)k32_GetSystemTimeAsFileTime },

	/* 파일 시스템 */
	{ "kernel32.dll", "CreateDirectoryA", (void *)k32_CreateDirectoryA },
	{ "kernel32.dll", "RemoveDirectoryA", (void *)k32_RemoveDirectoryA },
	{ "kernel32.dll", "GetTempPathA",   (void *)k32_GetTempPathA },
	{ "kernel32.dll", "GetCurrentDirectoryA", (void *)k32_GetCurrentDirectoryA },
	{ "kernel32.dll", "SetCurrentDirectoryA", (void *)k32_SetCurrentDirectoryA },
	{ "kernel32.dll", "FindFirstFileA", (void *)k32_FindFirstFileA },
	{ "kernel32.dll", "FindNextFileA",  (void *)k32_FindNextFileA },
	{ "kernel32.dll", "FindClose",      (void *)k32_FindClose },
	{ "kernel32.dll", "GetFileAttributesA", (void *)k32_GetFileAttributesA },
	{ "kernel32.dll", "GetFileType",    (void *)k32_GetFileType },

	/* 시스템 정보 */
	{ "kernel32.dll", "GetSystemInfo",  (void *)k32_GetSystemInfo },
	{ "kernel32.dll", "GlobalMemoryStatusEx", (void *)k32_GlobalMemoryStatusEx },
	{ "kernel32.dll", "GetVersionExA",  (void *)k32_GetVersionExA },
	{ "kernel32.dll", "GetComputerNameA", (void *)k32_GetComputerNameA },
	{ "kernel32.dll", "GetSystemDirectoryA", (void *)k32_GetSystemDirectoryA },
	{ "kernel32.dll", "GetWindowsDirectoryA", (void *)k32_GetWindowsDirectoryA },

	/* 테이블 끝 */
	{ NULL, NULL, NULL }
};
