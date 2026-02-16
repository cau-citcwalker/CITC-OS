/*
 * ntdll.h - NT Native API 선언
 * ==============================
 *
 * ntdll.dll은 Windows의 가장 낮은 유저 모드 DLL입니다.
 * kernel32.dll, advapi32.dll 등 모든 Win32 API가
 * 내부적으로 ntdll의 Nt* 함수를 호출합니다.
 *
 * Windows API 계층:
 *
 *   Win32 앱
 *     ↓
 *   kernel32.dll  (CreateFileA)
 *     ↓
 *   ntdll.dll     (NtCreateFile)     ← 이 파일
 *     ↓
 *   syscall       (커널 진입)
 *     ↓
 *   ntoskrnl.exe  (커널 모드 NtCreateFile)
 *
 * 왜 두 계층이 있는가?
 *   kernel32: 사용자 친화적 API (문자열, 경로 변환 등)
 *   ntdll:    커널에 가까운 저수준 API (핸들, 바이트 단위)
 *
 *   예: CreateFileA("test.txt")
 *     1. kernel32: 문자열을 유니코드로 변환, 경로 확장
 *     2. ntdll: NtCreateFile(ObjectAttributes, ...)
 *     3. 커널: 실제 파일 시스템 접근
 *
 * NT 상태 코드 (NTSTATUS):
 *   Nt* 함수들은 Win32 에러 코드 대신 NTSTATUS를 반환합니다.
 *   성공: STATUS_SUCCESS (0)
 *   실패: 음수 값 (bit 31 = 1)
 *   kernel32이 NTSTATUS → Win32 에러 코드로 변환합니다.
 *
 * 우리 구현:
 *   kernel32.c에 있던 파일 I/O 로직을 이쪽으로 이동.
 *   kernel32는 Nt* 함수를 호출하는 얇은 래퍼가 됨.
 */

#ifndef CITC_NTDLL_H
#define CITC_NTDLL_H

#include <stdint.h>
#include "../../include/win32.h"
#include "object_manager.h"

/* ============================================================
 * NT 상태 코드 (NTSTATUS)
 * ============================================================
 *
 * bit 31 = severity (0=success/info, 1=warning/error)
 * bit 30 = customer flag
 * bit 29 = reserved
 * bit 28 = NTSTATUS (vs HRESULT)
 * bit 16-27 = facility
 * bit 0-15 = code
 */
typedef int32_t NTSTATUS;

#define STATUS_SUCCESS            ((NTSTATUS)0x00000000)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define STATUS_ACCESS_DENIED      ((NTSTATUS)0xC0000022)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035)
#define STATUS_TOO_MANY_OPENED_FILES ((NTSTATUS)0xC000011F)
#define STATUS_DISK_FULL          ((NTSTATUS)0xC000007F)
#define STATUS_INVALID_HANDLE     ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER  ((NTSTATUS)0xC000000D)
#define STATUS_UNSUCCESSFUL       ((NTSTATUS)0xC0000001)
#define STATUS_NOT_A_DIRECTORY    ((NTSTATUS)0xC0000103)
#define STATUS_END_OF_FILE        ((NTSTATUS)0xC0000011)

/* NTSTATUS 매크로 */
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)

/* ============================================================
 * 경로 변환 (내부 유틸리티)
 * ============================================================ */
#define NT_MAX_PATH 1024

/*
 * nt_translate_path — Windows 경로 → Linux 경로
 *
 * 반환: 변환된 바이트 수, 또는 -1 (truncation/에러)
 */
int nt_translate_path(const char *win_path, char *linux_path, size_t size);

/* ============================================================
 * NT Native API 함수
 * ============================================================ */

/*
 * NtCreateFile — 파일 열기/생성
 *
 * kernel32 CreateFileA의 내부 구현.
 * POSIX open()에 매핑.
 */
NTSTATUS nt_create_file(HANDLE *out_handle,
			uint32_t desired_access,
			const char *path,
			uint32_t creation_disposition);

/*
 * NtReadFile — 파일 읽기
 */
NTSTATUS nt_read_file(HANDLE handle, void *buf,
		      uint32_t length, uint32_t *bytes_read);

/*
 * NtWriteFile — 파일 쓰기
 */
NTSTATUS nt_write_file(HANDLE handle, const void *buf,
		       uint32_t length, uint32_t *bytes_written);

/*
 * NtClose — 핸들 닫기
 */
NTSTATUS nt_close(HANDLE handle);

/*
 * NtQueryInformationFile — 파일 정보 조회 (크기 등)
 */
NTSTATUS nt_query_file_size(HANDLE handle, uint64_t *size_out);

/*
 * NtSetInformationFile — 파일 포인터 이동
 */
NTSTATUS nt_set_file_position(HANDLE handle, int64_t offset, int whence);

/*
 * NtDeleteFile — 파일 삭제
 */
NTSTATUS nt_delete_file(const char *path);

/* ============================================================
 * 초기화
 * ============================================================ */

/*
 * ntdll_init — NT 서브시스템 초기화
 *
 * Object Manager + 에러 코드 테이블 초기화.
 * kernel32_init() 대신 이것을 호출.
 */
void ntdll_init(void);

/* ============================================================
 * 에러 변환
 * ============================================================ */

/*
 * nt_status_to_win32 — NTSTATUS → Win32 에러 코드
 *
 * kernel32이 GetLastError를 위해 사용.
 */
uint32_t nt_status_to_win32(NTSTATUS status);

/* ============================================================
 * 스텁 테이블 (citcrun의 import resolver에서 사용)
 * ============================================================ */

#include "../../include/stub_entry.h"
extern struct stub_entry ntdll_stub_table[];

#endif /* CITC_NTDLL_H */
