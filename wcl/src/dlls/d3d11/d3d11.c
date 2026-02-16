/*
 * d3d11.c — CITC OS d3d11.dll 구현
 * ==================================
 *
 * Direct3D 11 소프트웨어 래스터라이저.
 *
 * D3D11 렌더링 파이프라인 (소프트웨어 구현):
 *
 *   IA (Input Assembler)
 *     → 버텍스 버퍼에서 정점 읽기
 *     → InputLayout에 따라 attribute 추출
 *
 *   VS (Vertex Shader)
 *     → Phase 4: pass-through (position, color 그대로 NDC)
 *     → 향후: DXBC 바이트코드 인터프리터
 *
 *   RS (Rasterizer)
 *     → NDC → 스크린 좌표 변환
 *     → 삼각형 래스터라이징 (edge function / barycentric)
 *
 *   PS (Pixel Shader)
 *     → Phase 4: 보간된 vertex color 출력
 *
 *   OM (Output Merger)
 *     → 렌더 타깃 텍스처에 쓰기
 *
 * 소프트웨어 렌더링이므로 모든 작업이 CPU에서 수행.
 * Vulkan 백엔드는 Phase 5에서 추가 예정.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../../../include/win32.h"
#include "../../../include/d3d11_types.h"
#include "../../../include/stub_entry.h"
#include "../dxgi/dxgi.h"

/* ============================================================
 * 핸들 오프셋
 * ============================================================ */
#define DX_RESOURCE_OFFSET  0x52000
#define DX_VIEW_OFFSET      0x53000
#define DX_SHADER_OFFSET    0x54000
#define DX_LAYOUT_OFFSET    0x56000

/* ============================================================
 * 리소스 테이블 (gdi32 패턴)
 * ============================================================
 *
 * D3D11 리소스 (버퍼, 텍스처)를 정적 테이블에 저장.
 * 핸들 = (void*)(index + DX_RESOURCE_OFFSET)
 */
#define MAX_D3D_RESOURCES 256

enum d3d_resource_type {
	D3D_RES_FREE = 0,
	D3D_RES_BUFFER,
	D3D_RES_TEXTURE2D,
};

struct d3d_resource {
	int active;
	enum d3d_resource_type type;
	void *data;             /* CPU 메모리 (버퍼 내용 / 텍스처 픽셀) */
	size_t size;            /* data의 바이트 크기 */

	/* BUFFER 전용 */
	D3D11_BUFFER_DESC buf_desc;

	/* TEXTURE2D 전용 */
	int width, height;
	DXGI_FORMAT format;
	uint32_t *pixels;       /* XRGB8888 (렌더 타깃일 때) */

	/* SwapChain 연동 */
	int is_swapchain_buffer; /* 1이면 pixels는 SwapChain 소유 */
};

static struct d3d_resource resource_table[MAX_D3D_RESOURCES];

static int alloc_resource(void)
{
	for (int i = 0; i < MAX_D3D_RESOURCES; i++)
		if (!resource_table[i].active)
			return i;
	return -1;
}

static void *resource_to_handle(int idx)
{
	return (void *)(uintptr_t)(idx + DX_RESOURCE_OFFSET);
}

static int handle_to_resource_idx(void *handle)
{
	uintptr_t val = (uintptr_t)handle;
	if (val < DX_RESOURCE_OFFSET) return -1;
	int idx = (int)(val - DX_RESOURCE_OFFSET);
	if (idx < 0 || idx >= MAX_D3D_RESOURCES) return -1;
	if (!resource_table[idx].active) return -1;
	return idx;
}

/* ============================================================
 * 뷰 테이블
 * ============================================================ */
#define MAX_D3D_VIEWS 128

enum d3d_view_type {
	D3D_VIEW_FREE = 0,
	D3D_VIEW_RTV,
	D3D_VIEW_SRV,
	D3D_VIEW_DSV,
};

struct d3d_view {
	int active;
	enum d3d_view_type type;
	int resource_idx;
};

static struct d3d_view view_table[MAX_D3D_VIEWS];

static int alloc_view(void)
{
	for (int i = 0; i < MAX_D3D_VIEWS; i++)
		if (!view_table[i].active)
			return i;
	return -1;
}

static void *view_to_handle(int idx)
{
	return (void *)(uintptr_t)(idx + DX_VIEW_OFFSET);
}

static int handle_to_view_idx(void *handle)
{
	uintptr_t val = (uintptr_t)handle;
	if (val < DX_VIEW_OFFSET) return -1;
	int idx = (int)(val - DX_VIEW_OFFSET);
	if (idx < 0 || idx >= MAX_D3D_VIEWS) return -1;
	if (!view_table[idx].active) return -1;
	return idx;
}

/* ============================================================
 * 셰이더 테이블
 * ============================================================ */
#define MAX_D3D_SHADERS 64

enum d3d_shader_type {
	D3D_SHADER_FREE = 0,
	D3D_SHADER_VERTEX,
	D3D_SHADER_PIXEL,
};

struct d3d_shader {
	int active;
	enum d3d_shader_type type;
	void *bytecode;
	size_t bytecode_size;
};

static struct d3d_shader shader_table[MAX_D3D_SHADERS];

static int alloc_shader(void)
{
	for (int i = 0; i < MAX_D3D_SHADERS; i++)
		if (!shader_table[i].active)
			return i;
	return -1;
}

static void *shader_to_handle(int idx)
{
	return (void *)(uintptr_t)(idx + DX_SHADER_OFFSET);
}

/* ============================================================
 * InputLayout 테이블
 * ============================================================ */
#define MAX_D3D_LAYOUTS 32
#define MAX_INPUT_ELEMENTS 16

struct d3d_input_layout {
	int active;
	D3D11_INPUT_ELEMENT_DESC elements[MAX_INPUT_ELEMENTS];
	int num_elements;
};

static struct d3d_input_layout layout_table[MAX_D3D_LAYOUTS];

static int alloc_layout(void)
{
	for (int i = 0; i < MAX_D3D_LAYOUTS; i++)
		if (!layout_table[i].active)
			return i;
	return -1;
}

static void *layout_to_handle(int idx)
{
	return (void *)(uintptr_t)(idx + DX_LAYOUT_OFFSET);
}

static int handle_to_layout_idx(void *handle)
{
	uintptr_t val = (uintptr_t)handle;
	if (val < DX_LAYOUT_OFFSET) return -1;
	int idx = (int)(val - DX_LAYOUT_OFFSET);
	if (idx < 0 || idx >= MAX_D3D_LAYOUTS) return -1;
	if (!layout_table[idx].active) return -1;
	return idx;
}

/* ============================================================
 * 유틸리티
 * ============================================================ */

/* float RGBA (0.0~1.0) → XRGB8888 */
static uint32_t float4_to_xrgb(const float c[4])
{
	int r = (int)(c[0] * 255.0f + 0.5f);
	int g = (int)(c[1] * 255.0f + 0.5f);
	int b = (int)(c[2] * 255.0f + 0.5f);
	if (r < 0) r = 0;
	if (r > 255) r = 255;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (b < 0) b = 0;
	if (b > 255) b = 255;
	return (uint32_t)((r << 16) | (g << 8) | b);
}

/* ============================================================
 * ID3D11Device 구현
 * ============================================================ */

struct d3d11_device {
	ID3D11DeviceVtbl *lpVtbl;
	ULONG ref_count;
	D3D_FEATURE_LEVEL feature_level;
};

/* IUnknown */
static HRESULT __attribute__((ms_abi))
dev_QueryInterface(void *This, REFIID riid, void **ppv)
{ (void)riid; if (!ppv) return E_POINTER; *ppv = This; return S_OK; }
static ULONG __attribute__((ms_abi))
dev_AddRef(void *This)
{ struct d3d11_device *d = This; return ++d->ref_count; }
static ULONG __attribute__((ms_abi))
dev_Release(void *This)
{
	struct d3d11_device *d = This;
	ULONG r = --d->ref_count;
	if (r == 0) free(d);
	return r;
}

/* CreateBuffer */
static HRESULT __attribute__((ms_abi))
dev_CreateBuffer(void *This, const D3D11_BUFFER_DESC *pDesc,
		 const D3D11_SUBRESOURCE_DATA *pInitialData, void **ppBuffer)
{
	(void)This;
	if (!pDesc || !ppBuffer) return E_POINTER;

	int idx = alloc_resource();
	if (idx < 0) return E_OUTOFMEMORY;

	struct d3d_resource *r = &resource_table[idx];
	memset(r, 0, sizeof(*r));
	r->active = 1;
	r->type = D3D_RES_BUFFER;
	r->buf_desc = *pDesc;
	r->size = pDesc->ByteWidth;

	r->data = calloc(1, pDesc->ByteWidth);
	if (!r->data) { r->active = 0; return E_OUTOFMEMORY; }

	if (pInitialData && pInitialData->pSysMem)
		memcpy(r->data, pInitialData->pSysMem, pDesc->ByteWidth);

	*ppBuffer = resource_to_handle(idx);
	return S_OK;
}

/* CreateTexture1D — 스텁 */
static HRESULT __attribute__((ms_abi))
dev_CreateTexture1D(void *T, void *d, void *i, void **pp)
{ (void)T; (void)d; (void)i; (void)pp; return E_FAIL; }

/* CreateTexture2D */
static HRESULT __attribute__((ms_abi))
dev_CreateTexture2D(void *This, const D3D11_TEXTURE2D_DESC *pDesc,
		    const D3D11_SUBRESOURCE_DATA *pInitialData, void **ppTexture2D)
{
	(void)This;
	if (!pDesc || !ppTexture2D) return E_POINTER;

	int idx = alloc_resource();
	if (idx < 0) return E_OUTOFMEMORY;

	struct d3d_resource *r = &resource_table[idx];
	memset(r, 0, sizeof(*r));
	r->active = 1;
	r->type = D3D_RES_TEXTURE2D;
	r->width = (int)pDesc->Width;
	r->height = (int)pDesc->Height;
	r->format = pDesc->Format;

	size_t pixel_count = (size_t)pDesc->Width * pDesc->Height;
	r->pixels = calloc(pixel_count, sizeof(uint32_t));
	if (!r->pixels) { r->active = 0; return E_OUTOFMEMORY; }

	r->data = r->pixels;
	r->size = pixel_count * 4;

	if (pInitialData && pInitialData->pSysMem)
		memcpy(r->pixels, pInitialData->pSysMem, r->size);

	*ppTexture2D = resource_to_handle(idx);
	return S_OK;
}

/* CreateTexture3D — 스텁 */
static HRESULT __attribute__((ms_abi))
dev_CreateTexture3D(void *T, void *d, void *i, void **pp)
{ (void)T; (void)d; (void)i; (void)pp; return E_FAIL; }

/* CreateShaderResourceView — 스텁 */
static HRESULT __attribute__((ms_abi))
dev_CreateShaderResourceView(void *T, void *r, void *d, void **pp)
{ (void)T; (void)r; (void)d; if (pp) *pp = NULL; return E_FAIL; }

/* CreateUnorderedAccessView — 스텁 */
static HRESULT __attribute__((ms_abi))
dev_CreateUnorderedAccessView(void *T, void *r, void *d, void **pp)
{ (void)T; (void)r; (void)d; if (pp) *pp = NULL; return E_FAIL; }

/* CreateRenderTargetView */
static HRESULT __attribute__((ms_abi))
dev_CreateRenderTargetView(void *This, void *pResource,
			   const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
			   void **ppRTView)
{
	(void)This; (void)pDesc;
	if (!ppRTView) return E_POINTER;

	int res_idx = handle_to_resource_idx(pResource);

	/*
	 * pResource가 리소스 테이블에 없으면,
	 * SwapChain의 GetBuffer()가 반환한 SwapChain 포인터일 수 있음.
	 * SwapChain의 백버퍼를 텍스처로 등록.
	 */
	if (res_idx < 0) {
		uint32_t *sc_pixels = NULL;
		int sc_w = 0, sc_h = 0;
		if (dxgi_get_swapchain_backbuffer(pResource, &sc_pixels,
						  &sc_w, &sc_h) == 0) {
			int idx = alloc_resource();
			if (idx < 0) return E_OUTOFMEMORY;

			struct d3d_resource *r = &resource_table[idx];
			memset(r, 0, sizeof(*r));
			r->active = 1;
			r->type = D3D_RES_TEXTURE2D;
			r->width = sc_w;
			r->height = sc_h;
			r->format = DXGI_FORMAT_B8G8R8A8_UNORM;
			r->pixels = sc_pixels; /* SwapChain 소유 */
			r->data = sc_pixels;
			r->size = (size_t)sc_w * sc_h * 4;
			r->is_swapchain_buffer = 1;

			dxgi_set_swapchain_resource(pResource, idx);
			res_idx = idx;
		}
	}

	if (res_idx < 0) return E_INVALIDARG;

	int vidx = alloc_view();
	if (vidx < 0) return E_OUTOFMEMORY;

	view_table[vidx].active = 1;
	view_table[vidx].type = D3D_VIEW_RTV;
	view_table[vidx].resource_idx = res_idx;

	*ppRTView = view_to_handle(vidx);
	return S_OK;
}

/* CreateDepthStencilView — 스텁 */
static HRESULT __attribute__((ms_abi))
dev_CreateDepthStencilView(void *T, void *r, void *d, void **pp)
{ (void)T; (void)r; (void)d; if (pp) *pp = NULL; return E_FAIL; }

/* CreateInputLayout */
static HRESULT __attribute__((ms_abi))
dev_CreateInputLayout(void *This,
		      const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs,
		      UINT NumElements,
		      const void *pShaderBytecodeWithInputSignature,
		      size_t BytecodeLength,
		      void **ppInputLayout)
{
	(void)This; (void)pShaderBytecodeWithInputSignature;
	(void)BytecodeLength;
	if (!ppInputLayout) return E_POINTER;

	int idx = alloc_layout();
	if (idx < 0) return E_OUTOFMEMORY;

	struct d3d_input_layout *l = &layout_table[idx];
	memset(l, 0, sizeof(*l));
	l->active = 1;
	l->num_elements = (int)(NumElements < MAX_INPUT_ELEMENTS ? NumElements : MAX_INPUT_ELEMENTS);

	for (int i = 0; i < l->num_elements; i++)
		l->elements[i] = pInputElementDescs[i];

	*ppInputLayout = layout_to_handle(idx);
	return S_OK;
}

/* CreateVertexShader */
static HRESULT __attribute__((ms_abi))
dev_CreateVertexShader(void *This, const void *pBytecode, size_t Length,
		       void *pClassLinkage, void **ppVertexShader)
{
	(void)This; (void)pClassLinkage;
	if (!ppVertexShader) return E_POINTER;

	int idx = alloc_shader();
	if (idx < 0) return E_OUTOFMEMORY;

	shader_table[idx].active = 1;
	shader_table[idx].type = D3D_SHADER_VERTEX;
	shader_table[idx].bytecode_size = Length;
	shader_table[idx].bytecode = NULL;
	if (pBytecode && Length > 0) {
		shader_table[idx].bytecode = malloc(Length);
		if (shader_table[idx].bytecode)
			memcpy(shader_table[idx].bytecode, pBytecode, Length);
	}

	*ppVertexShader = shader_to_handle(idx);
	return S_OK;
}

/* 중간 셰이더 스텁 (Hull, Domain, Geometry) */
static HRESULT __attribute__((ms_abi))
dev_CreateHullShader(void *T, const void *p, size_t l, void *c, void **pp)
{ (void)T; (void)p; (void)l; (void)c; if (pp) *pp = NULL; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
dev_CreateDomainShader(void *T, const void *p, size_t l, void *c, void **pp)
{ (void)T; (void)p; (void)l; (void)c; if (pp) *pp = NULL; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
dev_CreateGeometryShader(void *T, const void *p, size_t l, void *c, void **pp)
{ (void)T; (void)p; (void)l; (void)c; if (pp) *pp = NULL; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
dev_CreateGeometryShaderWithStreamOutput(void *T, const void *p, size_t l,
	void *so, UINT ne, void *bs, UINT nb, UINT rs, void *c, void **pp)
{ (void)T; (void)p; (void)l; (void)so; (void)ne; (void)bs;
  (void)nb; (void)rs; (void)c; if (pp) *pp = NULL; return E_FAIL; }

/* CreatePixelShader */
static HRESULT __attribute__((ms_abi))
dev_CreatePixelShader(void *This, const void *pBytecode, size_t Length,
		      void *pClassLinkage, void **ppPixelShader)
{
	(void)This; (void)pClassLinkage;
	if (!ppPixelShader) return E_POINTER;

	int idx = alloc_shader();
	if (idx < 0) return E_OUTOFMEMORY;

	shader_table[idx].active = 1;
	shader_table[idx].type = D3D_SHADER_PIXEL;
	shader_table[idx].bytecode_size = Length;
	shader_table[idx].bytecode = NULL;
	if (pBytecode && Length > 0) {
		shader_table[idx].bytecode = malloc(Length);
		if (shader_table[idx].bytecode)
			memcpy(shader_table[idx].bytecode, pBytecode, Length);
	}

	*ppPixelShader = shader_to_handle(idx);
	return S_OK;
}

/* 나머지 Device 메서드 — 스텁 */
static HRESULT __attribute__((ms_abi)) dev_stub_hr(void *T, ...) { (void)T; return E_FAIL; }
static HRESULT __attribute__((ms_abi)) dev_stub_hr_ok(void *T, ...) { (void)T; return S_OK; }
static void __attribute__((ms_abi)) dev_stub_void(void *T, ...) { (void)T; }
static UINT __attribute__((ms_abi)) dev_stub_uint(void *T, ...) { (void)T; return 0; }

static D3D_FEATURE_LEVEL __attribute__((ms_abi))
dev_GetFeatureLevel(void *This)
{
	struct d3d11_device *d = This;
	return d->feature_level;
}

static void __attribute__((ms_abi))
dev_GetImmediateContext(void *This, void **ppContext);

/* forward declaration — context 생성 후 설정 */
static struct d3d11_context *g_context;

static ID3D11DeviceVtbl g_device_vtbl = {
	.QueryInterface                = dev_QueryInterface,
	.AddRef                        = dev_AddRef,
	.Release                       = dev_Release,
	.CreateBuffer                  = dev_CreateBuffer,
	.CreateTexture1D               = dev_CreateTexture1D,
	.CreateTexture2D               = dev_CreateTexture2D,
	.CreateTexture3D               = dev_CreateTexture3D,
	.CreateShaderResourceView      = dev_CreateShaderResourceView,
	.CreateUnorderedAccessView     = dev_CreateUnorderedAccessView,
	.CreateRenderTargetView        = dev_CreateRenderTargetView,
	.CreateDepthStencilView        = dev_CreateDepthStencilView,
	.CreateInputLayout             = dev_CreateInputLayout,
	.CreateVertexShader            = dev_CreateVertexShader,
	.CreateHullShader              = dev_CreateHullShader,
	.CreateDomainShader            = dev_CreateDomainShader,
	.CreateGeometryShader          = dev_CreateGeometryShader,
	.CreateGeometryShaderWithStreamOutput = dev_CreateGeometryShaderWithStreamOutput,
	.CreatePixelShader             = dev_CreatePixelShader,
	.CreateBlendState              = (void *)dev_stub_hr,
	.CreateDepthStencilState       = (void *)dev_stub_hr,
	.CreateRasterizerState         = (void *)dev_stub_hr,
	.CreateSamplerState            = (void *)dev_stub_hr,
	.CreateQuery                   = (void *)dev_stub_hr,
	.CreatePredicate               = (void *)dev_stub_hr,
	.CreateCounter                 = (void *)dev_stub_hr,
	.CreateDeferredContext          = (void *)dev_stub_hr,
	.OpenSharedResource            = (void *)dev_stub_hr,
	.CheckFormatSupport            = (void *)dev_stub_hr,
	.CheckMultisampleQualityLevels = (void *)dev_stub_hr,
	.CheckCounterInfo              = (void *)dev_stub_void,
	.CheckCounter                  = (void *)dev_stub_hr,
	.CheckFeatureSupport           = (void *)dev_stub_hr,
	.GetPrivateData                = (void *)dev_stub_hr,
	.SetPrivateData                = (void *)dev_stub_hr_ok,
	.SetPrivateDataInterface       = (void *)dev_stub_hr_ok,
	.GetFeatureLevel               = dev_GetFeatureLevel,
	.GetCreationFlags              = (void *)dev_stub_uint,
	.GetDeviceRemovedReason        = (void *)dev_stub_hr_ok,
	.GetImmediateContext           = dev_GetImmediateContext,
	.SetExceptionMode              = (void *)dev_stub_hr_ok,
	.GetExceptionMode              = (void *)dev_stub_uint,
};

/* ============================================================
 * ID3D11DeviceContext 구현
 * ============================================================ */

struct d3d11_context {
	ID3D11DeviceContextVtbl *lpVtbl;
	ULONG ref_count;

	/* IA 스테이지 */
	int vb_resource_idx;
	UINT vb_stride, vb_offset;
	int ib_resource_idx;
	DXGI_FORMAT ib_format;
	int input_layout_idx;
	D3D11_PRIMITIVE_TOPOLOGY topology;

	/* 셰이더 스테이지 */
	int vs_idx, ps_idx;

	/* OM 스테이지 */
	int rtv_idx;
	int dsv_idx;

	/* RS 스테이지 */
	D3D11_VIEWPORT viewport;
};

/* IUnknown */
static HRESULT __attribute__((ms_abi))
ctx_QueryInterface(void *This, REFIID riid, void **ppv)
{ (void)riid; if (!ppv) return E_POINTER; *ppv = This; return S_OK; }
static ULONG __attribute__((ms_abi))
ctx_AddRef(void *This)
{ struct d3d11_context *c = This; return ++c->ref_count; }
static ULONG __attribute__((ms_abi))
ctx_Release(void *This)
{
	struct d3d11_context *c = This;
	ULONG r = --c->ref_count;
	if (r == 0)
		free(c);
	return r;
}

/* DeviceChild stubs */
static void __attribute__((ms_abi))
ctx_GetDevice(void *T, void **pp) { (void)T; if (pp) *pp = NULL; }
static HRESULT __attribute__((ms_abi))
ctx_GetPrivateData(void *T, REFIID g, UINT *s, void *d)
{ (void)T; (void)g; (void)s; (void)d; return E_FAIL; }
static HRESULT __attribute__((ms_abi))
ctx_SetPrivateData(void *T, REFIID g, UINT s, const void *d)
{ (void)T; (void)g; (void)s; (void)d; return S_OK; }
static HRESULT __attribute__((ms_abi))
ctx_SetPrivateDataInterface(void *T, REFIID g, void *d)
{ (void)T; (void)g; (void)d; return S_OK; }

/* VS 스테이지 */
static void __attribute__((ms_abi))
ctx_VSSetConstantBuffers(void *T, UINT s, UINT n, void *const *pp)
{ (void)T; (void)s; (void)n; (void)pp; }

static void __attribute__((ms_abi))
ctx_VSSetShader(void *This, void *pVS, void *const *ppCI, UINT nCI)
{
	struct d3d11_context *c = This;
	(void)ppCI; (void)nCI;
	uintptr_t val = (uintptr_t)pVS;
	c->vs_idx = (val >= DX_SHADER_OFFSET) ? (int)(val - DX_SHADER_OFFSET) : -1;
}

/* PS 스테이지 */
static void __attribute__((ms_abi))
ctx_PSSetShaderResources(void *T, UINT s, UINT n, void *const *pp)
{ (void)T; (void)s; (void)n; (void)pp; }
static void __attribute__((ms_abi))
ctx_PSSetSamplers(void *T, UINT s, UINT n, void *const *pp)
{ (void)T; (void)s; (void)n; (void)pp; }

static void __attribute__((ms_abi))
ctx_PSSetShader(void *This, void *pPS, void *const *ppCI, UINT nCI)
{
	struct d3d11_context *c = This;
	(void)ppCI; (void)nCI;
	uintptr_t val = (uintptr_t)pPS;
	c->ps_idx = (val >= DX_SHADER_OFFSET) ? (int)(val - DX_SHADER_OFFSET) : -1;
}

/* IA 스테이지 */
static void __attribute__((ms_abi))
ctx_IASetInputLayout(void *This, void *pInputLayout)
{
	struct d3d11_context *c = This;
	c->input_layout_idx = handle_to_layout_idx(pInputLayout);
}

static void __attribute__((ms_abi))
ctx_IASetVertexBuffers(void *This, UINT StartSlot, UINT NumBuffers,
		       void *const *ppVertexBuffers,
		       const UINT *pStrides, const UINT *pOffsets)
{
	struct d3d11_context *c = This;
	(void)StartSlot; (void)NumBuffers;
	if (ppVertexBuffers && ppVertexBuffers[0])
		c->vb_resource_idx = handle_to_resource_idx(ppVertexBuffers[0]);
	else
		c->vb_resource_idx = -1;
	c->vb_stride = pStrides ? pStrides[0] : 0;
	c->vb_offset = pOffsets ? pOffsets[0] : 0;
}

static void __attribute__((ms_abi))
ctx_IASetIndexBuffer(void *This, void *pIndexBuffer,
		     DXGI_FORMAT Format, UINT Offset)
{
	struct d3d11_context *c = This;
	(void)Offset;
	if (pIndexBuffer)
		c->ib_resource_idx = handle_to_resource_idx(pIndexBuffer);
	else
		c->ib_resource_idx = -1;
	c->ib_format = Format;
}

static void __attribute__((ms_abi))
ctx_IASetPrimitiveTopology(void *This, D3D11_PRIMITIVE_TOPOLOGY Topology)
{
	struct d3d11_context *c = This;
	c->topology = Topology;
}

/* OM 스테이지 */
static void __attribute__((ms_abi))
ctx_OMSetRenderTargets(void *This, UINT NumViews,
		       void *const *ppRenderTargetViews,
		       void *pDepthStencilView)
{
	struct d3d11_context *c = This;
	(void)pDepthStencilView;
	if (ppRenderTargetViews && NumViews > 0 && ppRenderTargetViews[0])
		c->rtv_idx = handle_to_view_idx(ppRenderTargetViews[0]);
	else
		c->rtv_idx = -1;
}

/* RS 스테이지 */
static void __attribute__((ms_abi))
ctx_RSSetViewports(void *This, UINT NumViewports,
		   const D3D11_VIEWPORT *pViewports)
{
	struct d3d11_context *c = This;
	if (pViewports && NumViewports > 0)
		c->viewport = pViewports[0];
}

/*
 * ClearRenderTargetView — RTV를 단색으로 초기화
 *
 * 가장 기본적인 렌더링 동작:
 *   RTV가 가리키는 텍스처의 모든 픽셀을 clearColor로 채움.
 */
static void __attribute__((ms_abi))
ctx_ClearRenderTargetView(void *This, void *pRenderTargetView,
			  const float ColorRGBA[4])
{
	(void)This;
	int vidx = handle_to_view_idx(pRenderTargetView);
	if (vidx < 0) return;

	int ridx = view_table[vidx].resource_idx;
	if (ridx < 0 || ridx >= MAX_D3D_RESOURCES) return;

	struct d3d_resource *r = &resource_table[ridx];
	if (!r->active || !r->pixels) return;

	uint32_t c = float4_to_xrgb(ColorRGBA);
	int count = r->width * r->height;
	for (int i = 0; i < count; i++)
		r->pixels[i] = c;
}

/* Map/Unmap — 리소스 CPU 접근 */
static HRESULT __attribute__((ms_abi))
ctx_Map(void *This, void *pResource, UINT Subresource,
	D3D11_MAP MapType, UINT MapFlags,
	D3D11_MAPPED_SUBRESOURCE *pMapped)
{
	(void)This; (void)Subresource; (void)MapType; (void)MapFlags;
	if (!pMapped) return E_POINTER;

	int idx = handle_to_resource_idx(pResource);
	if (idx < 0) return E_INVALIDARG;

	struct d3d_resource *r = &resource_table[idx];
	pMapped->pData = r->data;
	pMapped->RowPitch = (r->type == D3D_RES_TEXTURE2D) ?
			    (UINT)(r->width * 4) : (UINT)r->size;
	pMapped->DepthPitch = 0;
	return S_OK;
}

static void __attribute__((ms_abi))
ctx_Unmap(void *This, void *pResource, UINT Subresource)
{ (void)This; (void)pResource; (void)Subresource; }

/* ============================================================
 * 소프트웨어 래스터라이저
 * ============================================================
 *
 * Edge function 기반 삼각형 래스터라이징.
 *
 * 소프트웨어 파이프라인:
 *   1. VB에서 버텍스 읽기 (InputLayout으로 POSITION, COLOR 추출)
 *   2. NDC → 스크린 좌표 변환 (viewport transform)
 *   3. 삼각형별 edge function으로 내부 픽셀 결정
 *   4. Barycentric 보간으로 색상 계산
 *   5. 렌더 타깃에 쓰기
 */

struct sw_vertex {
	float pos[4];   /* x, y, z, w (NDC) */
	float color[4]; /* r, g, b, a */
};

/* edge function: 점 P가 엣지 AB의 어느 쪽에 있는지 */
static float edge_func(float ax, float ay, float bx, float by,
		       float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void rasterize_triangle(struct d3d_resource *rt,
			       const D3D11_VIEWPORT *vp,
			       const struct sw_vertex v[3])
{
	if (!rt || !rt->pixels) return;

	int rt_w = rt->width;
	int rt_h = rt->height;

	/* NDC(-1~1) → 스크린 좌표 변환 */
	float sx[3], sy[3];
	for (int i = 0; i < 3; i++) {
		sx[i] = vp->TopLeftX + (v[i].pos[0] + 1.0f) * 0.5f * vp->Width;
		sy[i] = vp->TopLeftY + (1.0f - v[i].pos[1]) * 0.5f * vp->Height;
	}

	/* 바운딩 박스 */
	float fmin_x = sx[0], fmax_x = sx[0];
	float fmin_y = sy[0], fmax_y = sy[0];
	for (int i = 1; i < 3; i++) {
		if (sx[i] < fmin_x) fmin_x = sx[i];
		if (sx[i] > fmax_x) fmax_x = sx[i];
		if (sy[i] < fmin_y) fmin_y = sy[i];
		if (sy[i] > fmax_y) fmax_y = sy[i];
	}

	int min_x = (int)floorf(fmin_x);
	int max_x = (int)ceilf(fmax_x);
	int min_y = (int)floorf(fmin_y);
	int max_y = (int)ceilf(fmax_y);

	if (min_x < 0) min_x = 0;
	if (min_y < 0) min_y = 0;
	if (max_x >= rt_w) max_x = rt_w - 1;
	if (max_y >= rt_h) max_y = rt_h - 1;

	/* 삼각형 전체 면적 (2배) */
	float area = edge_func(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
	if (fabsf(area) < 0.001f) return; /* 퇴화 삼각형 */

	float inv_area = 1.0f / area;

	for (int y = min_y; y <= max_y; y++) {
		for (int x = min_x; x <= max_x; x++) {
			float px = (float)x + 0.5f;
			float py = (float)y + 0.5f;

			float w0 = edge_func(sx[1], sy[1], sx[2], sy[2], px, py);
			float w1 = edge_func(sx[2], sy[2], sx[0], sy[0], px, py);
			float w2 = edge_func(sx[0], sy[0], sx[1], sy[1], px, py);

			/* 삼각형 내부 (winding order에 따라 부호 통일) */
			int inside;
			if (area > 0)
				inside = (w0 >= 0 && w1 >= 0 && w2 >= 0);
			else
				inside = (w0 <= 0 && w1 <= 0 && w2 <= 0);

			if (!inside) continue;

			/* barycentric 좌표 정규화 */
			float b0 = w0 * inv_area;
			float b1 = w1 * inv_area;
			float b2 = w2 * inv_area;

			/* 색상 보간 */
			float r = b0 * v[0].color[0] + b1 * v[1].color[0] + b2 * v[2].color[0];
			float g = b0 * v[0].color[1] + b1 * v[1].color[1] + b2 * v[2].color[1];
			float b = b0 * v[0].color[2] + b1 * v[1].color[2] + b2 * v[2].color[2];

			float rgba[4] = { r, g, b, 1.0f };
			rt->pixels[y * rt_w + x] = float4_to_xrgb(rgba);
		}
	}
}

/* InputLayout에서 시맨틱으로 오프셋 찾기 */
static int find_semantic_offset(const struct d3d_input_layout *layout,
				const char *semantic, int *out_format)
{
	for (int i = 0; i < layout->num_elements; i++) {
		if (layout->elements[i].SemanticName &&
		    strcmp(layout->elements[i].SemanticName, semantic) == 0) {
			if (out_format)
				*out_format = (int)layout->elements[i].Format;
			return (int)layout->elements[i].AlignedByteOffset;
		}
	}
	return -1;
}

/* VB에서 float 읽기 (format에 따라) */
static void read_float3(const uint8_t *base, int format, float out[3])
{
	(void)format;
	const float *f = (const float *)base;
	out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
}

static void read_float4(const uint8_t *base, int format, float out[4])
{
	if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
		const float *f = (const float *)base;
		out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = f[3];
	} else if (format == DXGI_FORMAT_R32G32B32_FLOAT) {
		const float *f = (const float *)base;
		out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = 1.0f;
	} else {
		out[0] = out[1] = out[2] = 1.0f; out[3] = 1.0f;
	}
}

/*
 * Draw — 소프트웨어 렌더링 파이프라인 실행
 *
 * 현재 바인딩된 VB, InputLayout, RTV를 사용하여
 * 삼각형 단위로 래스터라이징.
 */
static void __attribute__((ms_abi))
ctx_Draw(void *This, UINT VertexCount, UINT StartVertexLocation)
{
	struct d3d11_context *c = This;

	/* RTV → 렌더 타깃 리소스 */
	if (c->rtv_idx < 0) return;
	int ridx = view_table[c->rtv_idx].resource_idx;
	if (ridx < 0) return;
	struct d3d_resource *rt = &resource_table[ridx];

	/* VB 데이터 */
	if (c->vb_resource_idx < 0) return;
	struct d3d_resource *vb = &resource_table[c->vb_resource_idx];
	if (!vb->data) return;

	/* InputLayout */
	if (c->input_layout_idx < 0) return;
	struct d3d_input_layout *layout = &layout_table[c->input_layout_idx];

	/* POSITION, COLOR 오프셋 찾기 */
	int pos_fmt = 0, col_fmt = 0;
	int pos_off = find_semantic_offset(layout, "POSITION", &pos_fmt);
	int col_off = find_semantic_offset(layout, "COLOR", &col_fmt);

	if (pos_off < 0) {
		/* SV_Position 시도 */
		pos_off = find_semantic_offset(layout, "SV_Position", &pos_fmt);
	}
	if (pos_off < 0) return; /* position 없으면 그릴 수 없음 */

	UINT stride = c->vb_stride;
	if (stride == 0) return;

	const uint8_t *vb_data = (const uint8_t *)vb->data;

	/* 삼각형 리스트 래스터라이징 */
	if (c->topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		for (UINT i = StartVertexLocation; i + 2 < StartVertexLocation + VertexCount; i += 3) {
			struct sw_vertex tri[3];

			for (int j = 0; j < 3; j++) {
				const uint8_t *v = vb_data + (i + j) * stride;

				read_float3(v + pos_off, pos_fmt, tri[j].pos);
				tri[j].pos[3] = 1.0f;

				if (col_off >= 0)
					read_float4(v + col_off, col_fmt, tri[j].color);
				else {
					tri[j].color[0] = 1.0f;
					tri[j].color[1] = 1.0f;
					tri[j].color[2] = 1.0f;
					tri[j].color[3] = 1.0f;
				}
			}

			rasterize_triangle(rt, &c->viewport, tri);
		}
	}
}

/* DrawIndexed */
static void __attribute__((ms_abi))
ctx_DrawIndexed(void *This, UINT IndexCount,
		UINT StartIndexLocation, int BaseVertexLocation)
{
	struct d3d11_context *c = This;

	if (c->rtv_idx < 0) return;
	int ridx = view_table[c->rtv_idx].resource_idx;
	if (ridx < 0) return;
	struct d3d_resource *rt = &resource_table[ridx];

	if (c->vb_resource_idx < 0 || c->ib_resource_idx < 0) return;
	struct d3d_resource *vb = &resource_table[c->vb_resource_idx];
	struct d3d_resource *ib = &resource_table[c->ib_resource_idx];
	if (!vb->data || !ib->data) return;

	if (c->input_layout_idx < 0) return;
	struct d3d_input_layout *layout = &layout_table[c->input_layout_idx];

	int pos_fmt = 0, col_fmt = 0;
	int pos_off = find_semantic_offset(layout, "POSITION", &pos_fmt);
	int col_off = find_semantic_offset(layout, "COLOR", &col_fmt);
	if (pos_off < 0)
		pos_off = find_semantic_offset(layout, "SV_Position", &pos_fmt);
	if (pos_off < 0) return;

	UINT stride = c->vb_stride;
	if (stride == 0) return;

	const uint8_t *vb_data = (const uint8_t *)vb->data;
	const uint8_t *ib_data = (const uint8_t *)ib->data;

	if (c->topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		for (UINT i = StartIndexLocation; i + 2 < StartIndexLocation + IndexCount; i += 3) {
			struct sw_vertex tri[3];

			for (int j = 0; j < 3; j++) {
				UINT idx;
				if (c->ib_format == DXGI_FORMAT_R16_UINT) {
					idx = ((const uint16_t *)ib_data)[i + j];
				} else {
					idx = ((const uint32_t *)ib_data)[i + j];
				}
				idx = (UINT)((int)idx + BaseVertexLocation);

				const uint8_t *v = vb_data + idx * stride;
				read_float3(v + pos_off, pos_fmt, tri[j].pos);
				tri[j].pos[3] = 1.0f;

				if (col_off >= 0)
					read_float4(v + col_off, col_fmt, tri[j].color);
				else {
					tri[j].color[0] = 1.0f;
					tri[j].color[1] = 1.0f;
					tri[j].color[2] = 1.0f;
					tri[j].color[3] = 1.0f;
				}
			}

			rasterize_triangle(rt, &c->viewport, tri);
		}
	}
}

/* PSSetConstantBuffers */
static void __attribute__((ms_abi))
ctx_PSSetConstantBuffers(void *T, UINT s, UINT n, void *const *pp)
{ (void)T; (void)s; (void)n; (void)pp; }

/* 나머지 Context 스텁 */
static void __attribute__((ms_abi)) ctx_stub(void *T, ...)
{ (void)T; }
static HRESULT __attribute__((ms_abi)) ctx_stub_hr(void *T, ...)
{ (void)T; return E_FAIL; }

static void __attribute__((ms_abi))
ctx_ClearState(void *This)
{
	struct d3d11_context *c = This;
	c->vb_resource_idx = -1;
	c->ib_resource_idx = -1;
	c->input_layout_idx = -1;
	c->vs_idx = -1;
	c->ps_idx = -1;
	c->rtv_idx = -1;
	c->dsv_idx = -1;
	c->topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	memset(&c->viewport, 0, sizeof(c->viewport));
}

static ID3D11DeviceContextVtbl g_context_vtbl = {
	.QueryInterface           = ctx_QueryInterface,
	.AddRef                   = ctx_AddRef,
	.Release                  = ctx_Release,
	.GetDevice                = ctx_GetDevice,
	.GetPrivateData           = ctx_GetPrivateData,
	.SetPrivateData           = ctx_SetPrivateData,
	.SetPrivateDataInterface  = ctx_SetPrivateDataInterface,
	/* VS */
	.VSSetConstantBuffers     = ctx_VSSetConstantBuffers,
	.PSSetShaderResources     = ctx_PSSetShaderResources,
	.PSSetShader              = ctx_PSSetShader,
	.PSSetSamplers            = ctx_PSSetSamplers,
	.VSSetShader              = ctx_VSSetShader,
	/* Draw */
	.DrawIndexed              = ctx_DrawIndexed,
	.Draw                     = ctx_Draw,
	.Map                      = ctx_Map,
	.Unmap                    = ctx_Unmap,
	.PSSetConstantBuffers     = ctx_PSSetConstantBuffers,
	/* IA */
	.IASetInputLayout         = ctx_IASetInputLayout,
	.IASetVertexBuffers       = ctx_IASetVertexBuffers,
	.IASetIndexBuffer         = ctx_IASetIndexBuffer,
	/* instancing */
	.DrawIndexedInstanced     = (void *)ctx_stub,
	.DrawInstanced            = (void *)ctx_stub,
	.GSSetConstantBuffers     = (void *)ctx_stub,
	.GSSetShader              = (void *)ctx_stub,
	.IASetPrimitiveTopology   = ctx_IASetPrimitiveTopology,
	.VSSetShaderResources     = (void *)ctx_stub,
	.VSSetSamplers            = (void *)ctx_stub,
	.Begin                    = (void *)ctx_stub,
	.End                      = (void *)ctx_stub,
	.GetData                  = (void *)ctx_stub_hr,
	.SetPredication           = (void *)ctx_stub,
	.GSSetShaderResources     = (void *)ctx_stub,
	.GSSetSamplers            = (void *)ctx_stub,
	/* OM */
	.OMSetRenderTargets       = ctx_OMSetRenderTargets,
	.OMSetRenderTargetsAndUnorderedAccessViews = (void *)ctx_stub,
	.OMSetBlendState          = (void *)ctx_stub,
	.OMSetDepthStencilState   = (void *)ctx_stub,
	.SOSetTargets             = (void *)ctx_stub,
	.DrawAuto                 = (void *)ctx_stub,
	.DrawIndexedInstancedIndirect = (void *)ctx_stub,
	.DrawInstancedIndirect    = (void *)ctx_stub,
	.Dispatch                 = (void *)ctx_stub,
	.DispatchIndirect         = (void *)ctx_stub,
	.RSSetState               = (void *)ctx_stub,
	.RSSetViewports           = ctx_RSSetViewports,
	.RSSetScissorRects        = (void *)ctx_stub,
	/* Copy/Update */
	.CopySubresourceRegion    = (void *)ctx_stub,
	.CopyResource             = (void *)ctx_stub,
	.UpdateSubresource        = (void *)ctx_stub,
	.CopyStructureCount       = (void *)ctx_stub,
	/* Clear */
	.ClearRenderTargetView    = ctx_ClearRenderTargetView,
	.ClearUnorderedAccessViewUint  = (void *)ctx_stub,
	.ClearUnorderedAccessViewFloat = (void *)ctx_stub,
	.ClearDepthStencilView    = (void *)ctx_stub,
	.GenerateMips             = (void *)ctx_stub,
	.SetResourceMinLOD        = (void *)ctx_stub,
	.GetResourceMinLOD        = (void *)ctx_stub,
	.ResolveSubresource       = (void *)ctx_stub,
	.ExecuteCommandList       = (void *)ctx_stub,
	/* HS/DS/CS */
	.HSSetShaderResources     = (void *)ctx_stub,
	.HSSetShader              = (void *)ctx_stub,
	.HSSetSamplers            = (void *)ctx_stub,
	.HSSetConstantBuffers     = (void *)ctx_stub,
	.DSSetShaderResources     = (void *)ctx_stub,
	.DSSetShader              = (void *)ctx_stub,
	.DSSetSamplers            = (void *)ctx_stub,
	.DSSetConstantBuffers     = (void *)ctx_stub,
	.CSSetShaderResources     = (void *)ctx_stub,
	.CSSetUnorderedAccessViews = (void *)ctx_stub,
	.CSSetShader              = (void *)ctx_stub,
	.CSSetSamplers            = (void *)ctx_stub,
	.CSSetConstantBuffers     = (void *)ctx_stub,
	/* Get methods */
	.VSGetConstantBuffers     = (void *)ctx_stub,
	.PSGetShaderResources     = (void *)ctx_stub,
	.PSGetShader              = (void *)ctx_stub,
	.PSGetSamplers            = (void *)ctx_stub,
	.VSGetShader              = (void *)ctx_stub,
	.PSGetConstantBuffers     = (void *)ctx_stub,
	.IAGetInputLayout         = (void *)ctx_stub,
	.IAGetVertexBuffers       = (void *)ctx_stub,
	.IAGetIndexBuffer         = (void *)ctx_stub,
	.GSGetConstantBuffers     = (void *)ctx_stub,
	.GSGetShader              = (void *)ctx_stub,
	.IAGetPrimitiveTopology   = (void *)ctx_stub,
	.VSGetShaderResources     = (void *)ctx_stub,
	.VSGetSamplers            = (void *)ctx_stub,
	.GetPredication           = (void *)ctx_stub,
	.GSGetShaderResources     = (void *)ctx_stub,
	.GSGetSamplers            = (void *)ctx_stub,
	.OMGetRenderTargets       = (void *)ctx_stub,
	.OMGetRenderTargetsAndUnorderedAccessViews = (void *)ctx_stub,
	.OMGetBlendState          = (void *)ctx_stub,
	.OMGetDepthStencilState   = (void *)ctx_stub,
	.SOGetTargets             = (void *)ctx_stub,
	.RSGetState               = (void *)ctx_stub,
	.RSGetViewports           = (void *)ctx_stub,
	.RSGetScissorRects        = (void *)ctx_stub,
	.HSGetShaderResources     = (void *)ctx_stub,
	.HSGetShader              = (void *)ctx_stub,
	.HSGetSamplers            = (void *)ctx_stub,
	.HSGetConstantBuffers     = (void *)ctx_stub,
	.DSGetShaderResources     = (void *)ctx_stub,
	.DSGetShader              = (void *)ctx_stub,
	.DSGetSamplers            = (void *)ctx_stub,
	.DSGetConstantBuffers     = (void *)ctx_stub,
	.CSGetShaderResources     = (void *)ctx_stub,
	.CSGetUnorderedAccessViews = (void *)ctx_stub,
	.CSGetShader              = (void *)ctx_stub,
	.CSGetSamplers            = (void *)ctx_stub,
	.CSGetConstantBuffers     = (void *)ctx_stub,
	.ClearState               = ctx_ClearState,
	.Flush                    = (void *)ctx_stub,
	.GetType                  = (void *)ctx_stub,
	.GetContextFlags          = (void *)ctx_stub,
	.FinishCommandList        = (void *)ctx_stub_hr,
};

/* ============================================================
 * GetImmediateContext (Device vtable에서 참조)
 * ============================================================ */

static void __attribute__((ms_abi))
dev_GetImmediateContext(void *This, void **ppContext)
{
	(void)This;
	if (ppContext) {
		*ppContext = g_context;
		if (g_context) g_context->ref_count++;
	}
}

/* ============================================================
 * 내부 API (dxgi.c 연동)
 * ============================================================ */

int d3d11_register_swapchain_texture(void *pSwapChain,
				     uint32_t *pixels,
				     int width, int height)
{
	(void)pSwapChain;
	int idx = alloc_resource();
	if (idx < 0) return -1;

	struct d3d_resource *r = &resource_table[idx];
	memset(r, 0, sizeof(*r));
	r->active = 1;
	r->type = D3D_RES_TEXTURE2D;
	r->width = width;
	r->height = height;
	r->format = DXGI_FORMAT_B8G8R8A8_UNORM;
	r->pixels = pixels;
	r->data = pixels;
	r->size = (size_t)width * height * 4;
	r->is_swapchain_buffer = 1;

	return idx;
}

/* ============================================================
 * DLL 엔트리: D3D11CreateDevice, D3D11CreateDeviceAndSwapChain
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
d3d11_CreateDevice(void *pAdapter, D3D_DRIVER_TYPE DriverType,
		   void *Software, UINT Flags,
		   const D3D_FEATURE_LEVEL *pFeatureLevels,
		   UINT FeatureLevels,
		   UINT SDKVersion,
		   void **ppDevice,
		   D3D_FEATURE_LEVEL *pFeatureLevel,
		   void **ppImmediateContext)
{
	(void)pAdapter; (void)DriverType; (void)Software;
	(void)Flags; (void)SDKVersion;

	/* Device 생성 */
	struct d3d11_device *dev = calloc(1, sizeof(*dev));
	if (!dev) return E_OUTOFMEMORY;

	dev->lpVtbl = &g_device_vtbl;
	dev->ref_count = 1;
	dev->feature_level = D3D_FEATURE_LEVEL_11_0;

	if (pFeatureLevels && FeatureLevels > 0)
		dev->feature_level = pFeatureLevels[0];

	/* Context 생성 (싱글턴) */
	if (!g_context) {
		g_context = calloc(1, sizeof(*g_context));
		if (!g_context) { free(dev); return E_OUTOFMEMORY; }
		g_context->lpVtbl = &g_context_vtbl;
		g_context->ref_count = 1;
		g_context->vb_resource_idx = -1;
		g_context->ib_resource_idx = -1;
		g_context->input_layout_idx = -1;
		g_context->vs_idx = -1;
		g_context->ps_idx = -1;
		g_context->rtv_idx = -1;
		g_context->dsv_idx = -1;
	}

	if (ppDevice) *ppDevice = dev;
	if (pFeatureLevel) *pFeatureLevel = dev->feature_level;
	if (ppImmediateContext) {
		*ppImmediateContext = g_context;
		g_context->ref_count++;
	}

	printf("d3d11: Device created (FL %x, software rasterizer)\n",
	       dev->feature_level);
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
d3d11_CreateDeviceAndSwapChain(void *pAdapter, D3D_DRIVER_TYPE DriverType,
			       void *Software, UINT Flags,
			       const D3D_FEATURE_LEVEL *pFeatureLevels,
			       UINT FeatureLevels,
			       UINT SDKVersion,
			       DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
			       void **ppSwapChain,
			       void **ppDevice,
			       D3D_FEATURE_LEVEL *pFeatureLevel,
			       void **ppImmediateContext)
{
	/* Device + Context 생성 */
	HRESULT hr = d3d11_CreateDevice(pAdapter, DriverType, Software,
					Flags, pFeatureLevels, FeatureLevels,
					SDKVersion, ppDevice, pFeatureLevel,
					ppImmediateContext);
	if (FAILED(hr)) return hr;

	/* SwapChain 생성 (DXGI 내부 API 직접 호출) */
	if (pSwapChainDesc && ppSwapChain) {
		hr = dxgi_create_swapchain_for_d3d11(
			ppDevice ? *ppDevice : NULL,
			pSwapChainDesc, ppSwapChain);

		if (FAILED(hr)) {
			if (ppDevice && *ppDevice) {
				ID3D11DeviceVtbl **dvt = *ppDevice;
				(*dvt)->Release(*ppDevice);
				*ppDevice = NULL;
			}
			return hr;
		}
	}

	return S_OK;
}

/* ============================================================
 * 스텁 테이블
 * ============================================================ */

struct stub_entry d3d11_stub_table[] = {
	{ "d3d11.dll", "D3D11CreateDevice",
	  (void *)d3d11_CreateDevice },
	{ "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
	  (void *)d3d11_CreateDeviceAndSwapChain },
	{ NULL, NULL, NULL }
};
