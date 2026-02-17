/*
 * ole32.h — CITC OS ole32.dll (COM 런타임)
 * ==========================================
 *
 * COM (Component Object Model) 최소 런타임.
 *
 * 실제 Windows COM은 apartment model, proxy/stub 마샬링,
 * ORPC 프로토콜 등 매우 복잡하지만,
 * 우리는 in-process COM만 지원 (CLSCTX_INPROC_SERVER).
 *
 * 구현:
 *   CoInitializeEx     — per-thread 초기화 플래그 설정
 *   CoCreateInstance    — 내부 클래스 레지스트리에서 CLSID 검색
 *   CoTaskMemAlloc/Free — malloc/free 래퍼
 *   IsEqualGUID         — GUID 비교
 */

#ifndef CITC_OLE32_H
#define CITC_OLE32_H

#include "../../../include/win32.h"
#include "../../../include/stub_entry.h"

/* 스텁 테이블 (citcrun이 참조) */
extern struct stub_entry ole32_stub_table[];

/*
 * COM 클래스 등록 (내부 API)
 *
 * DLL 초기화 시 호출하여 CoCreateInstance에서 검색 가능하도록.
 * 현재는 컴파일 시 정적 등록.
 */

#endif /* CITC_OLE32_H */
