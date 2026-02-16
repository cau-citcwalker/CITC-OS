/*
 * registry.h - Windows 레지스트리 v0.1
 * =======================================
 *
 * Windows의 레지스트리는 계층적 key-value 저장소입니다.
 *
 * 구조:
 *   HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\...
 *   ↑ 루트 키(HIVE)    ↑ 서브키 경로
 *
 * 주요 루트 키 (HIVE):
 *   HKLM  (HKEY_LOCAL_MACHINE)  — 시스템 전역 설정
 *   HKCU  (HKEY_CURRENT_USER)   — 현재 사용자 설정
 *   HKCR  (HKEY_CLASSES_ROOT)   — 파일 연결, COM 클래스
 *
 * 값 타입:
 *   REG_SZ       — 널 종료 문자열
 *   REG_DWORD    — 32비트 정수
 *   REG_BINARY   — 바이너리 데이터
 *   REG_EXPAND_SZ — 환경변수 확장 문자열
 *
 * 우리 구현:
 *   파일 시스템 기반 (디렉토리 = 키, 파일 = 값)
 *
 *   HKLM\SOFTWARE\Test  →  /etc/citc-registry/HKLM/SOFTWARE/Test/
 *   값 "Version" = "1.0" →  /etc/citc-registry/HKLM/SOFTWARE/Test/Version
 *
 *   파일 포맷 (값 파일):
 *     바이트 0-3:   타입 (REG_SZ=1, REG_DWORD=4 등)
 *     바이트 4-7:   데이터 길이
 *     바이트 8+:    실제 데이터
 *
 * 실제 Windows 레지스트리는 바이너리 하이브 파일이지만,
 * v0.1에서는 디버깅이 쉽도록 파일 시스템 기반으로 구현.
 */

#ifndef CITC_REGISTRY_H
#define CITC_REGISTRY_H

#include <stdint.h>
#include "../../include/win32.h"

/* ============================================================
 * 레지스트리 타입과 상수
 * ============================================================ */

/* HKEY는 HANDLE과 동일 (레지스트리 키 핸들) */
typedef HANDLE HKEY;

/* 미리 정의된 루트 키 핸들 (실제 Windows 값) */
#define HKEY_CLASSES_ROOT     ((HKEY)(uintptr_t)0x80000000)
#define HKEY_CURRENT_USER     ((HKEY)(uintptr_t)0x80000001)
#define HKEY_LOCAL_MACHINE    ((HKEY)(uintptr_t)0x80000002)
#define HKEY_USERS            ((HKEY)(uintptr_t)0x80000003)

/* 값 타입 */
#define REG_NONE       0   /* 타입 없음 */
#define REG_SZ         1   /* 널 종료 문자열 */
#define REG_EXPAND_SZ  2   /* 환경변수 확장 문자열 */
#define REG_BINARY     3   /* 바이너리 데이터 */
#define REG_DWORD      4   /* 32비트 정수 (리틀 엔디안) */

/* 키 접근 권한 */
#define KEY_READ       0x20019
#define KEY_WRITE      0x20006
#define KEY_ALL_ACCESS 0xF003F

/* 키 생성 결과 (RegCreateKeyEx) */
#define REG_CREATED_NEW_KEY     1
#define REG_OPENED_EXISTING_KEY 2

/* 에러 코드 (레지스트리 전용) */
#define ERROR_MORE_DATA     234
#define ERROR_NO_MORE_ITEMS 259

/* 레지스트리 기본 경로 (런타임에 결정) */
#define REGISTRY_DEFAULT_PATH "/etc/citc-registry"
const char *reg_get_base_path(void);

/* 경로 최대 길이 */
#define REG_MAX_PATH 1024

/* 값 파일 헤더 */
struct reg_value_header {
	uint32_t type;      /* REG_SZ, REG_DWORD 등 */
	uint32_t data_len;  /* 데이터 바이트 수 */
	/* 이후 data_len 바이트의 실제 데이터 */
};

/* ============================================================
 * 레지스트리 함수 (advapi32.dll API)
 * ============================================================ */

/*
 * reg_init — 레지스트리 서브시스템 초기화
 *
 * 기본 디렉토리 구조 생성 (HKLM, HKCU 등).
 */
void reg_init(void);

/*
 * RegOpenKeyExA — 레지스트리 키 열기
 *
 * 실제 Windows: hKey = 부모 키, lpSubKey = 하위 경로
 * 우리 구현: 디렉토리 존재 확인 + HANDLE 할당
 *
 * 반환: ERROR_SUCCESS 또는 에러 코드
 */
uint32_t reg_open_key(HKEY parent, const char *sub_key,
		      uint32_t access, HKEY *result);

/*
 * RegCreateKeyExA — 레지스트리 키 생성/열기
 *
 * 키가 없으면 생성, 있으면 열기.
 *
 * disposition: REG_CREATED_NEW_KEY 또는 REG_OPENED_EXISTING_KEY
 */
uint32_t reg_create_key(HKEY parent, const char *sub_key,
			uint32_t access, HKEY *result,
			uint32_t *disposition);

/*
 * RegCloseKey — 레지스트리 키 닫기
 */
uint32_t reg_close_key(HKEY key);

/*
 * RegQueryValueExA — 레지스트리 값 읽기
 *
 * value_name: 값 이름 (NULL이면 기본값 "(Default)")
 * type: [out] 값 타입 (REG_SZ 등, NULL 가능)
 * data: [out] 데이터 버퍼 (NULL이면 크기만 반환)
 * data_len: [in/out] 버퍼 크기 / 실제 크기
 */
uint32_t reg_query_value(HKEY key, const char *value_name,
			 uint32_t *type, void *data,
			 uint32_t *data_len);

/*
 * RegSetValueExA — 레지스트리 값 쓰기
 *
 * value_name: 값 이름
 * type: 값 타입 (REG_SZ, REG_DWORD 등)
 * data: 데이터
 * data_len: 데이터 바이트 수
 */
uint32_t reg_set_value(HKEY key, const char *value_name,
		       uint32_t type, const void *data,
		       uint32_t data_len);

/* ============================================================
 * 스텁 테이블
 * ============================================================ */
#include "../../include/stub_entry.h"
extern struct stub_entry advapi32_stub_table[];

#endif /* CITC_REGISTRY_H */
