/*
 * ntdll.c - NT Native API 구현
 * ==============================
 *
 * Windows의 ntdll.dll에 해당하는 구현.
 * kernel32.c에 있던 핵심 로직(파일 I/O, 경로 변환)을 이쪽으로 이동.
 *
 * 계층 구조:
 *   kernel32.CreateFileA()     ← Win32 인터페이스
 *     → nt_create_file()      ← NT 네이티브 (이 파일)
 *       → open()              ← POSIX 시스콜
 *
 * 왜 이렇게 분리하는가?
 *   1. 실제 Windows 아키텍처와 동일한 구조
 *   2. ntdll을 직접 호출하는 프로그램 지원 (Nt* API)
 *   3. Object Manager를 통한 스레드 안전한 핸들 관리
 *   4. NTSTATUS 기반 에러 처리 (kernel32이 Win32 에러로 변환)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "ntdll.h"
#include "object_manager.h"

/* ============================================================
 * errno → NTSTATUS 변환
 * ============================================================
 *
 * POSIX errno를 NT 상태 코드로 변환.
 * kernel32.c의 errno_to_win32()를 대체.
 */
static NTSTATUS errno_to_ntstatus(int err)
{
	switch (err) {
	case 0:         return STATUS_SUCCESS;
	case ENOENT:    return STATUS_OBJECT_NAME_NOT_FOUND;
	case EACCES:    return STATUS_ACCESS_DENIED;
	case EPERM:     return STATUS_ACCESS_DENIED;
	case EEXIST:    return STATUS_OBJECT_NAME_COLLISION;
	case EMFILE:    return STATUS_TOO_MANY_OPENED_FILES;
	case ENFILE:    return STATUS_TOO_MANY_OPENED_FILES;
	case ENOSPC:    return STATUS_DISK_FULL;
	case EISDIR:    return STATUS_OBJECT_NAME_NOT_FOUND;
	case ENOTDIR:   return STATUS_NOT_A_DIRECTORY;
	case EBADF:     return STATUS_INVALID_HANDLE;
	case EINVAL:    return STATUS_INVALID_PARAMETER;
	case EIO:       return STATUS_UNSUCCESSFUL;
	case EROFS:     return STATUS_ACCESS_DENIED;
	case ENAMETOOLONG: return STATUS_OBJECT_NAME_NOT_FOUND;
	default:        return STATUS_UNSUCCESSFUL;
	}
}

/* ============================================================
 * NTSTATUS → Win32 에러 코드 변환
 * ============================================================
 *
 * kernel32이 Nt* 함수 호출 후 SetLastError를 위해 사용.
 */
uint32_t nt_status_to_win32(NTSTATUS status)
{
	switch (status) {
	case STATUS_SUCCESS:
		return ERROR_SUCCESS;
	case STATUS_OBJECT_NAME_NOT_FOUND:
		return ERROR_FILE_NOT_FOUND;
	case STATUS_ACCESS_DENIED:
		return ERROR_ACCESS_DENIED;
	case STATUS_OBJECT_NAME_COLLISION:
		return ERROR_ALREADY_EXISTS;
	case STATUS_TOO_MANY_OPENED_FILES:
		return ERROR_TOO_MANY_OPEN_FILES;
	case STATUS_DISK_FULL:
		return ERROR_DISK_FULL;
	case STATUS_INVALID_HANDLE:
		return ERROR_INVALID_HANDLE;
	case STATUS_INVALID_PARAMETER:
		return ERROR_INVALID_PARAMETER;
	case STATUS_NOT_A_DIRECTORY:
		return ERROR_PATH_NOT_FOUND;
	default:
		return ERROR_GEN_FAILURE;
	}
}

/* ============================================================
 * 경로 변환
 * ============================================================
 *
 * Windows 경로 → Linux 경로 변환.
 *
 * 변환 규칙:
 *   C:\Users\test.txt → /Users/test.txt  (드라이브 문자 제거)
 *   D:\path\file      → /path/file
 *   relative.txt      → relative.txt      (상대 경로 유지)
 *   백슬래시(\)       → 슬래시(/)
 *
 * 반환: 변환된 바이트 수, 또는 -1 (에러/truncation)
 */
int nt_translate_path(const char *win_path, char *linux_path, size_t size)
{
	if (!win_path || !linux_path || size == 0)
		return -1;

	const char *src = win_path;

	/* 드라이브 문자 감지: "C:" 또는 "c:" 패턴 */
	if (((src[0] >= 'A' && src[0] <= 'Z') ||
	     (src[0] >= 'a' && src[0] <= 'z')) && src[1] == ':') {
		src += 2; /* 드라이브 문자 건너뛰기 */
	}

	/* 나머지를 복사하면서 백슬래시 → 슬래시 변환 */
	size_t i;

	for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
		if (src[i] == '\\')
			linux_path[i] = '/';
		else
			linux_path[i] = src[i];
	}
	linux_path[i] = '\0';

	/* 경로가 잘렸는지 확인 */
	if (src[i] != '\0')
		return -1;

	return (int)i;
}

/* ============================================================
 * ntdll_init — 초기화
 * ============================================================ */
void ntdll_init(void)
{
	ob_init();
}

/* ============================================================
 * NtCreateFile — 파일 열기/생성
 * ============================================================
 *
 * kernel32.c의 k32_CreateFileA 내부 로직을 이동.
 *
 * 실제 Windows NtCreateFile의 시그니처:
 *   NTSTATUS NtCreateFile(
 *     PHANDLE FileHandle,
 *     ACCESS_MASK DesiredAccess,
 *     POBJECT_ATTRIBUTES ObjectAttributes,  ← 복잡!
 *     PIO_STATUS_BLOCK IoStatusBlock,
 *     PLARGE_INTEGER AllocationSize,
 *     ULONG FileAttributes,
 *     ULONG ShareAccess,
 *     ULONG CreateDisposition,
 *     ULONG CreateOptions,
 *     PVOID EaBuffer,
 *     ULONG EaLength
 *   );
 *
 * 우리는 핵심만 추출한 단순화 버전.
 */
NTSTATUS nt_create_file(HANDLE *out_handle,
			uint32_t desired_access,
			const char *path,
			uint32_t creation_disposition)
{
	if (!out_handle || !path)
		return STATUS_INVALID_PARAMETER;

	*out_handle = INVALID_HANDLE_VALUE;

	/* 경로 변환 */
	char linux_path[NT_MAX_PATH];

	if (nt_translate_path(path, linux_path, sizeof(linux_path)) < 0)
		return STATUS_OBJECT_NAME_NOT_FOUND;

	/* 접근 권한 → POSIX 플래그 */
	int flags = 0;
	int has_read = (desired_access & GENERIC_READ) != 0;
	int has_write = (desired_access & GENERIC_WRITE) != 0;

	if (has_read && has_write)
		flags = O_RDWR;
	else if (has_write)
		flags = O_WRONLY;
	else
		flags = O_RDONLY;

	/* 생성 모드 → POSIX 플래그 */
	switch (creation_disposition) {
	case CREATE_NEW:
		flags |= O_CREAT | O_EXCL;
		break;
	case CREATE_ALWAYS:
		flags |= O_CREAT | O_TRUNC;
		break;
	case OPEN_EXISTING:
		break;
	case OPEN_ALWAYS:
		flags |= O_CREAT;
		break;
	case TRUNCATE_EXISTING:
		flags |= O_TRUNC;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}

	/* POSIX open() */
	int fd = open(linux_path, flags, 0644);

	if (fd < 0)
		return errno_to_ntstatus(errno);

	/* Object Manager에서 핸들 할당 */
	HANDLE h = ob_create_handle(fd, OB_FILE, desired_access);

	if (h == INVALID_HANDLE_VALUE) {
		close(fd);
		return STATUS_TOO_MANY_OPENED_FILES;
	}

	*out_handle = h;
	return STATUS_SUCCESS;
}

/* ============================================================
 * NtReadFile — 파일 읽기
 * ============================================================ */
NTSTATUS nt_read_file(HANDLE handle, void *buf,
		      uint32_t length, uint32_t *bytes_read)
{
	if (bytes_read)
		*bytes_read = 0;

	struct ob_entry *entry = ob_ref_handle(handle);

	if (!entry)
		return STATUS_INVALID_HANDLE;

	ssize_t ret = read(entry->fd, buf, length);

	if (ret < 0)
		return errno_to_ntstatus(errno);

	if (bytes_read)
		*bytes_read = (uint32_t)ret;

	return STATUS_SUCCESS;
}

/* ============================================================
 * NtWriteFile — 파일 쓰기
 * ============================================================ */
NTSTATUS nt_write_file(HANDLE handle, const void *buf,
		       uint32_t length, uint32_t *bytes_written)
{
	if (bytes_written)
		*bytes_written = 0;

	struct ob_entry *entry = ob_ref_handle(handle);

	if (!entry)
		return STATUS_INVALID_HANDLE;

	ssize_t ret = write(entry->fd, buf, length);

	if (ret < 0)
		return errno_to_ntstatus(errno);

	if (bytes_written)
		*bytes_written = (uint32_t)ret;

	return STATUS_SUCCESS;
}

/* ============================================================
 * NtClose — 핸들 닫기
 * ============================================================ */
NTSTATUS nt_close(HANDLE handle)
{
	struct ob_entry *entry = ob_ref_handle(handle);

	if (!entry)
		return STATUS_INVALID_HANDLE;

	/* 콘솔 핸들은 닫지 않음 */
	if (entry->type == OB_CONSOLE) {
		return STATUS_SUCCESS;
	}

	int fd = entry->fd;

	ob_close_handle(handle);

	if (close(fd) < 0)
		return errno_to_ntstatus(errno);

	return STATUS_SUCCESS;
}

/* ============================================================
 * NtQueryInformationFile — 파일 크기 조회
 * ============================================================ */
NTSTATUS nt_query_file_size(HANDLE handle, uint64_t *size_out)
{
	if (!size_out)
		return STATUS_INVALID_PARAMETER;

	struct ob_entry *entry = ob_ref_handle(handle);

	if (!entry)
		return STATUS_INVALID_HANDLE;

	struct stat st;

	if (fstat(entry->fd, &st) < 0)
		return errno_to_ntstatus(errno);

	*size_out = (uint64_t)st.st_size;
	return STATUS_SUCCESS;
}

/* ============================================================
 * NtSetInformationFile — 파일 포인터 이동
 * ============================================================ */
NTSTATUS nt_set_file_position(HANDLE handle, int64_t offset, int whence)
{
	struct ob_entry *entry = ob_ref_handle(handle);

	if (!entry)
		return STATUS_INVALID_HANDLE;

	off_t result = lseek(entry->fd, (off_t)offset, whence);

	if (result < 0)
		return errno_to_ntstatus(errno);

	return STATUS_SUCCESS;
}

/* ============================================================
 * NtDeleteFile — 파일 삭제
 * ============================================================ */
NTSTATUS nt_delete_file(const char *path)
{
	if (!path)
		return STATUS_INVALID_PARAMETER;

	char linux_path[NT_MAX_PATH];

	if (nt_translate_path(path, linux_path, sizeof(linux_path)) < 0)
		return STATUS_OBJECT_NAME_NOT_FOUND;

	if (unlink(linux_path) < 0)
		return errno_to_ntstatus(errno);

	return STATUS_SUCCESS;
}

/* ============================================================
 * ntdll 스텁 테이블
 * ============================================================
 *
 * ntdll.dll을 직접 임포트하는 프로그램용.
 * 현재는 비어 있음 — Nt* API는 ms_abi 래퍼가 필요하므로
 * 추후 추가.
 */
struct stub_entry ntdll_stub_table[] = {
	/* 추후: { "ntdll.dll", "NtCreateFile", (void *)nt_create_file_msabi }, */
	{ NULL, NULL, NULL }
};
