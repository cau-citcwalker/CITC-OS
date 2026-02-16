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
 * 이 구현의 한계:
 *   - 열거(RegEnumKey/Value) 미구현
 *   - 보안/접근 제어 없음
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

/* ============================================================
 * advapi32 스텁 테이블
 * ============================================================ */

struct stub_entry advapi32_stub_table[] = {
	{ "advapi32.dll", "RegOpenKeyExA",    (void *)adv_RegOpenKeyExA },
	{ "advapi32.dll", "RegCreateKeyExA",  (void *)adv_RegCreateKeyExA },
	{ "advapi32.dll", "RegCloseKey",      (void *)adv_RegCloseKey },
	{ "advapi32.dll", "RegQueryValueExA", (void *)adv_RegQueryValueExA },
	{ "advapi32.dll", "RegSetValueExA",   (void *)adv_RegSetValueExA },

	{ NULL, NULL, NULL }
};
