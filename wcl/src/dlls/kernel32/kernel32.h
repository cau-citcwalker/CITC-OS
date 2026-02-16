/*
 * kernel32.h — kernel32.dll 스텁 인터페이스
 * ==========================================
 *
 * citcrun.c(PE 로더)와 kernel32.c(API 구현) 사이의 인터페이스.
 *
 * struct stub_entry:
 *   DLL 함수 이름 → C 함수 포인터 매핑.
 *   citcrun이 PE의 임포트 테이블을 해석할 때 이 테이블을 검색합니다.
 *
 *   .exe: call [IAT + offset]  →  IAT[n] = kernel32_stub_table[i].func_ptr
 */

#ifndef CITC_KERNEL32_H
#define CITC_KERNEL32_H

#include "../../../include/stub_entry.h"

/*
 * kernel32 서브시스템 초기화.
 * 핸들 테이블 초기화 + 콘솔 핸들(stdin/stdout/stderr) 미리 할당.
 * PE 임포트 해석 전에 반드시 호출해야 합니다.
 */
void kernel32_init(void);

/*
 * kernel32_set_cmdline — 명령줄 문자열 설정
 *
 * citcrun이 PE 실행 전에 호출하여 GetCommandLineA()의 반환값을 설정.
 */
void kernel32_set_cmdline(const char *cmdline);

/* kernel32.dll 스텁 테이블 (kernel32.c에서 정의) */
extern struct stub_entry kernel32_stub_table[];

#endif /* CITC_KERNEL32_H */
