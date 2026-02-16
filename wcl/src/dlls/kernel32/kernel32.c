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

	/* 테이블 끝 */
	{ NULL, NULL, NULL }
};
