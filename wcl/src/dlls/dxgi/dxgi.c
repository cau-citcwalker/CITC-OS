/*
 * dxgi.c — CITC OS dxgi.dll 구현
 * ================================
 *
 * DXGI (DirectX Graphics Infrastructure).
 *
 * DXGI는 DirectX의 디스플레이 관리 계층:
 *   앱 → CreateDXGIFactory() → IDXGIFactory
 *     → EnumAdapters() → IDXGIAdapter (GPU 정보)
 *     → CreateSwapChain() → IDXGISwapChain (화면 출력)
 *     → Present() → 백버퍼를 윈도우에 복사
 *
 * COM vtable 패턴:
 *   각 인터페이스는 구조체의 첫 번째 멤버가 vtable 포인터.
 *   앱은 pObj->lpVtbl->Method(pObj, ...) 로 호출.
 *   vtable은 전역 static — 모든 인스턴스가 공유.
 *
 * 소프트웨어 렌더링 모드:
 *   SwapChain의 백버퍼 = malloc'd XRGB8888 배열.
 *   Present() = 백버퍼 → HWND 픽셀 버퍼 memcpy + CDP commit.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../../../include/win32.h"
#include "../../../include/d3d11_types.h"
#include "../../../include/stub_entry.h"
#include "../user32/user32.h"
#include "../d3d11/d3d11.h"

/* ============================================================
 * IDXGIAdapter 구현
 * ============================================================ */

struct dxgi_adapter {
	IDXGIAdapterVtbl *lpVtbl;
	ULONG ref_count;
};

static HRESULT __attribute__((ms_abi))
adapter_QueryInterface(void *This, REFIID riid, void **ppv)
{
	(void)riid;
	if (!ppv) return E_POINTER;
	*ppv = This;
	return S_OK;
}

static ULONG __attribute__((ms_abi))
adapter_AddRef(void *This)
{
	struct dxgi_adapter *a = This;
	return ++a->ref_count;
}

static ULONG __attribute__((ms_abi))
adapter_Release(void *This)
{
	struct dxgi_adapter *a = This;
	ULONG r = --a->ref_count;
	if (r == 0)
		free(a);
	return r;
}

/* IDXGIObject stubs */
static HRESULT __attribute__((ms_abi))
adapter_SetPrivateData(void *T, REFIID n, UINT s, const void *d)
{ (void)T; (void)n; (void)s; (void)d; return S_OK; }
static HRESULT __attribute__((ms_abi))
adapter_GetPrivateData(void *T, REFIID n, UINT *s, void *d)
{ (void)T; (void)n; (void)s; (void)d; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
adapter_GetParent(void *T, REFIID r, void **pp)
{ (void)T; (void)r; (void)pp; return E_FAIL; }

/* IDXGIAdapter::EnumOutputs — 모니터 열거 (더미) */
static HRESULT __attribute__((ms_abi))
adapter_EnumOutputs(void *This, UINT Output, void **ppOutput)
{
	(void)This; (void)Output; (void)ppOutput;
	/* 출력 없음 — DXGI_ERROR_NOT_FOUND */
	return ((HRESULT)0x887A0002);
}

/* IDXGIAdapter::GetDesc — GPU 정보 반환 */
static HRESULT __attribute__((ms_abi))
adapter_GetDesc(void *This, DXGI_ADAPTER_DESC *pDesc)
{
	(void)This;
	if (!pDesc) return E_POINTER;

	memset(pDesc, 0, sizeof(*pDesc));

	/* "CITC Software Adapter" in UTF-16LE */
	const char *name = "CITC Software Adapter";
	for (int i = 0; name[i] && i < 127; i++)
		pDesc->Description[i] = (uint16_t)name[i];

	pDesc->VendorId = 0xCCCC;       /* 가짜 벤더 ID */
	pDesc->DeviceId = 0x0001;
	pDesc->DedicatedVideoMemory = 256 * 1024 * 1024;  /* 256 MB */
	pDesc->SharedSystemMemory = 512 * 1024 * 1024;

	return S_OK;
}

/* IDXGIAdapter::CheckInterfaceSupport */
static HRESULT __attribute__((ms_abi))
adapter_CheckInterfaceSupport(void *T, REFIID r, void *v)
{ (void)T; (void)r; (void)v; return S_OK; }

static IDXGIAdapterVtbl g_adapter_vtbl = {
	.QueryInterface         = adapter_QueryInterface,
	.AddRef                 = adapter_AddRef,
	.Release                = adapter_Release,
	.SetPrivateData         = adapter_SetPrivateData,
	.GetPrivateData         = adapter_GetPrivateData,
	.GetParent              = adapter_GetParent,
	.EnumOutputs            = adapter_EnumOutputs,
	.GetDesc                = adapter_GetDesc,
	.CheckInterfaceSupport  = adapter_CheckInterfaceSupport,
};

/* ============================================================
 * IDXGISwapChain 구현
 * ============================================================ */

struct dxgi_swap_chain {
	IDXGISwapChainVtbl *lpVtbl;
	ULONG ref_count;

	HWND output_window;
	UINT width, height;
	DXGI_FORMAT format;
	uint32_t *backbuffer;   /* malloc'd XRGB8888 픽셀 배열 */

	/* d3d11 리소스 테이블 연동 */
	int resource_idx;       /* -1 = 미연동 */

	DXGI_SWAP_CHAIN_DESC desc; /* 원본 설명 보관 */
};

static HRESULT __attribute__((ms_abi))
sc_QueryInterface(void *This, REFIID riid, void **ppv)
{
	(void)riid;
	if (!ppv) return E_POINTER;
	*ppv = This;
	return S_OK;
}

static ULONG __attribute__((ms_abi))
sc_AddRef(void *This)
{
	struct dxgi_swap_chain *sc = This;
	return ++sc->ref_count;
}

static ULONG __attribute__((ms_abi))
sc_Release(void *This)
{
	struct dxgi_swap_chain *sc = This;
	ULONG r = --sc->ref_count;
	if (r == 0) {
		free(sc->backbuffer);
		free(sc);
	}
	return r;
}

/* IDXGIObject stubs */
static HRESULT __attribute__((ms_abi))
sc_SetPrivateData(void *T, REFIID n, UINT s, const void *d)
{ (void)T; (void)n; (void)s; (void)d; return S_OK; }
static HRESULT __attribute__((ms_abi))
sc_GetPrivateData(void *T, REFIID n, UINT *s, void *d)
{ (void)T; (void)n; (void)s; (void)d; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
sc_GetParent(void *T, REFIID r, void **pp)
{ (void)T; (void)r; (void)pp; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
sc_GetDevice(void *T, REFIID r, void **pp)
{ (void)T; (void)r; (void)pp; return E_FAIL; }

/*
 * IDXGISwapChain::Present — 백버퍼를 윈도우에 복사
 *
 * 핵심 경로:
 *   1. backbuffer → HWND의 pixel buffer로 memcpy
 *   2. user32_commit_window() → CDP commit (컴포지터에 알림)
 */
static HRESULT __attribute__((ms_abi))
sc_Present(void *This, UINT SyncInterval, UINT Flags)
{
	struct dxgi_swap_chain *sc = This;
	(void)SyncInterval; (void)Flags;

	uint32_t *wnd_pixels = NULL;
	int wnd_w = 0, wnd_h = 0;

	if (user32_get_window_pixels(sc->output_window, &wnd_pixels,
				     &wnd_w, &wnd_h) < 0)
		return E_FAIL;

	/* 백버퍼 → 윈도우 픽셀 버퍼 복사 */
	int copy_w = (int)sc->width < wnd_w ? (int)sc->width : wnd_w;
	int copy_h = (int)sc->height < wnd_h ? (int)sc->height : wnd_h;

	for (int y = 0; y < copy_h; y++) {
		memcpy(&wnd_pixels[y * wnd_w],
		       &sc->backbuffer[y * sc->width],
		       (size_t)copy_w * 4);
	}

	/* CDP commit (컴포지터에 프레임 전달) */
	user32_commit_window(sc->output_window);

	return S_OK;
}

/*
 * IDXGISwapChain::GetBuffer — 백버퍼 텍스처 핸들 반환
 *
 * D3D11에서 SwapChain의 렌더 타깃으로 사용하기 위해
 * 백버퍼를 ID3D11Texture2D로 받아오는 함수.
 *
 * 우리 구현에서는 SwapChain 포인터를 그대로 반환하고,
 * d3d11.c의 CreateRenderTargetView에서 특별 처리.
 */
static HRESULT __attribute__((ms_abi))
sc_GetBuffer(void *This, UINT Buffer, REFIID riid, void **ppSurface)
{
	(void)Buffer; (void)riid;
	if (!ppSurface) return E_POINTER;

	/*
	 * 실제 Windows에서는 ID3D11Texture2D를 반환하지만,
	 * 우리는 SwapChain 자체를 반환하고 d3d11에서 인식.
	 * d3d11_create_rtv_for_swapchain()이 이를 처리.
	 */
	*ppSurface = This;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
sc_SetFullscreenState(void *T, BOOL f, void *t)
{ (void)T; (void)f; (void)t; return S_OK; }
static HRESULT __attribute__((ms_abi))
sc_GetFullscreenState(void *T, BOOL *f, void **t)
{
	(void)T;
	if (f) *f = FALSE;
	if (t) *t = NULL;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
sc_GetDesc(void *This, DXGI_SWAP_CHAIN_DESC *pDesc)
{
	struct dxgi_swap_chain *sc = This;
	if (!pDesc) return E_POINTER;
	*pDesc = sc->desc;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
sc_ResizeBuffers(void *This, UINT BufferCount, UINT Width, UINT Height,
		 DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	struct dxgi_swap_chain *sc = This;
	(void)BufferCount; (void)NewFormat; (void)SwapChainFlags;

	if (Width == 0 || Height == 0)
		return E_INVALIDARG;

	free(sc->backbuffer);
	sc->width = Width;
	sc->height = Height;
	sc->backbuffer = calloc((size_t)Width * Height, sizeof(uint32_t));
	if (!sc->backbuffer)
		return E_OUTOFMEMORY;

	sc->desc.BufferDesc.Width = Width;
	sc->desc.BufferDesc.Height = Height;

	return S_OK;
}

static HRESULT __attribute__((ms_abi))
sc_ResizeTarget(void *T, const DXGI_MODE_DESC *d)
{ (void)T; (void)d; return S_OK; }

static IDXGISwapChainVtbl g_swap_chain_vtbl = {
	.QueryInterface      = sc_QueryInterface,
	.AddRef              = sc_AddRef,
	.Release             = sc_Release,
	.SetPrivateData      = sc_SetPrivateData,
	.GetPrivateData      = sc_GetPrivateData,
	.GetParent           = sc_GetParent,
	.GetDevice           = sc_GetDevice,
	.Present             = sc_Present,
	.GetBuffer           = sc_GetBuffer,
	.SetFullscreenState  = sc_SetFullscreenState,
	.GetFullscreenState  = sc_GetFullscreenState,
	.GetDesc             = sc_GetDesc,
	.ResizeBuffers       = sc_ResizeBuffers,
	.ResizeTarget        = sc_ResizeTarget,
};

/* ============================================================
 * IDXGIFactory 구현
 * ============================================================ */

struct dxgi_factory {
	IDXGIFactoryVtbl *lpVtbl;
	ULONG ref_count;
};

static HRESULT __attribute__((ms_abi))
factory_QueryInterface(void *This, REFIID riid, void **ppv)
{
	(void)riid;
	if (!ppv) return E_POINTER;
	*ppv = This;
	return S_OK;
}

static ULONG __attribute__((ms_abi))
factory_AddRef(void *This)
{
	struct dxgi_factory *f = This;
	return ++f->ref_count;
}

static ULONG __attribute__((ms_abi))
factory_Release(void *This)
{
	struct dxgi_factory *f = This;
	ULONG r = --f->ref_count;
	if (r == 0)
		free(f);
	return r;
}

/* IDXGIObject stubs */
static HRESULT __attribute__((ms_abi))
factory_SetPrivateData(void *T, REFIID n, UINT s, const void *d)
{ (void)T; (void)n; (void)s; (void)d; return S_OK; }
static HRESULT __attribute__((ms_abi))
factory_GetPrivateData(void *T, REFIID n, UINT *s, void *d)
{ (void)T; (void)n; (void)s; (void)d; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
factory_GetParent(void *T, REFIID r, void **pp)
{ (void)T; (void)r; (void)pp; return E_FAIL; }

/*
 * IDXGIFactory::EnumAdapters — GPU 열거
 *
 * index 0 = "CITC Software Adapter" (소프트웨어 렌더러)
 * index 1+ = DXGI_ERROR_NOT_FOUND
 */
static HRESULT __attribute__((ms_abi))
factory_EnumAdapters(void *This, UINT Adapter, void **ppAdapter)
{
	(void)This;
	if (!ppAdapter) return E_POINTER;

	if (Adapter > 0) {
		*ppAdapter = NULL;
		return ((HRESULT)0x887A0002); /* DXGI_ERROR_NOT_FOUND */
	}

	struct dxgi_adapter *a = calloc(1, sizeof(*a));
	if (!a) return E_OUTOFMEMORY;

	a->lpVtbl = &g_adapter_vtbl;
	a->ref_count = 1;
	*ppAdapter = a;

	return S_OK;
}

static HRESULT __attribute__((ms_abi))
factory_MakeWindowAssociation(void *T, HWND h, UINT f)
{ (void)T; (void)h; (void)f; return S_OK; }
static HRESULT __attribute__((ms_abi))
factory_GetWindowAssociation(void *T, HWND *h)
{ (void)T; if (h) *h = NULL; return S_OK; }

/*
 * IDXGIFactory::CreateSwapChain — 스왑 체인 생성
 *
 * HWND의 크기에 맞춰 백버퍼(XRGB8888)를 할당.
 * Present()에서 이 백버퍼를 윈도우 픽셀 버퍼로 복사.
 */
static HRESULT __attribute__((ms_abi))
factory_CreateSwapChain(void *This, void *pDevice,
			DXGI_SWAP_CHAIN_DESC *pDesc, void **ppSwapChain)
{
	(void)This; (void)pDevice;
	if (!pDesc || !ppSwapChain) return E_POINTER;

	UINT w = pDesc->BufferDesc.Width;
	UINT h = pDesc->BufferDesc.Height;

	/* 크기가 0이면 윈도우 크기 사용 */
	if (w == 0 || h == 0) {
		uint32_t *wnd_pix = NULL;
		int ww = 0, wh = 0;
		if (user32_get_window_pixels(pDesc->OutputWindow,
					     &wnd_pix, &ww, &wh) == 0) {
			if (w == 0) w = (UINT)ww;
			if (h == 0) h = (UINT)wh;
		}
		if (w == 0) w = 640;
		if (h == 0) h = 480;
	}

	struct dxgi_swap_chain *sc = calloc(1, sizeof(*sc));
	if (!sc) return E_OUTOFMEMORY;

	sc->lpVtbl = &g_swap_chain_vtbl;
	sc->ref_count = 1;
	sc->output_window = pDesc->OutputWindow;
	sc->width = w;
	sc->height = h;
	sc->format = pDesc->BufferDesc.Format;
	sc->resource_idx = -1;
	sc->desc = *pDesc;
	sc->desc.BufferDesc.Width = w;
	sc->desc.BufferDesc.Height = h;

	sc->backbuffer = calloc((size_t)w * h, sizeof(uint32_t));
	if (!sc->backbuffer) {
		free(sc);
		return E_OUTOFMEMORY;
	}

	printf("dxgi: SwapChain created (%ux%u, HWND=%p)\n",
	       w, h, pDesc->OutputWindow);

	*ppSwapChain = sc;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
factory_CreateSoftwareAdapter(void *T, void *m, void **pp)
{ (void)T; (void)m; (void)pp; return E_FAIL; }

static IDXGIFactoryVtbl g_factory_vtbl = {
	.QueryInterface        = factory_QueryInterface,
	.AddRef                = factory_AddRef,
	.Release               = factory_Release,
	.SetPrivateData        = factory_SetPrivateData,
	.GetPrivateData        = factory_GetPrivateData,
	.GetParent             = factory_GetParent,
	.EnumAdapters          = factory_EnumAdapters,
	.MakeWindowAssociation = factory_MakeWindowAssociation,
	.GetWindowAssociation  = factory_GetWindowAssociation,
	.CreateSwapChain       = factory_CreateSwapChain,
	.CreateSoftwareAdapter = factory_CreateSoftwareAdapter,
};

/* ============================================================
 * DLL 엔트리: CreateDXGIFactory
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
dxgi_CreateDXGIFactory(REFIID riid, void **ppFactory)
{
	(void)riid;
	if (!ppFactory) return E_POINTER;

	struct dxgi_factory *f = calloc(1, sizeof(*f));
	if (!f) return E_OUTOFMEMORY;

	f->lpVtbl = &g_factory_vtbl;
	f->ref_count = 1;

	printf("dxgi: Factory created\n");
	*ppFactory = f;
	return S_OK;
}

/* ============================================================
 * 내부 API (d3d11.c에서 사용)
 * ============================================================ */

int dxgi_get_swapchain_backbuffer(void *pSwapChain, uint32_t **pixels,
				  int *width, int *height)
{
	struct dxgi_swap_chain *sc = pSwapChain;
	if (!sc || !sc->backbuffer) return -1;
	if (pixels) *pixels = sc->backbuffer;
	if (width)  *width = (int)sc->width;
	if (height) *height = (int)sc->height;
	return 0;
}

void dxgi_set_swapchain_resource(void *pSwapChain, int resource_idx)
{
	struct dxgi_swap_chain *sc = pSwapChain;
	if (sc) sc->resource_idx = resource_idx;
}

int dxgi_get_swapchain_resource_idx(void *pSwapChain)
{
	struct dxgi_swap_chain *sc = pSwapChain;
	if (!sc || !sc->backbuffer) return -1;
	return sc->resource_idx;
}

/*
 * 내부 API — D3D11CreateDeviceAndSwapChain에서 호출
 *
 * ms_abi 함수와의 ABI 불일치 없이 SwapChain을 생성.
 * (ms_abi 함수에서 ms_abi 함수를 함수 포인터로 호출하면
 *  GCC가 ABI를 혼동할 수 있으므로, 일반 C 함수로 래핑.)
 */
HRESULT dxgi_create_swapchain_for_d3d11(void *pDevice,
					DXGI_SWAP_CHAIN_DESC *pDesc,
					void **ppSwapChain)
{
	if (!pDesc || !ppSwapChain) return E_POINTER;

	/* Factory 생성 (내부적으로) */
	struct dxgi_factory *f = calloc(1, sizeof(*f));
	if (!f) return E_OUTOFMEMORY;
	f->lpVtbl = &g_factory_vtbl;
	f->ref_count = 1;

	/* SwapChain 생성 */
	HRESULT hr = factory_CreateSwapChain(f, pDevice, pDesc, ppSwapChain);

	/* Factory 해제 */
	free(f);

	/* Vulkan 렌더 타깃 생성 (가능하면) */
	if (hr == S_OK)
		d3d11_vk_create_rt((int)pDesc->BufferDesc.Width,
				   (int)pDesc->BufferDesc.Height);

	return hr;
}

/* ============================================================
 * 스텁 테이블
 * ============================================================ */

struct stub_entry dxgi_stub_table[] = {
	{ "dxgi.dll", "CreateDXGIFactory", (void *)dxgi_CreateDXGIFactory },
	{ NULL, NULL, NULL }
};
