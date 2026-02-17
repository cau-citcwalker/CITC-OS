/*
 * object_manager.c - NT Object Manager 구현
 * ============================================
 *
 * Windows NT 커널의 핸들 테이블을 교육적으로 구현.
 *
 * 실제 Windows와의 차이:
 *   Windows: 프로세스별 핸들 테이블이 커널 공간에 존재
 *            핸들 = 인덱스 × 4, 참조 카운팅, 상속 지원
 *   우리:    단일 전역 배열, 단순 인덱스 + 오프셋, mutex 보호
 *
 * 스레드 안전성:
 *   pthread_mutex로 핸들 테이블 접근을 직렬화.
 *   Windows에서도 핸들 할당/해제는 커널 잠금 하에 수행됨.
 *   (ExAcquireResourceExclusiveLite 등)
 */

#include <string.h>
#include <pthread.h>

#include "object_manager.h"

/* 핸들 테이블 전역 상태 */
static struct ob_entry handle_table[OB_MAX_HANDLES];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * ob_init — 초기화
 * ============================================================ */
void ob_init(void)
{
	pthread_mutex_lock(&table_lock);
	memset(handle_table, 0, sizeof(handle_table));

	/*
	 * 콘솔 핸들 미리 할당 (인덱스 0, 1, 2)
	 *
	 * GetStdHandle(STD_INPUT_HANDLE)  → HANDLE 0x100 → 인덱스 0 → fd 0
	 * GetStdHandle(STD_OUTPUT_HANDLE) → HANDLE 0x101 → 인덱스 1 → fd 1
	 * GetStdHandle(STD_ERROR_HANDLE)  → HANDLE 0x102 → 인덱스 2 → fd 2
	 */
	handle_table[0] = (struct ob_entry){
		.type = OB_CONSOLE, .fd = 0, .access = GENERIC_READ
	};
	handle_table[1] = (struct ob_entry){
		.type = OB_CONSOLE, .fd = 1, .access = GENERIC_WRITE
	};
	handle_table[2] = (struct ob_entry){
		.type = OB_CONSOLE, .fd = 2, .access = GENERIC_WRITE
	};

	pthread_mutex_unlock(&table_lock);
}

/* ============================================================
 * ob_create_handle — 핸들 할당
 * ============================================================ */
HANDLE ob_create_handle(int fd, enum ob_type type, uint32_t access)
{
	HANDLE result = INVALID_HANDLE_VALUE;

	pthread_mutex_lock(&table_lock);

	/* 인덱스 3부터 검색 (0-2는 콘솔 핸들 예약) */
	for (int i = 3; i < OB_MAX_HANDLES; i++) {
		if (handle_table[i].type == OB_FREE) {
			handle_table[i].type = type;
			handle_table[i].fd = fd;
			handle_table[i].access = access;
			handle_table[i].extra = NULL;
			result = (HANDLE)(uintptr_t)(i + OB_HANDLE_OFFSET);
			break;
		}
	}

	pthread_mutex_unlock(&table_lock);
	return result;
}

/* ============================================================
 * ob_ref_handle — 핸들 조회
 * ============================================================ */
struct ob_entry *ob_ref_handle(HANDLE h)
{
	uintptr_t val = (uintptr_t)h;

	if (val < OB_HANDLE_OFFSET)
		return NULL;

	int idx = (int)(val - OB_HANDLE_OFFSET);

	if (idx < 0 || idx >= OB_MAX_HANDLES)
		return NULL;

	/*
	 * 읽기 전용 조회이므로 mutex를 잡지 않음.
	 * type이 OB_FREE인지만 확인하면 충분.
	 * (실제로는 읽기 잠금이 더 안전하지만, 단순화)
	 */
	if (handle_table[idx].type == OB_FREE)
		return NULL;

	return &handle_table[idx];
}

/* ============================================================
 * ob_create_handle_ex — extra 포인터 포함 핸들 할당
 * ============================================================ */
HANDLE ob_create_handle_ex(enum ob_type type, void *extra)
{
	HANDLE result = INVALID_HANDLE_VALUE;

	pthread_mutex_lock(&table_lock);

	for (int i = 3; i < OB_MAX_HANDLES; i++) {
		if (handle_table[i].type == OB_FREE) {
			handle_table[i].type = type;
			handle_table[i].fd = -1;
			handle_table[i].access = 0;
			handle_table[i].extra = extra;
			result = (HANDLE)(uintptr_t)(i + OB_HANDLE_OFFSET);
			break;
		}
	}

	pthread_mutex_unlock(&table_lock);
	return result;
}

/* ============================================================
 * ob_close_handle — 핸들 해제
 * ============================================================ */
void ob_close_handle(HANDLE h)
{
	uintptr_t val = (uintptr_t)h;

	if (val < OB_HANDLE_OFFSET)
		return;

	int idx = (int)(val - OB_HANDLE_OFFSET);

	if (idx < 0 || idx >= OB_MAX_HANDLES)
		return;

	pthread_mutex_lock(&table_lock);
	handle_table[idx].type = OB_FREE;
	handle_table[idx].fd = -1;
	handle_table[idx].extra = NULL;
	pthread_mutex_unlock(&table_lock);
}
