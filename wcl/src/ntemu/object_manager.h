/*
 * object_manager.h - NT Object Manager (핸들 테이블)
 * ====================================================
 *
 * Windows NT 커널의 Object Manager는 모든 커널 객체를
 * 핸들(HANDLE)로 관리합니다.
 *
 * 실제 Windows 커널 (ntoskrnl.exe):
 *   ObCreateHandle()  — 핸들 할당
 *   ObReferenceObjectByHandle() — 핸들 → 객체 조회
 *   ObCloseHandle()   — 핸들 해제
 *
 * Linux 커널의 대응물:
 *   fd_install()      — fd 할당
 *   fdget()           — fd → file 구조체 조회
 *   close_fd()        — fd 해제
 *
 * 우리 구현:
 *   - 고정 크기 핸들 테이블 (MAX_HANDLES=256)
 *   - pthread_mutex로 스레드 안전성 보장
 *   - HANDLE = (인덱스 + HANDLE_OFFSET)을 void*로 캐스팅
 *   - 인덱스 0-2는 콘솔 핸들(stdin/stdout/stderr) 예약
 */

#ifndef CITC_OBJECT_MANAGER_H
#define CITC_OBJECT_MANAGER_H

#include <stdint.h>
#include "../../include/win32.h"

/* 핸들 테이블 크기 */
#define OB_MAX_HANDLES    256

/* HANDLE 값 오프셋 (0, 1, 2가 NULL/fd와 혼동되지 않도록) */
#define OB_HANDLE_OFFSET  0x100

/* 핸들 객체 타입 */
enum ob_type {
	OB_FREE = 0,        /* 빈 슬롯 */
	OB_FILE,            /* 일반 파일 */
	OB_CONSOLE,         /* 콘솔 (stdin/stdout/stderr) */
	OB_MUTEX,           /* Win32 뮤텍스 */
	OB_EVENT,           /* Win32 이벤트 */
	OB_THREAD,          /* Win32 스레드 */
	OB_REGISTRY_KEY,    /* 레지스트리 키 */
};

/* 핸들 테이블 엔트리 */
struct ob_entry {
	enum ob_type type;
	int fd;              /* Linux file descriptor (-1=미사용) */
	uint32_t access;     /* 접근 권한 */
	void *extra;         /* 타입별 추가 데이터 (레지스트리 등) */
};

/*
 * ob_init — Object Manager 초기화
 *
 * 핸들 테이블을 비우고, 콘솔 핸들(0,1,2)을 미리 할당.
 * 프로세스 시작 시 한 번 호출.
 */
void ob_init(void);

/*
 * ob_create_handle — 새 핸들 할당
 *
 * 빈 슬롯을 찾아 fd/type/access를 등록하고 HANDLE 반환.
 * 실패 시 INVALID_HANDLE_VALUE 반환.
 * 스레드 안전.
 */
HANDLE ob_create_handle(int fd, enum ob_type type, uint32_t access);

/*
 * ob_ref_handle — HANDLE → 엔트리 조회
 *
 * 유효하지 않은 핸들이면 NULL 반환.
 * 스레드 안전.
 */
struct ob_entry *ob_ref_handle(HANDLE h);

/*
 * ob_close_handle — 핸들 해제
 *
 * 슬롯을 OB_FREE로 설정. fd는 닫지 않음 (호출자 책임).
 * 스레드 안전.
 */
void ob_close_handle(HANDLE h);

/*
 * ob_create_handle_ex — extra 포인터 포함 핸들 할당
 *
 * 스레드/이벤트/뮤텍스 등 추가 데이터가 필요한 객체용.
 */
HANDLE ob_create_handle_ex(enum ob_type type, void *extra);

#endif /* CITC_OBJECT_MANAGER_H */
