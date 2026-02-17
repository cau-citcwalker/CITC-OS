/*
 * ws2_32.h — CITC OS ws2_32.dll (Winsock2)
 * ==========================================
 *
 * Windows 소켓 API → POSIX 소켓 매핑.
 *
 * Winsock과 POSIX 소켓의 차이:
 *   - SOCKET: Windows=UINT_PTR(unsigned), POSIX=int(signed)
 *   - 초기화: Windows=WSAStartup 필수, POSIX=불필요
 *   - 에러: Windows=WSAGetLastError, POSIX=errno
 *   - 닫기: Windows=closesocket, POSIX=close
 *   - 나머지 API는 거의 동일
 *
 * 구현 전략: 내부적으로 SOCKET=int(fd), 1:1 매핑.
 */

#ifndef CITC_WS2_32_H
#define CITC_WS2_32_H

#include "../../../include/win32.h"
#include "../../../include/stub_entry.h"

/* 스텁 테이블 (citcrun이 참조) */
extern struct stub_entry ws2_32_stub_table[];

#endif /* CITC_WS2_32_H */
