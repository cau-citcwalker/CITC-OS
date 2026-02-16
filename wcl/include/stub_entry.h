/*
 * stub_entry.h — DLL 스텁 엔트리 공통 정의
 * =============================================
 *
 * 모든 DLL 구현 파일(kernel32, ntdll, advapi32 등)이
 * 공유하는 스텁 테이블 엔트리 구조체.
 *
 * citcrun의 import resolver가 이 테이블을 검색하여
 * PE 임포트를 해석합니다.
 */

#ifndef CITC_STUB_ENTRY_H
#define CITC_STUB_ENTRY_H

/* DLL 함수 이름 → 구현 함수 포인터 매핑 */
struct stub_entry {
	const char *dll_name;	/* "kernel32.dll" */
	const char *func_name;	/* "CreateFileA" 등 */
	void *func_ptr;		/* 실제 구현 함수의 주소 */
};

#endif /* CITC_STUB_ENTRY_H */
