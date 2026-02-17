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
#include "dxbc.h"
#include "spirv_emit.h"
#include "shader_cache.h"
#include "vk_backend.h"

/* ============================================================
 * 핸들 오프셋
 * ============================================================ */
#define DX_RESOURCE_OFFSET  0x52000
#define DX_VIEW_OFFSET      0x53000
#define DX_SHADER_OFFSET    0x54000
#define DX_LAYOUT_OFFSET    0x56000
#define DX_STATE_OFFSET     0x57000
#define DX_SAMPLER_OFFSET   0x58000

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
	float *depth;           /* D32_FLOAT 깊이 버퍼 */

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
	struct dxbc_info dxbc;  /* DXBC 파싱 결과 */
	/* SPIR-V 변환 결과 (Class 43-44) */
	uint32_t *spirv;
	size_t spirv_size;
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
 * 상태 오브젝트 테이블 (DepthStencil, Blend, Rasterizer)
 * ============================================================ */
#define MAX_D3D_STATES 64

enum d3d_state_type {
	D3D_STATE_FREE = 0,
	D3D_STATE_DEPTH_STENCIL,
	D3D_STATE_BLEND,
	D3D_STATE_RASTERIZER,
};

struct d3d_state {
	int active;
	enum d3d_state_type type;
	union {
		D3D11_DEPTH_STENCIL_DESC ds;
		D3D11_BLEND_DESC blend;
		D3D11_RASTERIZER_DESC rs;
	};
};

static struct d3d_state state_table[MAX_D3D_STATES];

static int alloc_state(void)
{
	for (int i = 0; i < MAX_D3D_STATES; i++)
		if (!state_table[i].active)
			return i;
	return -1;
}

static void *state_to_handle(int idx)
{
	return (void *)(uintptr_t)(idx + DX_STATE_OFFSET);
}

static int handle_to_state_idx(void *handle)
{
	uintptr_t val = (uintptr_t)handle;
	if (val < DX_STATE_OFFSET) return -1;
	int idx = (int)(val - DX_STATE_OFFSET);
	if (idx < 0 || idx >= MAX_D3D_STATES) return -1;
	if (!state_table[idx].active) return -1;
	return idx;
}

/* ============================================================
 * 샘플러 테이블
 * ============================================================ */
#define MAX_D3D_SAMPLERS 32

struct d3d_sampler {
	int active;
	D3D11_SAMPLER_DESC desc;
};

static struct d3d_sampler sampler_table[MAX_D3D_SAMPLERS];

static int alloc_sampler(void)
{
	for (int i = 0; i < MAX_D3D_SAMPLERS; i++)
		if (!sampler_table[i].active)
			return i;
	return -1;
}

static void *sampler_to_handle(int idx)
{
	return (void *)(uintptr_t)(idx + DX_SAMPLER_OFFSET);
}

static int handle_to_sampler_idx(void *handle)
{
	uintptr_t val = (uintptr_t)handle;
	if (val < DX_SAMPLER_OFFSET) return -1;
	int idx = (int)(val - DX_SAMPLER_OFFSET);
	if (idx < 0 || idx >= MAX_D3D_SAMPLERS) return -1;
	if (!sampler_table[idx].active) return -1;
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

	if (pDesc->Format == DXGI_FORMAT_D32_FLOAT) {
		/* 깊이 버퍼: float 배열, 1.0f로 초기화 */
		r->depth = malloc(pixel_count * sizeof(float));
		if (!r->depth) { r->active = 0; return E_OUTOFMEMORY; }
		for (size_t i = 0; i < pixel_count; i++)
			r->depth[i] = 1.0f;
		r->data = r->depth;
		r->size = pixel_count * sizeof(float);
		r->pixels = NULL;
	} else {
		r->pixels = calloc(pixel_count, sizeof(uint32_t));
		if (!r->pixels) { r->active = 0; return E_OUTOFMEMORY; }
		r->data = r->pixels;
		r->size = pixel_count * 4;
		r->depth = NULL;

		if (pInitialData && pInitialData->pSysMem)
			memcpy(r->pixels, pInitialData->pSysMem, r->size);
	}

	*ppTexture2D = resource_to_handle(idx);
	return S_OK;
}

/* CreateTexture3D — 스텁 */
static HRESULT __attribute__((ms_abi))
dev_CreateTexture3D(void *T, void *d, void *i, void **pp)
{ (void)T; (void)d; (void)i; (void)pp; return E_FAIL; }

/* CreateShaderResourceView */
static HRESULT __attribute__((ms_abi))
dev_CreateShaderResourceView(void *This, void *pResource,
			     void *pDesc, void **ppSRView)
{
	(void)This; (void)pDesc;
	if (!ppSRView) return E_POINTER;

	int res_idx = handle_to_resource_idx(pResource);
	if (res_idx < 0) return E_INVALIDARG;

	int vidx = alloc_view();
	if (vidx < 0) return E_OUTOFMEMORY;

	view_table[vidx].active = 1;
	view_table[vidx].type = D3D_VIEW_SRV;
	view_table[vidx].resource_idx = res_idx;

	*ppSRView = view_to_handle(vidx);
	return S_OK;
}

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

/* CreateDepthStencilView */
static HRESULT __attribute__((ms_abi))
dev_CreateDepthStencilView(void *This, void *pResource,
			   void *pDesc, void **ppDSView)
{
	(void)This; (void)pDesc;
	if (!ppDSView) return E_POINTER;

	int res_idx = handle_to_resource_idx(pResource);
	if (res_idx < 0) return E_INVALIDARG;

	int vidx = alloc_view();
	if (vidx < 0) return E_OUTOFMEMORY;

	view_table[vidx].active = 1;
	view_table[vidx].type = D3D_VIEW_DSV;
	view_table[vidx].resource_idx = res_idx;

	*ppDSView = view_to_handle(vidx);
	return S_OK;
}

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
	shader_table[idx].spirv = NULL;
	shader_table[idx].spirv_size = 0;
	memset(&shader_table[idx].dxbc, 0, sizeof(struct dxbc_info));
	if (pBytecode && Length > 0) {
		shader_table[idx].bytecode = malloc(Length);
		if (shader_table[idx].bytecode) {
			memcpy(shader_table[idx].bytecode, pBytecode, Length);
			dxbc_parse(shader_table[idx].bytecode, Length,
				   &shader_table[idx].dxbc);
			/* Shader cache 조회 (Class 53) */
			if (shader_table[idx].dxbc.valid) {
				if (shader_cache_lookup(
					    pBytecode, Length,
					    &shader_table[idx].spirv,
					    &shader_table[idx].spirv_size) != 0) {
					/* 캐시 미스 → 컴파일 + 저장 */
					dxbc_to_spirv(&shader_table[idx].dxbc,
						      &shader_table[idx].spirv,
						      &shader_table[idx].spirv_size);
					if (shader_table[idx].spirv)
						shader_cache_store(
							pBytecode, Length,
							shader_table[idx].spirv,
							shader_table[idx].spirv_size);
				}
			}
		}
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
	shader_table[idx].spirv = NULL;
	shader_table[idx].spirv_size = 0;
	memset(&shader_table[idx].dxbc, 0, sizeof(struct dxbc_info));
	if (pBytecode && Length > 0) {
		shader_table[idx].bytecode = malloc(Length);
		if (shader_table[idx].bytecode) {
			memcpy(shader_table[idx].bytecode, pBytecode, Length);
			dxbc_parse(shader_table[idx].bytecode, Length,
				   &shader_table[idx].dxbc);
			/* Shader cache 조회 (Class 53) */
			if (shader_table[idx].dxbc.valid) {
				if (shader_cache_lookup(
					    pBytecode, Length,
					    &shader_table[idx].spirv,
					    &shader_table[idx].spirv_size) != 0) {
					dxbc_to_spirv(&shader_table[idx].dxbc,
						      &shader_table[idx].spirv,
						      &shader_table[idx].spirv_size);
					if (shader_table[idx].spirv)
						shader_cache_store(
							pBytecode, Length,
							shader_table[idx].spirv,
							shader_table[idx].spirv_size);
				}
			}
		}
	}

	*ppPixelShader = shader_to_handle(idx);
	return S_OK;
}

/* CreateDepthStencilState */
static HRESULT __attribute__((ms_abi))
dev_CreateDepthStencilState(void *This, const D3D11_DEPTH_STENCIL_DESC *pDesc,
			    void **ppDepthStencilState)
{
	(void)This;
	if (!pDesc || !ppDepthStencilState) return E_POINTER;

	int idx = alloc_state();
	if (idx < 0) return E_OUTOFMEMORY;

	state_table[idx].active = 1;
	state_table[idx].type = D3D_STATE_DEPTH_STENCIL;
	state_table[idx].ds = *pDesc;

	*ppDepthStencilState = state_to_handle(idx);
	return S_OK;
}

/* CreateBlendState */
static HRESULT __attribute__((ms_abi))
dev_CreateBlendState(void *This, const D3D11_BLEND_DESC *pDesc,
		     void **ppBlendState)
{
	(void)This;
	if (!pDesc || !ppBlendState) return E_POINTER;

	int idx = alloc_state();
	if (idx < 0) return E_OUTOFMEMORY;

	state_table[idx].active = 1;
	state_table[idx].type = D3D_STATE_BLEND;
	state_table[idx].blend = *pDesc;

	*ppBlendState = state_to_handle(idx);
	return S_OK;
}

/* CreateRasterizerState */
static HRESULT __attribute__((ms_abi))
dev_CreateRasterizerState(void *This, const D3D11_RASTERIZER_DESC *pDesc,
			  void **ppRasterizerState)
{
	(void)This;
	if (!pDesc || !ppRasterizerState) return E_POINTER;

	int idx = alloc_state();
	if (idx < 0) return E_OUTOFMEMORY;

	state_table[idx].active = 1;
	state_table[idx].type = D3D_STATE_RASTERIZER;
	state_table[idx].rs = *pDesc;

	*ppRasterizerState = state_to_handle(idx);
	return S_OK;
}

/* CreateSamplerState */
static HRESULT __attribute__((ms_abi))
dev_CreateSamplerState(void *This, const D3D11_SAMPLER_DESC *pDesc,
		       void **ppSamplerState)
{
	(void)This;
	if (!pDesc || !ppSamplerState) return E_POINTER;

	int idx = alloc_sampler();
	if (idx < 0) return E_OUTOFMEMORY;

	sampler_table[idx].active = 1;
	sampler_table[idx].desc = *pDesc;

	*ppSamplerState = sampler_to_handle(idx);
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

/* ============================================================
 * Vulkan GPU 백엔드 전역 상태 (Class 41-44)
 * ============================================================ */
#ifdef CITC_VULKAN_ENABLED
#include "vk_pipeline.h"
static int use_vulkan;                   /* 1이면 GPU 가속 활성 */
static struct vk_backend g_vk;           /* Vulkan 디바이스 */
static struct vk_render_target g_vk_rt;  /* 렌더 타깃 (SwapChain 연동) */
static struct vk_pipeline_cache g_vk_pcache; /* 파이프라인 캐시 (Class 44) */
static struct vk_gpu_buffer g_vk_vb;     /* 임시 VB */
static struct vk_gpu_buffer g_vk_ib;     /* 임시 IB */
static struct vk_gpu_buffer g_vk_ubo;    /* 임시 UBO */
#endif

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
	.CreateBlendState              = (void *)dev_CreateBlendState,
	.CreateDepthStencilState       = (void *)dev_CreateDepthStencilState,
	.CreateRasterizerState         = (void *)dev_CreateRasterizerState,
	.CreateSamplerState            = (void *)dev_CreateSamplerState,
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

	/* Constant Buffer 슬롯 */
	int vs_cb_idx[8];   /* resource_table 인덱스, -1 = unbound */
	int ps_cb_idx[8];

	/* OM 스테이지 */
	int rtv_idx;
	int dsv_idx;

	/* PS 리소스 슬롯 */
	int ps_srv_idx[8];     /* view_table 인덱스, -1 = unbound */
	int ps_sampler_idx[8]; /* sampler_table 인덱스, -1 = unbound */

	/* 상태 오브젝트 인덱스 (state_table, -1 = 기본값) */
	int ds_state_idx;   /* DepthStencil */
	int blend_state_idx;
	int rs_state_idx;   /* Rasterizer */
	UINT stencil_ref;

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
ctx_VSSetConstantBuffers(void *This, UINT StartSlot, UINT NumBuffers,
			 void *const *ppConstantBuffers)
{
	struct d3d11_context *c = This;
	for (UINT i = 0; i < NumBuffers && (StartSlot + i) < 8; i++) {
		if (ppConstantBuffers && ppConstantBuffers[i])
			c->vs_cb_idx[StartSlot + i] =
				handle_to_resource_idx(ppConstantBuffers[i]);
		else
			c->vs_cb_idx[StartSlot + i] = -1;
	}
}

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
ctx_PSSetShaderResources(void *This, UINT StartSlot, UINT NumViews,
			 void *const *ppSRViews)
{
	struct d3d11_context *c = This;
	for (UINT i = 0; i < NumViews && (StartSlot + i) < 8; i++) {
		if (ppSRViews && ppSRViews[i])
			c->ps_srv_idx[StartSlot + i] =
				handle_to_view_idx(ppSRViews[i]);
		else
			c->ps_srv_idx[StartSlot + i] = -1;
	}
}
static void __attribute__((ms_abi))
ctx_PSSetSamplers(void *This, UINT StartSlot, UINT NumSamplers,
		  void *const *ppSamplers)
{
	struct d3d11_context *c = This;
	for (UINT i = 0; i < NumSamplers && (StartSlot + i) < 8; i++) {
		if (ppSamplers && ppSamplers[i])
			c->ps_sampler_idx[StartSlot + i] =
				handle_to_sampler_idx(ppSamplers[i]);
		else
			c->ps_sampler_idx[StartSlot + i] = -1;
	}
}

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
	if (ppRenderTargetViews && NumViews > 0 && ppRenderTargetViews[0])
		c->rtv_idx = handle_to_view_idx(ppRenderTargetViews[0]);
	else
		c->rtv_idx = -1;

	if (pDepthStencilView)
		c->dsv_idx = handle_to_view_idx(pDepthStencilView);
	else
		c->dsv_idx = -1;
}

/* OM 상태 바인딩 */
static void __attribute__((ms_abi))
ctx_OMSetDepthStencilState(void *This, void *pState, UINT StencilRef)
{
	struct d3d11_context *c = This;
	c->ds_state_idx = pState ? handle_to_state_idx(pState) : -1;
	c->stencil_ref = StencilRef;
}

static void __attribute__((ms_abi))
ctx_OMSetBlendState(void *This, void *pState,
		    const float BlendFactor[4], UINT SampleMask)
{
	struct d3d11_context *c = This;
	(void)BlendFactor; (void)SampleMask;
	c->blend_state_idx = pState ? handle_to_state_idx(pState) : -1;
}

/* RS 상태 바인딩 */
static void __attribute__((ms_abi))
ctx_RSSetState(void *This, void *pState)
{
	struct d3d11_context *c = This;
	c->rs_state_idx = pState ? handle_to_state_idx(pState) : -1;
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

#ifdef CITC_VULKAN_ENABLED
	/* GPU도 같이 clear (Class 42에서 GPU Draw 시 사용) */
	if (use_vulkan && g_vk_rt.active)
		vk_clear_color(&g_vk, &g_vk_rt,
			       ColorRGBA[0], ColorRGBA[1],
			       ColorRGBA[2], ColorRGBA[3]);
#endif
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

	/* SwapChain의 GetBuffer()가 반환한 포인터일 수 있음 */
	if (idx < 0)
		idx = dxgi_get_swapchain_resource_idx(pResource);

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

/* UpdateSubresource — USAGE_DEFAULT 리소스에 CPU 데이터 복사 */
static void __attribute__((ms_abi))
ctx_UpdateSubresource(void *This, void *pDstResource, UINT DstSubresource,
		      void *pDstBox, const void *pSrcData,
		      UINT SrcRowPitch, UINT SrcDepthPitch)
{
	(void)This; (void)DstSubresource; (void)pDstBox;
	(void)SrcRowPitch; (void)SrcDepthPitch;
	if (!pSrcData) return;

	int idx = handle_to_resource_idx(pDstResource);
	if (idx < 0) return;

	struct d3d_resource *r = &resource_table[idx];
	if (r->data && r->size > 0)
		memcpy(r->data, pSrcData, r->size);
}

/* ClearDepthStencilView — 깊이/스텐실 버퍼 초기화 */
static void __attribute__((ms_abi))
ctx_ClearDepthStencilView(void *This, void *pDSView,
			  UINT ClearFlags, float Depth, uint8_t Stencil)
{
	(void)This; (void)Stencil;
	int vidx = handle_to_view_idx(pDSView);
	if (vidx < 0) return;

	int ridx = view_table[vidx].resource_idx;
	if (ridx < 0 || ridx >= MAX_D3D_RESOURCES) return;

	struct d3d_resource *r = &resource_table[ridx];
	if ((ClearFlags & D3D11_CLEAR_DEPTH) && r->depth) {
		int count = r->width * r->height;
		for (int i = 0; i < count; i++)
			r->depth[i] = Depth;
	}
}

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

/*
 * 4x4 행렬 × float4 벡터 곱셈
 *
 * D3D11 HLSL: mul(vector, matrix) = row-vector × matrix.
 * C에서 행렬은 row-major 순서로 저장:
 *   m[0..3] = row0, m[4..7] = row1, ...
 * mul(v, M) = { dot(v, col0), dot(v, col1), dot(v, col2), dot(v, col3) }
 */
static void mat4_mul_vec4(const float m[16], const float v[4], float out[4])
{
	for (int c = 0; c < 4; c++) {
		out[c] = 0.0f;
		for (int r = 0; r < 4; r++)
			out[c] += v[r] * m[r * 4 + c];
	}
}

struct sw_vertex {
	float pos[4];      /* x, y, z, w (NDC / clip space) */
	float color[4];    /* r, g, b, a */
	float texcoord[2]; /* u, v */
	int has_texcoord;
};

/* edge function: 점 P가 엣지 AB의 어느 쪽에 있는지 */
static float edge_func(float ax, float ay, float bx, float by,
		       float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* 텍스처 주소 모드 적용 */
static float apply_address_mode(float coord, D3D11_TEXTURE_ADDRESS_MODE mode)
{
	switch (mode) {
	case D3D11_TEXTURE_ADDRESS_WRAP:
		coord = coord - floorf(coord); /* fmod → [0,1) */
		break;
	case D3D11_TEXTURE_ADDRESS_MIRROR: {
		float t = coord - floorf(coord);
		int period = (int)floorf(coord);
		if (period & 1) t = 1.0f - t;
		coord = t;
		break;
	}
	case D3D11_TEXTURE_ADDRESS_CLAMP:
	default:
		if (coord < 0.0f) coord = 0.0f;
		if (coord > 1.0f) coord = 1.0f;
		break;
	}
	return coord;
}

/* UV → 텍셀 샘플링 (POINT 필터링) */
static void sample_texture(const struct d3d_resource *tex,
			   const D3D11_SAMPLER_DESC *samp,
			   float u, float v, float out[4])
{
	if (!tex || !tex->pixels) {
		out[0] = out[1] = out[2] = out[3] = 1.0f;
		return;
	}

	D3D11_TEXTURE_ADDRESS_MODE addr_u = D3D11_TEXTURE_ADDRESS_CLAMP;
	D3D11_TEXTURE_ADDRESS_MODE addr_v = D3D11_TEXTURE_ADDRESS_CLAMP;
	if (samp) {
		addr_u = samp->AddressU;
		addr_v = samp->AddressV;
	}

	u = apply_address_mode(u, addr_u);
	v = apply_address_mode(v, addr_v);

	int tx = (int)(u * (tex->width  - 1) + 0.5f);
	int ty = (int)(v * (tex->height - 1) + 0.5f);
	if (tx < 0) tx = 0;
	if (tx >= tex->width)  tx = tex->width - 1;
	if (ty < 0) ty = 0;
	if (ty >= tex->height) ty = tex->height - 1;

	uint32_t pixel = tex->pixels[ty * tex->width + tx];
	/* XRGB8888 → float4 */
	out[0] = (float)((pixel >> 16) & 0xFF) / 255.0f;
	out[1] = (float)((pixel >>  8) & 0xFF) / 255.0f;
	out[2] = (float)((pixel >>  0) & 0xFF) / 255.0f;
	out[3] = 1.0f;
}

/* 래스터라이저 파라미터 */
struct raster_params {
	struct d3d_resource *rt;
	const D3D11_VIEWPORT *vp;
	/* 깊이 테스트 */
	float *depth_buf;          /* NULL이면 깊이 테스트 안함 */
	int depth_enable;
	int depth_write;
	D3D11_COMPARISON_FUNC depth_func;
	/* 컬링 */
	D3D11_CULL_MODE cull_mode;
	/* 텍스처 */
	const struct d3d_resource *texture;  /* NULL이면 텍스처 없음 */
	const D3D11_SAMPLER_DESC *sampler;   /* NULL이면 기본 */
	/* PS 셰이더 VM */
	const struct dxbc_info *ps_dxbc;     /* NULL이면 고정 함수 */
	const float *ps_cb[4];              /* PS 상수 버퍼 */
	int ps_cb_size[4];
};

/* 비교 함수 평가 */
static int depth_compare(D3D11_COMPARISON_FUNC func, float src, float dst)
{
	switch (func) {
	case D3D11_COMPARISON_NEVER:         return 0;
	case D3D11_COMPARISON_LESS:          return src < dst;
	case D3D11_COMPARISON_EQUAL:         return fabsf(src - dst) < 1e-6f;
	case D3D11_COMPARISON_LESS_EQUAL:    return src <= dst;
	case D3D11_COMPARISON_GREATER:       return src > dst;
	case D3D11_COMPARISON_NOT_EQUAL:     return fabsf(src - dst) >= 1e-6f;
	case D3D11_COMPARISON_GREATER_EQUAL: return src >= dst;
	case D3D11_COMPARISON_ALWAYS:        return 1;
	default:                             return 1;
	}
}

static void rasterize_triangle(const struct raster_params *p,
			       const struct sw_vertex v[3])
{
	if (!p->rt || !p->rt->pixels) return;

	int rt_w = p->rt->width;
	int rt_h = p->rt->height;

	/* NDC(-1~1) → 스크린 좌표 변환 */
	float sx[3], sy[3];
	for (int i = 0; i < 3; i++) {
		sx[i] = p->vp->TopLeftX + (v[i].pos[0] + 1.0f) * 0.5f * p->vp->Width;
		sy[i] = p->vp->TopLeftY + (1.0f - v[i].pos[1]) * 0.5f * p->vp->Height;
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

	/* 삼각형 전체 면적 (2배) — 부호로 앞/뒷면 판별 */
	float area = edge_func(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
	if (fabsf(area) < 0.001f) return; /* 퇴화 삼각형 */

	/* 컬링: area > 0 = CW (기본 앞면), area < 0 = CCW (뒷면) */
	if (p->cull_mode == D3D11_CULL_BACK && area < 0)
		return;
	if (p->cull_mode == D3D11_CULL_FRONT && area > 0)
		return;

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

			/* 깊이 보간 + 테스트 */
			if (p->depth_enable && p->depth_buf) {
				float z = b0 * v[0].pos[2] + b1 * v[1].pos[2]
					+ b2 * v[2].pos[2];
				int pi = y * rt_w + x;
				if (!depth_compare(p->depth_func, z,
						   p->depth_buf[pi]))
					continue;
				if (p->depth_write)
					p->depth_buf[pi] = z;
			}

			/* 색상 보간 */
			float cr = b0 * v[0].color[0] + b1 * v[1].color[0] + b2 * v[2].color[0];
			float cg = b0 * v[0].color[1] + b1 * v[1].color[1] + b2 * v[2].color[1];
			float cb_c = b0 * v[0].color[2] + b1 * v[1].color[2] + b2 * v[2].color[2];

			/* PS VM 실행 (있으면 고정 함수 대체) */
			if (p->ps_dxbc && p->ps_dxbc->valid) {
				struct shader_vm ps_vm;
				memset(&ps_vm, 0, sizeof(ps_vm));
				/* PS 입력: 보간된 VS 출력
				 * v0 = 색상 (기존 PS 호환)
				 * v1 = 색상 (VS o1→PS v1 매핑) */
				ps_vm.inputs[0][0] = cr;
				ps_vm.inputs[0][1] = cg;
				ps_vm.inputs[0][2] = cb_c;
				ps_vm.inputs[0][3] = 1.0f;
				ps_vm.inputs[1][0] = cr;
				ps_vm.inputs[1][1] = cg;
				ps_vm.inputs[1][2] = cb_c;
				ps_vm.inputs[1][3] = 1.0f;
				for (int ci = 0; ci < 4; ci++) {
					ps_vm.cb[ci] = p->ps_cb[ci];
					ps_vm.cb_size[ci] = p->ps_cb_size[ci];
				}
				if (shader_vm_execute(&ps_vm, p->ps_dxbc) == 0) {
					cr = ps_vm.outputs[0][0];
					cg = ps_vm.outputs[0][1];
					cb_c = ps_vm.outputs[0][2];
				}
			} else {
				/* 텍스처 샘플링 (있으면 색상과 modulate) */
				if (p->texture && v[0].has_texcoord) {
					float tu = b0 * v[0].texcoord[0] + b1 * v[1].texcoord[0]
						 + b2 * v[2].texcoord[0];
					float tv = b0 * v[0].texcoord[1] + b1 * v[1].texcoord[1]
						 + b2 * v[2].texcoord[1];
					float tex_color[4];
					sample_texture(p->texture, p->sampler,
						       tu, tv, tex_color);
					cr *= tex_color[0];
					cg *= tex_color[1];
					cb_c *= tex_color[2];
				}
			}

			float rgba[4] = { cr, cg, cb_c, 1.0f };
			p->rt->pixels[y * rt_w + x] = float4_to_xrgb(rgba);
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
static void read_float2(const uint8_t *base, int format, float out[2])
{
	(void)format;
	const float *f = (const float *)base;
	out[0] = f[0]; out[1] = f[1];
}

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

/* 컨텍스트에서 래스터 파라미터 구성 */
static void build_raster_params(struct d3d11_context *c,
				struct d3d_resource *rt,
				struct raster_params *p)
{
	p->rt = rt;
	p->vp = &c->viewport;

	/* 깊이 테스트 설정 */
	p->depth_buf = NULL;
	p->depth_enable = 0;
	p->depth_write = 0;
	p->depth_func = D3D11_COMPARISON_LESS;

	if (c->dsv_idx >= 0) {
		int ds_ridx = view_table[c->dsv_idx].resource_idx;
		if (ds_ridx >= 0) {
			struct d3d_resource *ds = &resource_table[ds_ridx];
			if (ds->depth)
				p->depth_buf = ds->depth;
		}
	}

	if (c->ds_state_idx >= 0) {
		struct d3d_state *s = &state_table[c->ds_state_idx];
		if (s->type == D3D_STATE_DEPTH_STENCIL) {
			p->depth_enable = s->ds.DepthEnable;
			p->depth_write = (s->ds.DepthWriteMask ==
					  D3D11_DEPTH_WRITE_MASK_ALL);
			p->depth_func = s->ds.DepthFunc;
		}
	}

	/* 컬링 설정 */
	p->cull_mode = D3D11_CULL_NONE; /* 기본: 컬링 없음 */
	if (c->rs_state_idx >= 0) {
		struct d3d_state *s = &state_table[c->rs_state_idx];
		if (s->type == D3D_STATE_RASTERIZER)
			p->cull_mode = s->rs.CullMode;
	}

	/* 텍스처 설정 */
	p->texture = NULL;
	p->sampler = NULL;
	if (c->ps_srv_idx[0] >= 0) {
		int srv_ridx = view_table[c->ps_srv_idx[0]].resource_idx;
		if (srv_ridx >= 0)
			p->texture = &resource_table[srv_ridx];
	}
	if (c->ps_sampler_idx[0] >= 0)
		p->sampler = &sampler_table[c->ps_sampler_idx[0]].desc;

	/* PS 셰이더 VM 설정 */
	p->ps_dxbc = NULL;
	memset(p->ps_cb, 0, sizeof(p->ps_cb));
	memset(p->ps_cb_size, 0, sizeof(p->ps_cb_size));
	if (c->ps_idx >= 0 && shader_table[c->ps_idx].dxbc.valid)
		p->ps_dxbc = &shader_table[c->ps_idx].dxbc;
	/* PS CB 바인딩 */
	for (int i = 0; i < 4 && i < 8; i++) {
		if (c->ps_cb_idx[i] >= 0) {
			struct d3d_resource *r =
				&resource_table[c->ps_cb_idx[i]];
			if (r->data) {
				p->ps_cb[i] = (const float *)r->data;
				p->ps_cb_size[i] = (int)r->size;
			}
		}
	}
}

/* ============================================================
 * Vulkan GPU Draw 헬퍼 (Class 44)
 * ============================================================
 *
 * 바인딩된 VS/PS의 SPIR-V로 파이프라인 생성(캐시),
 * VB 데이터 업로드, CB(UBO) 업로드 후 GPU Draw 실행.
 * 성공 시 1 반환, 실패 시 0 (SW fallback).
 */
#ifdef CITC_VULKAN_ENABLED
static int vk_gpu_draw(struct d3d11_context *c,
                       const uint8_t *vb_data, UINT vb_size,
                       UINT vertex_stride,
                       UINT vertex_count, UINT start_vertex,
                       const uint8_t *ib_data, UINT ib_size,
                       UINT index_count, int ib_r16,
                       int rt_width, int rt_height)
{
	(void)start_vertex; /* TODO: VB 오프셋 지원 */

	if (!use_vulkan || !g_vk_rt.active)
		return 0;

	/* VS/PS의 SPIR-V가 있어야 GPU 경로 */
	if (c->vs_idx < 0 || c->ps_idx < 0)
		return 0;
	struct d3d_shader *vs = &shader_table[c->vs_idx];
	struct d3d_shader *ps = &shader_table[c->ps_idx];
	if (!vs->spirv || !ps->spirv)
		return 0;

	/* depth test 여부 */
	int depth_test = 0;
	if (c->dsv_idx >= 0 && c->ds_state_idx >= 0) {
		struct d3d_state *s = &state_table[c->ds_state_idx];
		if (s->type == D3D_STATE_DEPTH_STENCIL && s->ds.DepthEnable)
			depth_test = 1;
	}

	/* CB(UBO) 여부 */
	int has_ubo = (c->vs_cb_idx[0] >= 0);

	/* 정점 속성 수 결정 */
	int num_attrs = 1; /* 최소 pos */
	if (c->input_layout_idx >= 0) {
		struct d3d_input_layout *layout = &layout_table[c->input_layout_idx];
		for (int i = 0; i < layout->num_elements; i++) {
			if (strcmp(layout->elements[i].SemanticName, "COLOR") == 0)
				num_attrs = num_attrs < 2 ? 2 : num_attrs;
			else if (strcmp(layout->elements[i].SemanticName, "TEXCOORD") == 0)
				num_attrs = num_attrs < 3 ? 3 : num_attrs;
		}
	}

	/* 파이프라인 캐시 lookup */
	struct vk_cached_pipeline *cp = vk_cache_find(&g_vk_pcache,
		vs->spirv, ps->spirv, depth_test);

	if (!cp) {
		/* 새 파이프라인 생성 */
		cp = vk_cache_insert(&g_vk_pcache);
		if (!cp) return 0; /* 캐시 꽉 참 */

		if (vk_create_user_pipeline(&g_vk, &g_vk_rt,
				vs->spirv, vs->spirv_size,
				ps->spirv, ps->spirv_size,
				vertex_stride, num_attrs,
				has_ubo, depth_test,
				&cp->pipeline, &cp->layout,
				&cp->ds_layout, &cp->ds_pool) != 0) {
			g_vk_pcache.count--;
			return 0; /* 파이프라인 생성 실패 → SW fallback */
		}
		cp->vs_spirv = vs->spirv;
		cp->ps_spirv = ps->spirv;
		cp->depth_test = depth_test;
	}

	/* VB 업로드 */
	VkDeviceSize needed_vb = (VkDeviceSize)vb_size;
	if (!g_vk_vb.buffer || g_vk_vb.size < needed_vb) {
		if (g_vk_vb.buffer) vk_destroy_buffer(&g_vk, &g_vk_vb);
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if (vk_create_buffer(&g_vk, &g_vk_vb, needed_vb, usage,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
			return 0;
	}
	vk_upload_buffer(&g_vk, &g_vk_vb, vb_data, needed_vb);

	/* IB 업로드 (indexed draw) */
	if (ib_data && ib_size > 0) {
		VkDeviceSize needed_ib = (VkDeviceSize)ib_size;
		if (!g_vk_ib.buffer || g_vk_ib.size < needed_ib) {
			if (g_vk_ib.buffer) vk_destroy_buffer(&g_vk, &g_vk_ib);
			VkBufferUsageFlags usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			if (vk_create_buffer(&g_vk, &g_vk_ib, needed_ib, usage,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
					VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
				return 0;
		}
		vk_upload_buffer(&g_vk, &g_vk_ib, ib_data, needed_ib);
	}

	/* UBO 업로드 */
	VkDescriptorSet ds = VK_NULL_HANDLE;
	if (has_ubo && cp->ds_layout) {
		struct d3d_resource *cb_res = &resource_table[c->vs_cb_idx[0]];
		if (cb_res->data && cb_res->size > 0) {
			VkDeviceSize needed_ubo = (VkDeviceSize)cb_res->size;
			if (!g_vk_ubo.buffer || g_vk_ubo.size < needed_ubo) {
				if (g_vk_ubo.buffer)
					vk_destroy_buffer(&g_vk, &g_vk_ubo);
				if (vk_create_buffer(&g_vk, &g_vk_ubo, needed_ubo,
						VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
						VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
						VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
					return 0;
			}
			vk_upload_buffer(&g_vk, &g_vk_ubo,
					 cb_res->data, needed_ubo);

			if (vk_alloc_descriptor_set(&g_vk, cp->ds_layout,
						    cp->ds_pool, &ds) == 0)
				vk_update_ubo_descriptor(&g_vk, ds, &g_vk_ubo);
		}
	}

	/* GPU Draw */
	int ok;
	if (ib_data && index_count > 0) {
		VkIndexType idx_type = ib_r16 ? VK_INDEX_TYPE_UINT16
		                              : VK_INDEX_TYPE_UINT32;
		ok = vk_draw_indexed(&g_vk, &g_vk_rt, cp->pipeline, cp->layout,
		                     &g_vk_vb, &g_vk_ib,
		                     index_count, idx_type, ds,
		                     rt_width, rt_height) == 0;
	} else {
		ok = vk_draw_full(&g_vk, &g_vk_rt, cp->pipeline, cp->layout,
		                  &g_vk_vb, vertex_count, ds,
		                  rt_width, rt_height) == 0;
	}

	return ok ? 1 : 0;
}
#endif /* CITC_VULKAN_ENABLED */

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

	/* 래스터 파라미터 구성 */
	struct raster_params rp;
	build_raster_params(c, rt, &rp);

	/* VB 데이터 */
	if (c->vb_resource_idx < 0) return;
	struct d3d_resource *vb = &resource_table[c->vb_resource_idx];
	if (!vb->data) return;

	/* InputLayout */
	if (c->input_layout_idx < 0) return;
	struct d3d_input_layout *layout = &layout_table[c->input_layout_idx];

	/* POSITION, COLOR, TEXCOORD 오프셋 찾기 */
	int pos_fmt = 0, col_fmt = 0, tc_fmt = 0;
	int pos_off = find_semantic_offset(layout, "POSITION", &pos_fmt);
	int col_off = find_semantic_offset(layout, "COLOR", &col_fmt);
	int tc_off = find_semantic_offset(layout, "TEXCOORD", &tc_fmt);

	if (pos_off < 0)
		pos_off = find_semantic_offset(layout, "SV_Position", &pos_fmt);
	if (pos_off < 0) return;

	UINT stride = c->vb_stride;
	if (stride == 0) return;

	const uint8_t *vb_data = (const uint8_t *)vb->data;

#ifdef CITC_VULKAN_ENABLED
	/* GPU 경로 (SW와 병렬 실행 — Present에서 readback) */
	vk_gpu_draw(c, (const uint8_t *)vb->data, (UINT)vb->size,
		    stride, VertexCount, StartVertexLocation,
		    NULL, 0, 0, 0, rt->width, rt->height);
#endif

	/* VS DXBC VM 사용 여부 확인 */
	int use_vs_vm = 0;
	struct dxbc_info *vs_dxbc = NULL;
	if (c->vs_idx >= 0 && shader_table[c->vs_idx].dxbc.valid) {
		vs_dxbc = &shader_table[c->vs_idx].dxbc;
		use_vs_vm = 1;
	}

	/* 삼각형 리스트 래스터라이징 */
	if (c->topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		for (UINT i = StartVertexLocation; i + 2 < StartVertexLocation + VertexCount; i += 3) {
			struct sw_vertex tri[3];

			for (int j = 0; j < 3; j++) {
				const uint8_t *v = vb_data + (i + j) * stride;

				if (use_vs_vm) {
					/* === VS VM 경로 === */
					struct shader_vm vm;
					memset(&vm, 0, sizeof(vm));

					/* 입력 레지스터 설정 (v0=POS, v1=COL, v2=TC) */
					read_float3(v + pos_off, pos_fmt,
						    vm.inputs[0]);
					vm.inputs[0][3] = 1.0f;

					if (col_off >= 0)
						read_float4(v + col_off,
							    col_fmt,
							    vm.inputs[1]);
					else {
						vm.inputs[1][0] = 1.0f;
						vm.inputs[1][1] = 1.0f;
						vm.inputs[1][2] = 1.0f;
						vm.inputs[1][3] = 1.0f;
					}

					if (tc_off >= 0) {
						read_float2(v + tc_off,
							    tc_fmt,
							    vm.inputs[2]);
					}

					/* VS CB 바인딩 */
					for (int ci = 0; ci < 4; ci++) {
						if (c->vs_cb_idx[ci] >= 0) {
							struct d3d_resource *cb2 =
								&resource_table[c->vs_cb_idx[ci]];
							if (cb2->data) {
								vm.cb[ci] = (const float *)cb2->data;
								vm.cb_size[ci] = (int)cb2->size;
							}
						}
					}

					/* VM 실행 */
					shader_vm_execute(&vm, vs_dxbc);

					/* o0 = SV_Position (perspective divide) */
					float *clip = vm.outputs[0];
					if (fabsf(clip[3]) > 1e-6f) {
						tri[j].pos[0] = clip[0] / clip[3];
						tri[j].pos[1] = clip[1] / clip[3];
						tri[j].pos[2] = clip[2] / clip[3];
						tri[j].pos[3] = clip[3];
					} else {
						memcpy(tri[j].pos, clip, 16);
					}

					/* o1 = COLOR */
					memcpy(tri[j].color, vm.outputs[1], 16);

					/* TEXCOORD */
					if (tc_off >= 0) {
						tri[j].texcoord[0] = vm.outputs[2][0];
						tri[j].texcoord[1] = vm.outputs[2][1];
						tri[j].has_texcoord = 1;
					} else {
						tri[j].texcoord[0] = 0;
						tri[j].texcoord[1] = 0;
						tri[j].has_texcoord = 0;
					}
				} else {
					/* === 고정 함수 경로 === */
					float raw_pos[4];
					read_float3(v + pos_off, pos_fmt,
						    raw_pos);
					raw_pos[3] = 1.0f;

					/* MVP 변환 */
					if (c->vs_cb_idx[0] >= 0) {
						struct d3d_resource *cb =
							&resource_table[c->vs_cb_idx[0]];
						if (cb->data && cb->size >= 64) {
							float transformed[4];
							mat4_mul_vec4(
								(const float *)cb->data,
								raw_pos, transformed);
							if (fabsf(transformed[3]) > 1e-6f) {
								tri[j].pos[0] = transformed[0] / transformed[3];
								tri[j].pos[1] = transformed[1] / transformed[3];
								tri[j].pos[2] = transformed[2] / transformed[3];
								tri[j].pos[3] = transformed[3];
							} else {
								memcpy(tri[j].pos, transformed, 16);
							}
						} else {
							memcpy(tri[j].pos, raw_pos, 16);
						}
					} else {
						memcpy(tri[j].pos, raw_pos, 16);
					}

					if (col_off >= 0)
						read_float4(v + col_off, col_fmt, tri[j].color);
					else {
						tri[j].color[0] = 1.0f;
						tri[j].color[1] = 1.0f;
						tri[j].color[2] = 1.0f;
						tri[j].color[3] = 1.0f;
					}

					if (tc_off >= 0) {
						read_float2(v + tc_off, tc_fmt, tri[j].texcoord);
						tri[j].has_texcoord = 1;
					} else {
						tri[j].texcoord[0] = tri[j].texcoord[1] = 0.0f;
						tri[j].has_texcoord = 0;
					}
				}
			}

			rasterize_triangle(&rp, tri);
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

	struct raster_params rp;
	build_raster_params(c, rt, &rp);

	if (c->vb_resource_idx < 0 || c->ib_resource_idx < 0) return;
	struct d3d_resource *vb = &resource_table[c->vb_resource_idx];
	struct d3d_resource *ib = &resource_table[c->ib_resource_idx];
	if (!vb->data || !ib->data) return;

	if (c->input_layout_idx < 0) return;
	struct d3d_input_layout *layout = &layout_table[c->input_layout_idx];

	int pos_fmt = 0, col_fmt = 0, tc_fmt = 0;
	int pos_off = find_semantic_offset(layout, "POSITION", &pos_fmt);
	int col_off = find_semantic_offset(layout, "COLOR", &col_fmt);
	int tc_off = find_semantic_offset(layout, "TEXCOORD", &tc_fmt);
	if (pos_off < 0)
		pos_off = find_semantic_offset(layout, "SV_Position", &pos_fmt);
	if (pos_off < 0) return;

	UINT stride = c->vb_stride;
	if (stride == 0) return;

	const uint8_t *vb_data = (const uint8_t *)vb->data;
	const uint8_t *ib_data = (const uint8_t *)ib->data;

#ifdef CITC_VULKAN_ENABLED
	/* GPU 경로 */
	{
		int ib_r16 = (c->ib_format == DXGI_FORMAT_R16_UINT);
		vk_gpu_draw(c, (const uint8_t *)vb->data, (UINT)vb->size,
			    stride, 0, 0,
			    (const uint8_t *)ib->data, (UINT)ib->size,
			    IndexCount, ib_r16,
			    rt->width, rt->height);
	}
#endif

	/* VS DXBC VM 사용 여부 확인 */
	int use_vs_vm = 0;
	struct dxbc_info *vs_dxbc = NULL;
	if (c->vs_idx >= 0 && shader_table[c->vs_idx].dxbc.valid) {
		vs_dxbc = &shader_table[c->vs_idx].dxbc;
		use_vs_vm = 1;
	}

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

				if (use_vs_vm) {
					/* === VS VM 경로 === */
					struct shader_vm vm;
					memset(&vm, 0, sizeof(vm));

					read_float3(v + pos_off, pos_fmt,
						    vm.inputs[0]);
					vm.inputs[0][3] = 1.0f;

					if (col_off >= 0)
						read_float4(v + col_off,
							    col_fmt,
							    vm.inputs[1]);
					else {
						vm.inputs[1][0] = 1.0f;
						vm.inputs[1][1] = 1.0f;
						vm.inputs[1][2] = 1.0f;
						vm.inputs[1][3] = 1.0f;
					}

					if (tc_off >= 0) {
						read_float2(v + tc_off,
							    tc_fmt,
							    vm.inputs[2]);
					}

					for (int ci = 0; ci < 4; ci++) {
						if (c->vs_cb_idx[ci] >= 0) {
							struct d3d_resource *cb2 =
								&resource_table[c->vs_cb_idx[ci]];
							if (cb2->data) {
								vm.cb[ci] = (const float *)cb2->data;
								vm.cb_size[ci] = (int)cb2->size;
							}
						}
					}

					shader_vm_execute(&vm, vs_dxbc);

					float *clip = vm.outputs[0];
					if (fabsf(clip[3]) > 1e-6f) {
						tri[j].pos[0] = clip[0] / clip[3];
						tri[j].pos[1] = clip[1] / clip[3];
						tri[j].pos[2] = clip[2] / clip[3];
						tri[j].pos[3] = clip[3];
					} else {
						memcpy(tri[j].pos, clip, 16);
					}

					memcpy(tri[j].color, vm.outputs[1], 16);

					if (tc_off >= 0) {
						tri[j].texcoord[0] = vm.outputs[2][0];
						tri[j].texcoord[1] = vm.outputs[2][1];
						tri[j].has_texcoord = 1;
					} else {
						tri[j].texcoord[0] = 0;
						tri[j].texcoord[1] = 0;
						tri[j].has_texcoord = 0;
					}
				} else {
					/* === 고정 함수 경로 === */
					float raw_pos[4];
					read_float3(v + pos_off, pos_fmt, raw_pos);
					raw_pos[3] = 1.0f;

					if (c->vs_cb_idx[0] >= 0) {
						struct d3d_resource *cb =
							&resource_table[c->vs_cb_idx[0]];
						if (cb->data && cb->size >= 64) {
							float transformed[4];
							mat4_mul_vec4(
								(const float *)cb->data,
								raw_pos, transformed);
							if (fabsf(transformed[3]) > 1e-6f) {
								tri[j].pos[0] = transformed[0] / transformed[3];
								tri[j].pos[1] = transformed[1] / transformed[3];
								tri[j].pos[2] = transformed[2] / transformed[3];
								tri[j].pos[3] = transformed[3];
							} else {
								memcpy(tri[j].pos, transformed, 16);
							}
						} else {
							memcpy(tri[j].pos, raw_pos, 16);
						}
					} else {
						memcpy(tri[j].pos, raw_pos, 16);
					}

					if (col_off >= 0)
						read_float4(v + col_off, col_fmt, tri[j].color);
					else {
						tri[j].color[0] = 1.0f;
						tri[j].color[1] = 1.0f;
						tri[j].color[2] = 1.0f;
						tri[j].color[3] = 1.0f;
					}

					if (tc_off >= 0) {
						read_float2(v + tc_off, tc_fmt, tri[j].texcoord);
						tri[j].has_texcoord = 1;
					} else {
						tri[j].texcoord[0] = tri[j].texcoord[1] = 0.0f;
						tri[j].has_texcoord = 0;
					}
				}
			}

			rasterize_triangle(&rp, tri);
		}
	}
}

/* PSSetConstantBuffers */
static void __attribute__((ms_abi))
ctx_PSSetConstantBuffers(void *This, UINT StartSlot, UINT NumBuffers,
			 void *const *ppConstantBuffers)
{
	struct d3d11_context *c = This;
	for (UINT i = 0; i < NumBuffers && (StartSlot + i) < 8; i++) {
		if (ppConstantBuffers && ppConstantBuffers[i])
			c->ps_cb_idx[StartSlot + i] =
				handle_to_resource_idx(ppConstantBuffers[i]);
		else
			c->ps_cb_idx[StartSlot + i] = -1;
	}
}

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
	for (int i = 0; i < 8; i++) {
		c->vs_cb_idx[i] = -1;
		c->ps_cb_idx[i] = -1;
		c->ps_srv_idx[i] = -1;
		c->ps_sampler_idx[i] = -1;
	}
	c->ds_state_idx = -1;
	c->blend_state_idx = -1;
	c->rs_state_idx = -1;
	c->stencil_ref = 0;
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
	.OMSetBlendState          = ctx_OMSetBlendState,
	.OMSetDepthStencilState   = ctx_OMSetDepthStencilState,
	.SOSetTargets             = (void *)ctx_stub,
	.DrawAuto                 = (void *)ctx_stub,
	.DrawIndexedInstancedIndirect = (void *)ctx_stub,
	.DrawInstancedIndirect    = (void *)ctx_stub,
	.Dispatch                 = (void *)ctx_stub,
	.DispatchIndirect         = (void *)ctx_stub,
	.RSSetState               = ctx_RSSetState,
	.RSSetViewports           = ctx_RSSetViewports,
	.RSSetScissorRects        = (void *)ctx_stub,
	/* Copy/Update */
	.CopySubresourceRegion    = (void *)ctx_stub,
	.CopyResource             = (void *)ctx_stub,
	.UpdateSubresource        = ctx_UpdateSubresource,
	.CopyStructureCount       = (void *)ctx_stub,
	/* Clear */
	.ClearRenderTargetView    = ctx_ClearRenderTargetView,
	.ClearUnorderedAccessViewUint  = (void *)ctx_stub,
	.ClearUnorderedAccessViewFloat = (void *)ctx_stub,
	.ClearDepthStencilView    = ctx_ClearDepthStencilView,
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
		g_context->ds_state_idx = -1;
		g_context->blend_state_idx = -1;
		g_context->rs_state_idx = -1;
		for (int i = 0; i < 8; i++) {
			g_context->vs_cb_idx[i] = -1;
			g_context->ps_cb_idx[i] = -1;
			g_context->ps_srv_idx[i] = -1;
			g_context->ps_sampler_idx[i] = -1;
		}
	}

	if (ppDevice) *ppDevice = dev;
	if (pFeatureLevel) *pFeatureLevel = dev->feature_level;
	if (ppImmediateContext) {
		*ppImmediateContext = g_context;
		g_context->ref_count++;
	}

#ifdef CITC_VULKAN_ENABLED
	/* Vulkan GPU 백엔드 초기화 시도 */
	if (!use_vulkan) {
		if (vk_load_vulkan(&g_vk) == 0 && vk_backend_init(&g_vk) == 0) {
			use_vulkan = 1;
			printf("d3d11: Vulkan GPU backend: %s\n",
			       g_vk.device_name);
		} else {
			printf("d3d11: Vulkan not available, SW fallback\n");
		}
	}
#endif

#ifdef CITC_VULKAN_ENABLED
	printf("d3d11: Device created (FL %x, %s)\n",
	       dev->feature_level,
	       use_vulkan ? "Vulkan GPU" : "software rasterizer");
#else
	printf("d3d11: Device created (FL %x, software rasterizer)\n",
	       dev->feature_level);
#endif
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
 * Vulkan 렌더 타깃 / Readback (d3d11.h 공개 API)
 * ============================================================ */

void d3d11_vk_create_rt(int width, int height)
{
#ifdef CITC_VULKAN_ENABLED
	if (!use_vulkan) return;
	if (g_vk_rt.active)
		vk_destroy_render_target(&g_vk, &g_vk_rt);
	if (vk_create_render_target(&g_vk, &g_vk_rt,
				    (uint32_t)width, (uint32_t)height) == 0)
		printf("d3d11: Vulkan render target %dx%d created\n",
		       width, height);
#else
	(void)width; (void)height;
#endif
}

int d3d11_vk_readback(uint32_t *pixels, int width, int height)
{
#ifdef CITC_VULKAN_ENABLED
	if (!use_vulkan || !g_vk_rt.active) return 0;
	if ((int)g_vk_rt.width != width || (int)g_vk_rt.height != height)
		return 0;
	return vk_readback_pixels(&g_vk, &g_vk_rt, pixels) == 0 ? 1 : 0;
#else
	(void)pixels; (void)width; (void)height;
	return 0;
#endif
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
