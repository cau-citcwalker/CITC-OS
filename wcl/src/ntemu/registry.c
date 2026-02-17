/*
 * registry.c - Windows 레지스트리 v0.1 구현
 * ============================================
 *
 * 파일 시스템 기반 레지스트리.
 *
 * 디렉토리 구조:
 *   /etc/citc-registry/
 *     HKLM/                      ← HKEY_LOCAL_MACHINE
 *       SOFTWARE/
 *         Microsoft/
 *           Windows/
 *             CurrentVersion/
 *       SYSTEM/
 *         DriveMapping/
 *           C                    ← 값 파일 (REG_SZ "/home/user/citc-c/")
 *     HKCU/                      ← HKEY_CURRENT_USER
 *
 * 값 파일 포맷:
 *   [type:4바이트][length:4바이트][data:N바이트]
 *
 * 구현 상태:
 *   - CRUD: RegCreateKey, RegOpenKey, RegSetValue, RegQueryValue,
 *           RegDeleteKey, RegDeleteValue
 *   - 열거: RegEnumKeyEx, RegEnumValue (opendir/readdir 기반)
 *   - 보안: GetUserName, OpenProcessToken (스텁)
 *   - 서비스: OpenSCManager, OpenService (스텁)
 *
 * 한계:
 *   - 알림(RegNotifyChangeKeyValue) 미구현
 *   - 트랜잭션 미지원
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include "registry.h"
#include "object_manager.h"

/* ============================================================
 * 레지스트리 기본 경로 (런타임 결정)
 * ============================================================
 *
 * 우선순위:
 *   1. $CITC_REGISTRY_PATH 환경변수
 *   2. $HOME/.citc-registry (일반 유저)
 *   3. /etc/citc-registry (root / QEMU 환경)
 */
static char registry_base[REG_MAX_PATH] = "";

const char *reg_get_base_path(void)
{
	if (registry_base[0])
		return registry_base;

	/* 환경변수 우선 */
	const char *env = getenv("CITC_REGISTRY_PATH");

	if (env && env[0]) {
		snprintf(registry_base, sizeof(registry_base), "%s", env);
		return registry_base;
	}

	/* root이면 /etc, 아니면 $HOME */
	if (getuid() == 0) {
		snprintf(registry_base, sizeof(registry_base),
			 "%s", REGISTRY_DEFAULT_PATH);
	} else {
		const char *home = getenv("HOME");

		if (home && home[0])
			snprintf(registry_base, sizeof(registry_base),
				 "%s/.citc-registry", home);
		else
			snprintf(registry_base, sizeof(registry_base),
				 "/tmp/citc-registry");
	}

	return registry_base;
}

/* ============================================================
 * 내부 유틸리티
 * ============================================================ */

/*
 * 루트 키 핸들 → 디렉토리 이름 변환
 *
 * HKEY_LOCAL_MACHINE → "HKLM"
 * HKEY_CURRENT_USER  → "HKCU"
 */
static const char *hive_name(HKEY root)
{
	uintptr_t val = (uintptr_t)root;

	switch (val) {
	case 0x80000000: return "HKLM";
	case 0x80000001: return "HKCU";
	case 0x80000002: return "HKLM";
	case 0x80000003: return "HKU";
	default:         return NULL;
	}
}

/*
 * 루트 키인지 확인
 */
static int is_root_key(HKEY key)
{
	uintptr_t val = (uintptr_t)key;

	return (val >= 0x80000000 && val <= 0x80000003);
}

/*
 * 레지스트리 키의 전체 경로 구성
 *
 * parent가 루트 키:
 *   /etc/citc-registry/HKLM/sub_key
 *
 * parent가 Object Manager 핸들:
 *   핸들에 저장된 경로/sub_key
 */
static int build_key_path(HKEY parent, const char *sub_key,
			  char *path, size_t size)
{
	if (is_root_key(parent)) {
		const char *hive = hive_name(parent);

		if (!hive)
			return -1;
		if (sub_key && sub_key[0])
			snprintf(path, size, "%s/%s/%s",
				 reg_get_base_path(), hive, sub_key);
		else
			snprintf(path, size, "%s/%s",
				 reg_get_base_path(), hive);
	} else {
		/* parent는 Object Manager 핸들 */
		struct ob_entry *entry = ob_ref_handle(parent);

		if (!entry || entry->type != OB_REGISTRY_KEY)
			return -1;

		const char *parent_path = (const char *)entry->extra;

		if (!parent_path)
			return -1;
		if (sub_key && sub_key[0])
			snprintf(path, size, "%s/%s", parent_path, sub_key);
		else
			snprintf(path, size, "%s", parent_path);
	}

	/* 백슬래시 → 슬래시 변환 */
	for (char *p = path; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}

	return 0;
}

/*
 * 재귀적 디렉토리 생성 (mkdir -p)
 */
static int mkdir_recursive(const char *path)
{
	char tmp[REG_MAX_PATH];
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);

	/* 끝의 슬래시 제거 */
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}

	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* ============================================================
 * 레지스트리 초기화
 * ============================================================ */

void reg_init(void)
{
	const char *base = reg_get_base_path();
	char path[REG_MAX_PATH];

	/* 기본 디렉토리 구조 생성 */
	snprintf(path, sizeof(path), "%s/HKLM", base);
	mkdir_recursive(path);
	snprintf(path, sizeof(path), "%s/HKCU", base);
	mkdir_recursive(path);
	snprintf(path, sizeof(path), "%s/HKU", base);
	mkdir_recursive(path);
	snprintf(path, sizeof(path), "%s/HKCR", base);
	mkdir_recursive(path);

	/* 드라이브 매핑 기본값 */
	snprintf(path, sizeof(path), "%s/HKLM/SYSTEM/DriveMapping", base);
	mkdir_recursive(path);
}

/* ============================================================
 * RegOpenKeyExA — 키 열기
 * ============================================================ */

uint32_t reg_open_key(HKEY parent, const char *sub_key,
		      uint32_t access, HKEY *result)
{
	if (!result)
		return ERROR_INVALID_PARAMETER;

	*result = NULL;

	char path[REG_MAX_PATH];

	if (build_key_path(parent, sub_key, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	/* 디렉토리 존재 확인 */
	struct stat st;

	if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
		return ERROR_FILE_NOT_FOUND;

	/* 경로 문자열 복사 (핸들의 extra에 저장) */
	char *stored_path = strdup(path);

	if (!stored_path)
		return ERROR_GEN_FAILURE;

	/* Object Manager 핸들 할당 */
	HANDLE h = ob_create_handle(-1, OB_REGISTRY_KEY, access);

	if (h == INVALID_HANDLE_VALUE) {
		free(stored_path);
		return ERROR_GEN_FAILURE;
	}

	/* 핸들에 경로 저장 */
	struct ob_entry *entry = ob_ref_handle(h);

	if (entry)
		entry->extra = stored_path;

	*result = h;
	return ERROR_SUCCESS;
}

/* ============================================================
 * RegCreateKeyExA — 키 생성/열기
 * ============================================================ */

uint32_t reg_create_key(HKEY parent, const char *sub_key,
			uint32_t access, HKEY *result,
			uint32_t *disposition)
{
	if (!result)
		return ERROR_INVALID_PARAMETER;

	*result = NULL;

	char path[REG_MAX_PATH];

	if (build_key_path(parent, sub_key, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	/* 이미 존재하는지 확인 */
	struct stat st;
	int existed = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));

	if (!existed) {
		if (mkdir_recursive(path) < 0)
			return ERROR_ACCESS_DENIED;
	}

	if (disposition)
		*disposition = existed ? REG_OPENED_EXISTING_KEY
				      : REG_CREATED_NEW_KEY;

	/* 경로 문자열 복사 */
	char *stored_path = strdup(path);

	if (!stored_path)
		return ERROR_GEN_FAILURE;

	/* Object Manager 핸들 할당 */
	HANDLE h = ob_create_handle(-1, OB_REGISTRY_KEY, access);

	if (h == INVALID_HANDLE_VALUE) {
		free(stored_path);
		return ERROR_GEN_FAILURE;
	}

	struct ob_entry *entry = ob_ref_handle(h);

	if (entry)
		entry->extra = stored_path;

	*result = h;
	return ERROR_SUCCESS;
}

/* ============================================================
 * RegCloseKey — 키 닫기
 * ============================================================ */

uint32_t reg_close_key(HKEY key)
{
	/* 루트 키는 닫지 않음 */
	if (is_root_key(key))
		return ERROR_SUCCESS;

	struct ob_entry *entry = ob_ref_handle(key);

	if (!entry)
		return ERROR_INVALID_HANDLE;

	/* 저장된 경로 문자열 해제 */
	if (entry->extra)
		free(entry->extra);

	ob_close_handle(key);
	return ERROR_SUCCESS;
}

/* ============================================================
 * RegQueryValueExA — 값 읽기
 * ============================================================ */

uint32_t reg_query_value(HKEY key, const char *value_name,
			 uint32_t *type, void *data,
			 uint32_t *data_len)
{
	char path[REG_MAX_PATH];

	if (build_key_path(key, NULL, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	/* 값 이름 → 파일 경로 */
	const char *name = (value_name && value_name[0])
			   ? value_name : "(Default)";
	char value_path[REG_MAX_PATH];

	snprintf(value_path, sizeof(value_path), "%.768s/%.254s", path, name);

	/* 값 파일 열기 */
	int fd = open(value_path, O_RDONLY);

	if (fd < 0)
		return ERROR_FILE_NOT_FOUND;

	/* 헤더 읽기 */
	struct reg_value_header hdr;

	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return ERROR_GEN_FAILURE;
	}

	if (type)
		*type = hdr.type;

	/* 데이터 크기만 요청 (data == NULL) */
	if (!data || !data_len) {
		if (data_len)
			*data_len = hdr.data_len;
		close(fd);
		return ERROR_SUCCESS;
	}

	/* 버퍼가 작으면 ERROR_MORE_DATA */
	if (*data_len < hdr.data_len) {
		*data_len = hdr.data_len;
		close(fd);
		return ERROR_MORE_DATA;
	}

	/* 데이터 읽기 */
	ssize_t n = read(fd, data, hdr.data_len);

	close(fd);

	if (n < 0 || (uint32_t)n != hdr.data_len)
		return ERROR_GEN_FAILURE;

	*data_len = hdr.data_len;
	return ERROR_SUCCESS;
}

/* ============================================================
 * RegSetValueExA — 값 쓰기
 * ============================================================ */

uint32_t reg_set_value(HKEY key, const char *value_name,
		       uint32_t type, const void *data,
		       uint32_t data_len)
{
	if (!data && data_len > 0)
		return ERROR_INVALID_PARAMETER;

	char path[REG_MAX_PATH];

	if (build_key_path(key, NULL, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	/* 키 디렉토리가 존재하는지 확인 */
	struct stat st;

	if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
		return ERROR_FILE_NOT_FOUND;

	/* 값 이름 → 파일 경로 */
	const char *name = (value_name && value_name[0])
			   ? value_name : "(Default)";
	char value_path[REG_MAX_PATH];

	snprintf(value_path, sizeof(value_path), "%.768s/%.254s", path, name);

	/* 값 파일 생성/덮어쓰기 */
	int fd = open(value_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return ERROR_ACCESS_DENIED;

	/* 헤더 쓰기 */
	struct reg_value_header hdr = {
		.type = type,
		.data_len = data_len,
	};

	if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return ERROR_GEN_FAILURE;
	}

	/* 데이터 쓰기 */
	if (data_len > 0) {
		if (write(fd, data, data_len) != (ssize_t)data_len) {
			close(fd);
			return ERROR_GEN_FAILURE;
		}
	}

	close(fd);
	return ERROR_SUCCESS;
}

/* ============================================================
 * RegDeleteKeyA — 키 삭제
 * ============================================================
 *
 * 디렉토리 삭제 (rmdir). 하위 키가 있으면 실패.
 * 실제 Windows: 하위 키가 있으면 ERROR_ACCESS_DENIED.
 */

uint32_t reg_delete_key(HKEY parent, const char *sub_key)
{
	if (!sub_key || !sub_key[0])
		return ERROR_INVALID_PARAMETER;

	char path[REG_MAX_PATH];

	if (build_key_path(parent, sub_key, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	if (rmdir(path) < 0) {
		if (errno == ENOENT)
			return ERROR_FILE_NOT_FOUND;
		if (errno == ENOTEMPTY)
			return ERROR_ACCESS_DENIED;
		return ERROR_ACCESS_DENIED;
	}

	return ERROR_SUCCESS;
}

/* ============================================================
 * RegDeleteValueA — 값 삭제
 * ============================================================ */

uint32_t reg_delete_value(HKEY key, const char *value_name)
{
	if (!value_name || !value_name[0])
		return ERROR_INVALID_PARAMETER;

	char path[REG_MAX_PATH];

	if (build_key_path(key, NULL, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	char value_path[REG_MAX_PATH];

	snprintf(value_path, sizeof(value_path), "%.768s/%.254s",
		 path, value_name);

	if (unlink(value_path) < 0) {
		if (errno == ENOENT)
			return ERROR_FILE_NOT_FOUND;
		return ERROR_ACCESS_DENIED;
	}

	return ERROR_SUCCESS;
}

/* ============================================================
 * RegEnumKeyExA — 하위 키 열거
 * ============================================================
 *
 * opendir + readdir로 디렉토리만 수집, index번째 반환.
 * 실제 Windows는 내부 인덱스를 관리하지만,
 * 우리는 매번 전체 스캔 (간단하지만 O(n²)).
 */

uint32_t reg_enum_key(HKEY key, uint32_t index,
		      char *name, uint32_t *name_len)
{
	if (!name || !name_len)
		return ERROR_INVALID_PARAMETER;

	char path[REG_MAX_PATH];

	if (build_key_path(key, NULL, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	DIR *dir = opendir(path);

	if (!dir)
		return ERROR_FILE_NOT_FOUND;

	uint32_t current = 0;
	struct dirent *de;

	while ((de = readdir(dir)) != NULL) {
		/* . 과 .. 건너뛰기 */
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;

		/* 디렉토리만 (서브키) */
		char full[REG_MAX_PATH];

		snprintf(full, sizeof(full), "%.768s/%.254s",
			 path, de->d_name);

		struct stat st;

		if (stat(full, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;

		if (current == index) {
			uint32_t len = strlen(de->d_name);

			if (len >= *name_len) {
				closedir(dir);
				*name_len = len + 1;
				return ERROR_MORE_DATA;
			}

			memcpy(name, de->d_name, len + 1);
			*name_len = len;
			closedir(dir);
			return ERROR_SUCCESS;
		}

		current++;
	}

	closedir(dir);
	return ERROR_NO_MORE_ITEMS;
}

/* ============================================================
 * RegEnumValueA — 값 열거
 * ============================================================
 *
 * opendir + readdir로 일반 파일만 수집, index번째 반환.
 */

uint32_t reg_enum_value(HKEY key, uint32_t index,
			char *name, uint32_t *name_len,
			uint32_t *type)
{
	if (!name || !name_len)
		return ERROR_INVALID_PARAMETER;

	char path[REG_MAX_PATH];

	if (build_key_path(key, NULL, path, sizeof(path)) < 0)
		return ERROR_INVALID_HANDLE;

	DIR *dir = opendir(path);

	if (!dir)
		return ERROR_FILE_NOT_FOUND;

	uint32_t current = 0;
	struct dirent *de;

	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;

		/* 일반 파일만 (값) */
		char full[REG_MAX_PATH];

		snprintf(full, sizeof(full), "%.768s/%.254s",
			 path, de->d_name);

		struct stat st;

		if (stat(full, &st) < 0 || !S_ISREG(st.st_mode))
			continue;

		if (current == index) {
			uint32_t len = strlen(de->d_name);

			if (len >= *name_len) {
				closedir(dir);
				*name_len = len + 1;
				return ERROR_MORE_DATA;
			}

			memcpy(name, de->d_name, len + 1);
			*name_len = len;

			/* 값 타입 읽기 */
			if (type) {
				int fd = open(full, O_RDONLY);

				if (fd >= 0) {
					struct reg_value_header hdr;

					if (read(fd, &hdr, sizeof(hdr))
					    == sizeof(hdr))
						*type = hdr.type;
					else
						*type = REG_NONE;
					close(fd);
				} else {
					*type = REG_NONE;
				}
			}

			closedir(dir);
			return ERROR_SUCCESS;
		}

		current++;
	}

	closedir(dir);
	return ERROR_NO_MORE_ITEMS;
}

/* ============================================================
 * advapi32.dll 스텁 함수 (ms_abi 래퍼)
 * ============================================================
 *
 * Win32 API 표면의 레지스트리 함수.
 * advapi32.dll에서 export되는 형태.
 */

__attribute__((ms_abi))
static uint32_t adv_RegOpenKeyExA(HKEY parent, const char *sub_key,
				  uint32_t options, uint32_t access,
				  HKEY *result)
{
	(void)options;
	return reg_open_key(parent, sub_key, access, result);
}

__attribute__((ms_abi))
static uint32_t adv_RegCreateKeyExA(HKEY parent, const char *sub_key,
				    uint32_t reserved, const char *class_name,
				    uint32_t options, uint32_t access,
				    void *security, HKEY *result,
				    uint32_t *disposition)
{
	(void)reserved;
	(void)class_name;
	(void)options;
	(void)security;
	return reg_create_key(parent, sub_key, access, result, disposition);
}

__attribute__((ms_abi))
static uint32_t adv_RegCloseKey(HKEY key)
{
	return reg_close_key(key);
}

__attribute__((ms_abi))
static uint32_t adv_RegQueryValueExA(HKEY key, const char *value_name,
				     uint32_t *reserved, uint32_t *type,
				     void *data, uint32_t *data_len)
{
	(void)reserved;
	return reg_query_value(key, value_name, type, data, data_len);
}

__attribute__((ms_abi))
static uint32_t adv_RegSetValueExA(HKEY key, const char *value_name,
				   uint32_t reserved, uint32_t type,
				   const void *data, uint32_t data_len)
{
	(void)reserved;
	return reg_set_value(key, value_name, type, data, data_len);
}

__attribute__((ms_abi))
static uint32_t adv_RegDeleteKeyA(HKEY parent, const char *sub_key)
{
	return reg_delete_key(parent, sub_key);
}

__attribute__((ms_abi))
static uint32_t adv_RegDeleteValueA(HKEY key, const char *value_name)
{
	return reg_delete_value(key, value_name);
}

__attribute__((ms_abi))
static uint32_t adv_RegEnumKeyExA(HKEY key, uint32_t index,
				  char *name, uint32_t *name_len,
				  uint32_t *reserved, char *class_name,
				  uint32_t *class_len, void *last_write)
{
	(void)reserved;
	(void)class_name;
	(void)class_len;
	(void)last_write;
	return reg_enum_key(key, index, name, name_len);
}

__attribute__((ms_abi))
static uint32_t adv_RegEnumValueA(HKEY key, uint32_t index,
				  char *name, uint32_t *name_len,
				  uint32_t *reserved, uint32_t *type,
				  void *data, uint32_t *data_len)
{
	(void)reserved;
	(void)data;
	(void)data_len;
	return reg_enum_value(key, index, name, name_len, type);
}

/* ============================================================
 * 보안 스텁 (advapi32.dll)
 * ============================================================
 *
 * 대부분의 앱이 GetUserName, OpenProcessToken 등을 호출.
 * 최소한의 성공 응답으로 크래시 방지.
 */

__attribute__((ms_abi))
static BOOL adv_GetUserNameA(char *lpBuffer, uint32_t *pcbBuffer)
{
	const char *name = "citcuser";
	uint32_t len = 9; /* strlen("citcuser") + 1 */

	if (!lpBuffer || !pcbBuffer || *pcbBuffer < len) {
		if (pcbBuffer)
			*pcbBuffer = len;
		return FALSE;
	}

	memcpy(lpBuffer, name, len);
	*pcbBuffer = len;
	return TRUE;
}

__attribute__((ms_abi))
static BOOL adv_OpenProcessToken(HANDLE hProcess, uint32_t dwDesiredAccess,
				 HANDLE *pTokenHandle)
{
	(void)hProcess;
	(void)dwDesiredAccess;

	/*
	 * 더미 토큰 핸들 반환 — 고정 값.
	 * 앱이 GetTokenInformation을 호출하면 최소한의 정보 반환.
	 */
	if (pTokenHandle)
		*pTokenHandle = (HANDLE)(uintptr_t)0xDEAD0001;
	return TRUE;
}

__attribute__((ms_abi))
static BOOL adv_GetTokenInformation(HANDLE hToken, int tokenInfoClass,
				    void *tokenInfo, uint32_t tokenInfoLen,
				    uint32_t *returnLen)
{
	(void)hToken;
	(void)tokenInfoClass;
	(void)tokenInfo;
	(void)tokenInfoLen;

	/* 최소 구현: 필요 크기만 반환, 실패 */
	if (returnLen)
		*returnLen = 0;
	return FALSE;
}

__attribute__((ms_abi))
static BOOL adv_LookupAccountSidA(const char *lpSystemName, void *Sid,
				   char *Name, uint32_t *cchName,
				   char *ReferencedDomainName,
				   uint32_t *cchReferencedDomainName,
				   int *peUse)
{
	(void)lpSystemName;
	(void)Sid;
	(void)peUse;

	/* 최소 구현: "citcuser" / "CITC" 반환 */
	if (Name && cchName && *cchName >= 9) {
		memcpy(Name, "citcuser", 9);
		*cchName = 8;
	}
	if (ReferencedDomainName && cchReferencedDomainName &&
	    *cchReferencedDomainName >= 5) {
		memcpy(ReferencedDomainName, "CITC", 5);
		*cchReferencedDomainName = 4;
	}
	return TRUE;
}

/* ============================================================
 * 서비스 스텁 (advapi32.dll)
 * ============================================================
 *
 * 서비스 관리자 API — 대부분의 앱이 설치 시 호출.
 * 실제 서비스 설치/시작은 지원하지 않으나,
 * 크래시 방지를 위해 적절한 에러코드 반환.
 */

__attribute__((ms_abi))
static HANDLE adv_OpenSCManagerA(const char *lpMachineName,
				 const char *lpDatabaseName,
				 uint32_t dwDesiredAccess)
{
	(void)lpMachineName;
	(void)lpDatabaseName;
	(void)dwDesiredAccess;

	/* 더미 SC 핸들 */
	return (HANDLE)(uintptr_t)0xDEAD0002;
}

__attribute__((ms_abi))
static HANDLE adv_OpenServiceA(HANDLE hSCManager, const char *lpServiceName,
			       uint32_t dwDesiredAccess)
{
	(void)hSCManager;
	(void)lpServiceName;
	(void)dwDesiredAccess;

	/* 서비스 존재하지 않음 */
	return NULL;
}

__attribute__((ms_abi))
static HANDLE adv_CreateServiceA(HANDLE hSCManager, const char *lpServiceName,
				 const char *lpDisplayName,
				 uint32_t dwDesiredAccess,
				 uint32_t dwServiceType,
				 uint32_t dwStartType,
				 uint32_t dwErrorControl,
				 const char *lpBinaryPathName,
				 const char *lpLoadOrderGroup,
				 uint32_t *lpdwTagId,
				 const char *lpDependencies,
				 const char *lpServiceStartName,
				 const char *lpPassword)
{
	(void)hSCManager; (void)lpServiceName; (void)lpDisplayName;
	(void)dwDesiredAccess; (void)dwServiceType; (void)dwStartType;
	(void)dwErrorControl; (void)lpBinaryPathName; (void)lpLoadOrderGroup;
	(void)lpdwTagId; (void)lpDependencies; (void)lpServiceStartName;
	(void)lpPassword;

	/* 접근 거부 — 서비스 설치 방지 */
	return NULL;
}

__attribute__((ms_abi))
static BOOL adv_StartServiceA(HANDLE hService, uint32_t dwNumServiceArgs,
			      const char **lpServiceArgVectors)
{
	(void)hService;
	(void)dwNumServiceArgs;
	(void)lpServiceArgVectors;
	return FALSE;
}

__attribute__((ms_abi))
static BOOL adv_CloseServiceHandle(HANDLE hSCObject)
{
	(void)hSCObject;
	return TRUE;
}

/* ============================================================
 * advapi32 스텁 테이블
 * ============================================================ */

struct stub_entry advapi32_stub_table[] = {
	/* 레지스트리 */
	{ "advapi32.dll", "RegOpenKeyExA",    (void *)adv_RegOpenKeyExA },
	{ "advapi32.dll", "RegCreateKeyExA",  (void *)adv_RegCreateKeyExA },
	{ "advapi32.dll", "RegCloseKey",      (void *)adv_RegCloseKey },
	{ "advapi32.dll", "RegQueryValueExA", (void *)adv_RegQueryValueExA },
	{ "advapi32.dll", "RegSetValueExA",   (void *)adv_RegSetValueExA },
	{ "advapi32.dll", "RegDeleteKeyA",    (void *)adv_RegDeleteKeyA },
	{ "advapi32.dll", "RegDeleteValueA",  (void *)adv_RegDeleteValueA },
	{ "advapi32.dll", "RegEnumKeyExA",    (void *)adv_RegEnumKeyExA },
	{ "advapi32.dll", "RegEnumValueA",    (void *)adv_RegEnumValueA },

	/* 보안 */
	{ "advapi32.dll", "GetUserNameA",       (void *)adv_GetUserNameA },
	{ "advapi32.dll", "OpenProcessToken",   (void *)adv_OpenProcessToken },
	{ "advapi32.dll", "GetTokenInformation",(void *)adv_GetTokenInformation },
	{ "advapi32.dll", "LookupAccountSidA",  (void *)adv_LookupAccountSidA },

	/* 서비스 */
	{ "advapi32.dll", "OpenSCManagerA",     (void *)adv_OpenSCManagerA },
	{ "advapi32.dll", "OpenServiceA",       (void *)adv_OpenServiceA },
	{ "advapi32.dll", "CreateServiceA",     (void *)adv_CreateServiceA },
	{ "advapi32.dll", "StartServiceA",      (void *)adv_StartServiceA },
	{ "advapi32.dll", "CloseServiceHandle", (void *)adv_CloseServiceHandle },

	{ NULL, NULL, NULL }
};
