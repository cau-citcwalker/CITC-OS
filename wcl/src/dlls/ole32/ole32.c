/*
 * ole32.c — CITC OS COM 런타임
 * ==============================
 *
 * 최소 COM 구현:
 *   - CoInitializeEx / CoUninitialize — per-thread 상태
 *   - CoCreateInstance — 내부 CLSID 레지스트리로 객체 생성
 *   - CoTaskMemAlloc/Free — COM 메모리 관리
 *   - GUID 유틸리티 — IsEqualGUID, StringFromGUID2
 *
 * 설계 결정:
 *   - Apartment 모델 무시 (STA/MTA 구분 없음)
 *   - 프로세스 내(in-proc) COM만 지원
 *   - 클래스 레지스트리는 정적 배열
 *   - DirectSound8, XAudio2를 등록하여 CoCreateInstance 경로 지원
 *
 * 실제 Windows COM과의 차이:
 *   Windows: RPCSS 서비스, apartment threading, proxy/stub, SCM
 *   우리:    단순 함수 테이블 검색 + 직접 생성
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ole32.h"

/* ============================================================
 * COM 초기화 상수
 * ============================================================ */

#define COINIT_MULTITHREADED     0x0
#define COINIT_APARTMENTTHREADED 0x2

#define CLSCTX_INPROC_SERVER  0x1
#define CLSCTX_LOCAL_SERVER   0x4
#define CLSCTX_ALL            (CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER)

/* ============================================================
 * 잘 알려진 CLSID / IID
 * ============================================================ */

/* CLSID_DirectSound8
 * {3901CC3F-84B5-4FA4-BA35-AA8172B8A09B} */
static const GUID CLSID_DirectSound8 = {
	0x3901CC3F, 0x84B5, 0x4FA4,
	{ 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B }
};

/* ============================================================
 * 내부 GUID 유틸리티
 * ============================================================ */

static int guid_equal(const GUID *a, const GUID *b)
{
	return memcmp(a, b, sizeof(GUID)) == 0;
}

/* ============================================================
 * COM 초기화 (per-thread)
 * ============================================================
 *
 * 실제 Windows: STA/MTA apartment 생성, 메시지 루프 연동.
 * 우리: 단순 플래그. 대부분의 앱은 초기화 여부만 체크.
 */

static pthread_key_t com_init_key;
static pthread_once_t com_key_once = PTHREAD_ONCE_INIT;

static void com_key_create(void)
{
	pthread_key_create(&com_init_key, NULL);
}

__attribute__((ms_abi))
static HRESULT ole_CoInitialize(void *pvReserved)
{
	(void)pvReserved;
	pthread_once(&com_key_once, com_key_create);

	if (pthread_getspecific(com_init_key)) {
		/* 이미 초기화됨 — S_FALSE (성공이지만 중복) */
		return S_FALSE;
	}

	pthread_setspecific(com_init_key, (void *)(uintptr_t)1);
	return S_OK;
}

__attribute__((ms_abi))
static HRESULT ole_CoInitializeEx(void *pvReserved, uint32_t dwCoInit)
{
	(void)dwCoInit;
	return ole_CoInitialize(pvReserved);
}

__attribute__((ms_abi))
static void ole_CoUninitialize(void)
{
	pthread_once(&com_key_once, com_key_create);
	pthread_setspecific(com_init_key, NULL);
}

/* ============================================================
 * 내부 클래스 레지스트리
 * ============================================================
 *
 * CoCreateInstance가 검색하는 CLSID → 생성함수 매핑.
 * 외부 DLL의 생성 함수를 여기서 직접 호출.
 */

/*
 * DirectSoundCreate8 래퍼.
 *
 * dsound.c의 DirectSoundCreate8(NULL, &ppv, NULL)을 호출하는
 * 간접 경로. dsound_stub_table에서 함수 포인터를 가져옴.
 */
typedef HRESULT (__attribute__((ms_abi)) *dsound_create_fn)(
	void *lpGuid, void **ppDS8, void *pUnkOuter);

/* 외부 참조 — dsound.c에서 정의된 스텁 테이블 */
extern struct stub_entry dsound_stub_table[];

static HRESULT create_directsound8(REFIID riid, void **ppv)
{
	(void)riid;

	/* dsound_stub_table에서 DirectSoundCreate8 찾기 */
	dsound_create_fn fn = NULL;

	for (int i = 0; dsound_stub_table[i].dll_name; i++) {
		if (strcmp(dsound_stub_table[i].func_name,
			   "DirectSoundCreate8") == 0) {
			fn = (dsound_create_fn)dsound_stub_table[i].func_ptr;
			break;
		}
	}

	if (!fn)
		return E_FAIL;

	return fn(NULL, ppv, NULL);
}

/* 클래스 레지스트리 */
struct com_class_entry {
	const GUID *clsid;
	HRESULT (*create_instance)(REFIID riid, void **ppv);
};

static struct com_class_entry com_registry[] = {
	{ &CLSID_DirectSound8, create_directsound8 },
	{ NULL, NULL }
};

/* ============================================================
 * CoCreateInstance — COM 객체 생성
 * ============================================================ */

__attribute__((ms_abi))
static HRESULT ole_CoCreateInstance(const GUID *rclsid, void *pUnkOuter,
				    uint32_t dwClsContext, const GUID *riid,
				    void **ppv)
{
	(void)dwClsContext;

	if (!rclsid || !ppv)
		return E_INVALIDARG;

	*ppv = NULL;

	/* aggregation 미지원 */
	if (pUnkOuter)
		return E_INVALIDARG;

	/* 내부 레지스트리 검색 */
	for (int i = 0; com_registry[i].clsid; i++) {
		if (guid_equal(rclsid, com_registry[i].clsid)) {
			return com_registry[i].create_instance(riid, ppv);
		}
	}

	printf("ole32: CoCreateInstance: unknown CLSID "
	       "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
	       rclsid->Data1, rclsid->Data2, rclsid->Data3,
	       rclsid->Data4[0], rclsid->Data4[1],
	       rclsid->Data4[2], rclsid->Data4[3],
	       rclsid->Data4[4], rclsid->Data4[5],
	       rclsid->Data4[6], rclsid->Data4[7]);

	return E_NOINTERFACE;
}

/* ============================================================
 * CoGetClassObject — IClassFactory 획득 (스텁)
 * ============================================================ */

__attribute__((ms_abi))
static HRESULT ole_CoGetClassObject(const GUID *rclsid, uint32_t dwClsContext,
				    void *pvReserved, const GUID *riid,
				    void **ppv)
{
	(void)rclsid; (void)dwClsContext; (void)pvReserved; (void)riid;

	if (ppv) *ppv = NULL;

	/* 직접 생성 방식이므로 ClassFactory 제공 안 함 */
	return E_NOINTERFACE;
}

/* ============================================================
 * CoTaskMem — COM 메모리 관리
 * ============================================================
 *
 * COM 호출 간 메모리 전달 시 사용되는 할당자.
 * 실제 Windows: OLE 힙 (별도 관리).
 * 우리: malloc/realloc/free 직접 래핑.
 */

__attribute__((ms_abi))
static void *ole_CoTaskMemAlloc(size_t cb)
{
	return malloc(cb);
}

__attribute__((ms_abi))
static void *ole_CoTaskMemRealloc(void *pv, size_t cb)
{
	return realloc(pv, cb);
}

__attribute__((ms_abi))
static void ole_CoTaskMemFree(void *pv)
{
	free(pv);
}

/* ============================================================
 * GUID 유틸리티
 * ============================================================ */

__attribute__((ms_abi))
static int ole_IsEqualGUID(const GUID *rguid1, const GUID *rguid2)
{
	if (!rguid1 || !rguid2)
		return 0;
	return memcmp(rguid1, rguid2, sizeof(GUID)) == 0;
}

/*
 * StringFromGUID2 — GUID → 와이드 문자열
 *
 * 형식: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
 * 와이드 문자열 (UTF-16LE) 반환 (38문자 + 널 = 39).
 *
 * cchMax: 와이드 문자 단위 (최소 39).
 * 반환: 성공 시 문자 수 (39), 실패 시 0.
 */
__attribute__((ms_abi))
static int ole_StringFromGUID2(const GUID *rguid, uint16_t *lpsz, int cchMax)
{
	if (!rguid || !lpsz || cchMax < 39)
		return 0;

	char buf[64];

	snprintf(buf, sizeof(buf),
		 "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		 rguid->Data1, rguid->Data2, rguid->Data3,
		 rguid->Data4[0], rguid->Data4[1],
		 rguid->Data4[2], rguid->Data4[3],
		 rguid->Data4[4], rguid->Data4[5],
		 rguid->Data4[6], rguid->Data4[7]);

	/* ASCII → UTF-16LE */
	int i;

	for (i = 0; buf[i] && i < cchMax - 1; i++)
		lpsz[i] = (uint16_t)(unsigned char)buf[i];
	lpsz[i] = 0;

	return i + 1;
}

/*
 * CLSIDFromString — 문자열 → GUID (ASCII 버전)
 *
 * 입력 형식: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
 * 또는       XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
 */

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static uint32_t parse_hex(const char *s, int digits)
{
	uint32_t val = 0;

	for (int i = 0; i < digits; i++) {
		int d = hex_digit(s[i]);

		if (d < 0)
			return 0;
		val = (val << 4) | (uint32_t)d;
	}
	return val;
}

__attribute__((ms_abi))
static HRESULT ole_CLSIDFromString(const uint16_t *lpsz, GUID *pclsid)
{
	if (!lpsz || !pclsid)
		return E_INVALIDARG;

	/* UTF-16 → ASCII (GUID는 ASCII만 사용) */
	char buf[64];
	int i;

	for (i = 0; i < 63 && lpsz[i]; i++)
		buf[i] = (char)(lpsz[i] & 0x7F);
	buf[i] = '\0';

	const char *p = buf;

	if (*p == '{')
		p++;

	/* XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX */
	if (strlen(p) < 36)
		return E_INVALIDARG;

	pclsid->Data1 = parse_hex(p, 8);      p += 8;
	if (*p == '-') p++;
	pclsid->Data2 = (uint16_t)parse_hex(p, 4); p += 4;
	if (*p == '-') p++;
	pclsid->Data3 = (uint16_t)parse_hex(p, 4); p += 4;
	if (*p == '-') p++;

	pclsid->Data4[0] = (uint8_t)parse_hex(p, 2); p += 2;
	pclsid->Data4[1] = (uint8_t)parse_hex(p, 2); p += 2;
	if (*p == '-') p++;

	for (int j = 2; j < 8; j++) {
		pclsid->Data4[j] = (uint8_t)parse_hex(p, 2);
		p += 2;
	}

	return S_OK;
}

/* ============================================================
 * ole32 스텁 테이블
 * ============================================================ */

struct stub_entry ole32_stub_table[] = {
	/* COM 초기화 */
	{ "ole32.dll", "CoInitialize",     (void *)ole_CoInitialize },
	{ "ole32.dll", "CoInitializeEx",   (void *)ole_CoInitializeEx },
	{ "ole32.dll", "CoUninitialize",   (void *)ole_CoUninitialize },

	/* 객체 생성 */
	{ "ole32.dll", "CoCreateInstance", (void *)ole_CoCreateInstance },
	{ "ole32.dll", "CoGetClassObject", (void *)ole_CoGetClassObject },

	/* 메모리 */
	{ "ole32.dll", "CoTaskMemAlloc",   (void *)ole_CoTaskMemAlloc },
	{ "ole32.dll", "CoTaskMemRealloc", (void *)ole_CoTaskMemRealloc },
	{ "ole32.dll", "CoTaskMemFree",    (void *)ole_CoTaskMemFree },

	/* GUID */
	{ "ole32.dll", "IsEqualGUID",      (void *)ole_IsEqualGUID },
	{ "ole32.dll", "StringFromGUID2",  (void *)ole_StringFromGUID2 },
	{ "ole32.dll", "CLSIDFromString",  (void *)ole_CLSIDFromString },

	{ NULL, NULL, NULL }
};
