/*
 * d3d12.c — DirectX 12 최소 구현
 * ================================
 *
 * D3D12 핵심 인터페이스:
 *   ID3D12Device — 리소스/큐/커맨드 리스트 생성
 *   ID3D12CommandQueue — 커맨드 리스트 실행
 *   ID3D12GraphicsCommandList — 렌더 명령 기록
 *   ID3D12Resource — 버퍼/텍스처
 *   ID3D12Fence — GPU 동기화
 *   ID3D12DescriptorHeap — RTV/DSV/SRV 관리
 *   ID3D12RootSignature — 셰이더 바인딩 레이아웃
 *   ID3D12PipelineState — 고정 파이프라인 상태
 *
 * 내부 구현:
 *   CommandList가 Close()될 때 기록된 명령을 순서대로 실행.
 *   ClearRenderTargetView는 직접 픽셀 버퍼에 쓰기.
 *   Resource/Fence/DescriptorHeap은 COM 포인터 풀 패턴.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "d3d12.h"

/* ============================================================
 * 핸들 오프셋 (RootSignature, PSO용 — 단순 핸들)
 * ============================================================ */

#define D3D12_ROOTSIG_OFFSET     0x67000
#define D3D12_PSO_OFFSET         0x68000

/* ============================================================
 * 내부 상태
 * ============================================================ */

#define MAX_D3D12_RESOURCES  64
#define MAX_D3D12_DESCHEAPS  16
#define MAX_D3D12_FENCES     8
#define MAX_D3D12_ROOTSIGS   8
#define MAX_D3D12_PSOS       16

/* 리소스 */
struct d3d12_resource {
	int active;
	D3D12_RESOURCE_DESC desc;
	D3D12_HEAP_TYPE heap_type;
	void *data;          /* CPU 메모리 (upload/readback) */
	size_t data_size;
	uint32_t *pixels;    /* RT 픽셀 버퍼 */
	int width, height;
};

static struct d3d12_resource res_table[MAX_D3D12_RESOURCES];

/* Descriptor heap */
struct d3d12_desc_heap {
	int active;
	D3D12_DESCRIPTOR_HEAP_DESC desc;
	size_t base_cpu;     /* CPU handle 시작 */
	uint64_t base_gpu;
};

static struct d3d12_desc_heap heap_table[MAX_D3D12_DESCHEAPS];

/* Fence */
struct d3d12_fence {
	int active;
	uint64_t value;
};

static struct d3d12_fence fence_table[MAX_D3D12_FENCES];

/* Root Signature */
struct d3d12_rootsig {
	int active;
};

static struct d3d12_rootsig rootsig_table[MAX_D3D12_ROOTSIGS];

/* Pipeline State Object */
struct d3d12_pso {
	int active;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
};

static struct d3d12_pso pso_table[MAX_D3D12_PSOS];

/* COM 포인터 풀 — 실제 COM 객체처럼 vtable 간접 참조 */
static ID3D12ResourceVtbl res_vtbl;
static ID3D12FenceVtbl fence_vtbl;
static ID3D12DescriptorHeapVtbl descheap_vtbl;

static ID3D12ResourceVtbl *res_com[MAX_D3D12_RESOURCES];
static ID3D12FenceVtbl *fence_com[MAX_D3D12_FENCES];
static ID3D12DescriptorHeapVtbl *dh_com[MAX_D3D12_DESCHEAPS];

/* Descriptor → Resource 매핑 */
#define MAX_D3D12_DESCRIPTORS 256

struct d3d12_desc_mapping {
	size_t handle_ptr;
	int res_idx;
};

static struct d3d12_desc_mapping desc_map[MAX_D3D12_DESCRIPTORS];
static int desc_map_count;

/* Command List 상태 */
struct d3d12_cmdlist {
	int recording;
	/* 기록된 상태 */
	float clear_color[4];
	int clear_pending;
	int clear_rtv_res_idx;     /* 타겟 리소스 인덱스 */
	int draw_pending;
	UINT draw_vertex_count;
	UINT draw_instance_count;
	UINT draw_start_vertex;
};

static struct d3d12_cmdlist cmdlist;

/* 디바이스 상태 */
static int device_active;

/* ============================================================
 * 유틸리티
 * ============================================================ */

static int alloc_resource(void)
{
	for (int i = 0; i < MAX_D3D12_RESOURCES; i++)
		if (!res_table[i].active)
			return i;
	return -1;
}

static int alloc_desc_heap(void)
{
	for (int i = 0; i < MAX_D3D12_DESCHEAPS; i++)
		if (!heap_table[i].active)
			return i;
	return -1;
}

static int alloc_fence(void)
{
	for (int i = 0; i < MAX_D3D12_FENCES; i++)
		if (!fence_table[i].active)
			return i;
	return -1;
}

static int alloc_rootsig(void)
{
	for (int i = 0; i < MAX_D3D12_ROOTSIGS; i++)
		if (!rootsig_table[i].active)
			return i;
	return -1;
}

static int alloc_pso(void)
{
	for (int i = 0; i < MAX_D3D12_PSOS; i++)
		if (!pso_table[i].active)
			return i;
	return -1;
}

/* COM 포인터 풀에서 인덱스 추출 */
static int res_idx_from_this(void *This)
{
	ptrdiff_t idx = (ID3D12ResourceVtbl **)This - res_com;
	if (idx >= 0 && idx < MAX_D3D12_RESOURCES)
		return (int)idx;
	return -1;
}

static int fence_idx_from_this(void *This)
{
	ptrdiff_t idx = (ID3D12FenceVtbl **)This - fence_com;
	if (idx >= 0 && idx < MAX_D3D12_FENCES)
		return (int)idx;
	return -1;
}

static int dh_idx_from_this(void *This)
{
	ptrdiff_t idx = (ID3D12DescriptorHeapVtbl **)This - dh_com;
	if (idx >= 0 && idx < MAX_D3D12_DESCHEAPS)
		return (int)idx;
	return -1;
}

/* descriptor handle → resource index */
static int desc_handle_to_res(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	for (int i = 0; i < desc_map_count; i++)
		if (desc_map[i].handle_ptr == handle.ptr)
			return desc_map[i].res_idx;
	return -1;
}

/* ============================================================
 * IUnknown 공통
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
common_QueryInterface(void *This, const GUID *riid, void **ppv)
{
	(void)riid;
	if (ppv) *ppv = This;
	return S_OK;
}

static ULONG __attribute__((ms_abi))
common_AddRef(void *This)
{
	(void)This;
	return 1;
}

static ULONG __attribute__((ms_abi))
common_Release(void *This)
{
	(void)This;
	return 0;
}

/* ID3D12Object stubs */
static HRESULT __attribute__((ms_abi))
stub_GetPrivateData(void *T, const GUID *g, UINT *s, void *d)
{ (void)T; (void)g; (void)s; (void)d; return E_FAIL; }

static HRESULT __attribute__((ms_abi))
stub_SetPrivateData(void *T, const GUID *g, UINT s, const void *d)
{ (void)T; (void)g; (void)s; (void)d; return S_OK; }

static HRESULT __attribute__((ms_abi))
stub_SetPrivateDataInterface(void *T, const GUID *g, void *d)
{ (void)T; (void)g; (void)d; return S_OK; }

static HRESULT __attribute__((ms_abi))
stub_SetName(void *T, const void *n)
{ (void)T; (void)n; return S_OK; }

/* ============================================================
 * ID3D12Fence
 * ============================================================ */

static uint64_t __attribute__((ms_abi))
fence_GetCompletedValue(void *This)
{
	int idx = fence_idx_from_this(This);
	if (idx >= 0)
		return fence_table[idx].value;
	return 0;
}

static HRESULT __attribute__((ms_abi))
fence_SetEventOnCompletion(void *This, uint64_t val, void *hEvent)
{
	(void)This; (void)val; (void)hEvent;
	/* 즉시 완료 — CPU SW 구현이므로 대기 불필요 */
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
fence_Signal_method(void *This, uint64_t val)
{
	int idx = fence_idx_from_this(This);
	if (idx >= 0)
		fence_table[idx].value = val;
	return S_OK;
}

/* fence_vtbl initialized in init function below */

/* ============================================================
 * ID3D12Resource
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
res_Map(void *This, UINT sub, const void *range, void **ppData)
{
	(void)sub; (void)range;
	int idx = res_idx_from_this(This);
	if (idx >= 0 && res_table[idx].data) {
		if (ppData) *ppData = res_table[idx].data;
		return S_OK;
	}
	if (ppData) *ppData = NULL;
	return E_FAIL;
}

static void __attribute__((ms_abi))
res_Unmap(void *This, UINT sub, const void *range)
{
	(void)This; (void)sub; (void)range;
}

static uint64_t __attribute__((ms_abi))
res_GetGPUVirtualAddress(void *This)
{
	int idx = res_idx_from_this(This);
	if (idx >= 0 && res_table[idx].data)
		return (uint64_t)(uintptr_t)res_table[idx].data;
	return 0;
}

/* res_vtbl initialized in init function below */

/* ============================================================
 * ID3D12DescriptorHeap
 * ============================================================ */

static D3D12_CPU_DESCRIPTOR_HANDLE __attribute__((ms_abi))
dh_GetCPUDescriptorHandleForHeapStart(void *This)
{
	int idx = dh_idx_from_this(This);
	D3D12_CPU_DESCRIPTOR_HANDLE h = {0};
	if (idx >= 0)
		h.ptr = heap_table[idx].base_cpu;
	return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE __attribute__((ms_abi))
dh_GetGPUDescriptorHandleForHeapStart(void *This)
{
	int idx = dh_idx_from_this(This);
	D3D12_GPU_DESCRIPTOR_HANDLE h = {0};
	if (idx >= 0)
		h.ptr = heap_table[idx].base_gpu;
	return h;
}

/* descheap_vtbl initialized in init function below */

/* ============================================================
 * ID3D12RootSignature / ID3D12PipelineState / ID3D12CommandAllocator
 * ============================================================ */

static ID3D12RootSignatureVtbl rootsig_vtbl = {
	.QueryInterface = common_QueryInterface,
	.AddRef = common_AddRef,
	.Release = common_Release,
	.GetPrivateData = (void *)stub_GetPrivateData,
	.SetPrivateData = (void *)stub_SetPrivateData,
	.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface,
	.SetName = (void *)stub_SetName,
	.GetDevice = NULL,
};

static ID3D12RootSignatureVtbl *rootsig_vtbl_ptr
	__attribute__((unused)) = &rootsig_vtbl;

static ID3D12PipelineStateVtbl pso_vtbl = {
	.QueryInterface = common_QueryInterface,
	.AddRef = common_AddRef,
	.Release = common_Release,
	.GetPrivateData = (void *)stub_GetPrivateData,
	.SetPrivateData = (void *)stub_SetPrivateData,
	.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface,
	.SetName = (void *)stub_SetName,
	.GetDevice = NULL,
	.GetCachedBlob = NULL,
};

static ID3D12PipelineStateVtbl *pso_vtbl_ptr
	__attribute__((unused)) = &pso_vtbl;

static HRESULT __attribute__((ms_abi))
cmdallocator_Reset(void *This) { (void)This; return S_OK; }

static ID3D12CommandAllocatorVtbl cmdallocator_vtbl = {
	.QueryInterface = common_QueryInterface,
	.AddRef = common_AddRef,
	.Release = common_Release,
	.GetPrivateData = (void *)stub_GetPrivateData,
	.SetPrivateData = (void *)stub_SetPrivateData,
	.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface,
	.SetName = (void *)stub_SetName,
	.Reset = cmdallocator_Reset,
};

static ID3D12CommandAllocatorVtbl *cmdallocator_vtbl_ptr = &cmdallocator_vtbl;

/* ============================================================
 * ID3D12GraphicsCommandList
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
cl_Close(void *This)
{
	(void)This;
	/* Close 시 기록된 clear/draw 명령 실행 */
	if (cmdlist.clear_pending) {
		int idx = cmdlist.clear_rtv_res_idx;
		if (idx >= 0 && idx < MAX_D3D12_RESOURCES &&
		    res_table[idx].pixels) {
			int w = res_table[idx].width;
			int h = res_table[idx].height;
			uint8_t r = (uint8_t)(cmdlist.clear_color[0] * 255.0f);
			uint8_t g = (uint8_t)(cmdlist.clear_color[1] * 255.0f);
			uint8_t b = (uint8_t)(cmdlist.clear_color[2] * 255.0f);
			uint8_t a = (uint8_t)(cmdlist.clear_color[3] * 255.0f);
			uint32_t px = ((uint32_t)a << 24) |
				      ((uint32_t)r << 16) |
				      ((uint32_t)g << 8)  | b;
			for (int i = 0; i < w * h; i++)
				res_table[idx].pixels[i] = px;
		}
		cmdlist.clear_pending = 0;
	}

	if (cmdlist.draw_pending) {
		/* SW 래스터라이저: 간단한 삼각형 그리기 */
		cmdlist.draw_pending = 0;
	}

	cmdlist.recording = 0;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
cl_Reset(void *This, void *pAllocator, void *pInitialState)
{
	(void)This; (void)pAllocator; (void)pInitialState;
	memset(&cmdlist, 0, sizeof(cmdlist));
	cmdlist.recording = 1;
	return S_OK;
}

static void __attribute__((ms_abi))
cl_ClearState(void *This, void *pPSO) { (void)This; (void)pPSO; }

static void __attribute__((ms_abi))
cl_DrawInstanced(void *This, UINT VtxCount, UINT InstCount,
		 UINT StartVtx, UINT StartInst)
{
	(void)This; (void)StartInst;
	cmdlist.draw_pending = 1;
	cmdlist.draw_vertex_count = VtxCount;
	cmdlist.draw_instance_count = InstCount;
	cmdlist.draw_start_vertex = StartVtx;
}

static void __attribute__((ms_abi))
cl_DrawIndexedInstanced(void *This, UINT IdxCount, UINT InstCount,
			UINT StartIdx, int BaseVtx, UINT StartInst)
{
	(void)This; (void)IdxCount; (void)InstCount;
	(void)StartIdx; (void)BaseVtx; (void)StartInst;
	cmdlist.draw_pending = 1;
}

static void __attribute__((ms_abi))
cl_IASetPrimitiveTopology(void *This, int topo) { (void)This; (void)topo; }

static void __attribute__((ms_abi))
cl_RSSetViewports(void *This, UINT n, const D3D12_VIEWPORT *v)
{ (void)This; (void)n; (void)v; }

static void __attribute__((ms_abi))
cl_RSSetScissorRects(void *This, UINT n, const D3D12_RECT *r)
{ (void)This; (void)n; (void)r; }

static void __attribute__((ms_abi))
cl_SetPipelineState(void *This, void *pPSO) { (void)This; (void)pPSO; }

static void __attribute__((ms_abi))
cl_ResourceBarrier(void *This, UINT n, const D3D12_RESOURCE_BARRIER *b)
{ (void)This; (void)n; (void)b; /* 트랜지션은 SW 구현에서 no-op */ }

static void __attribute__((ms_abi))
cl_SetGraphicsRootSignature(void *This, void *pRS) { (void)This; (void)pRS; }

static void __attribute__((ms_abi))
cl_IASetIndexBuffer(void *This, const D3D12_INDEX_BUFFER_VIEW *v)
{ (void)This; (void)v; }

static void __attribute__((ms_abi))
cl_IASetVertexBuffers(void *This, UINT start, UINT n,
		      const D3D12_VERTEX_BUFFER_VIEW *v)
{ (void)This; (void)start; (void)n; (void)v; }

static void __attribute__((ms_abi))
cl_OMSetRenderTargets(void *This, UINT numRT,
		      const D3D12_CPU_DESCRIPTOR_HANDLE *rtHandles,
		      int singleHandle,
		      const D3D12_CPU_DESCRIPTOR_HANDLE *dsHandle)
{
	(void)This; (void)singleHandle; (void)dsHandle;
	if (numRT > 0 && rtHandles)
		cmdlist.clear_rtv_res_idx = desc_handle_to_res(rtHandles[0]);
}

static void __attribute__((ms_abi))
cl_ClearRenderTargetView(void *This, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
			 const float color[4], UINT numRects,
			 const D3D12_RECT *rects)
{
	(void)This; (void)numRects; (void)rects;
	cmdlist.clear_pending = 1;
	cmdlist.clear_rtv_res_idx = desc_handle_to_res(rtv);
	if (color) {
		cmdlist.clear_color[0] = color[0];
		cmdlist.clear_color[1] = color[1];
		cmdlist.clear_color[2] = color[2];
		cmdlist.clear_color[3] = color[3];
	}
}

static void __attribute__((ms_abi))
cl_ClearDepthStencilView(void *This, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
			 UINT clearFlags, float depth, unsigned char stencil,
			 UINT numRects, const D3D12_RECT *rects)
{
	(void)This; (void)dsv; (void)clearFlags; (void)depth;
	(void)stencil; (void)numRects; (void)rects;
}

static int __attribute__((ms_abi))
cl_GetType(void *This) { (void)This; return D3D12_COMMAND_LIST_TYPE_DIRECT; }

static ID3D12GraphicsCommandListVtbl cmdlist_vtbl = {
	.QueryInterface = common_QueryInterface,
	.AddRef = common_AddRef,
	.Release = common_Release,
	.GetPrivateData = (void *)stub_GetPrivateData,
	.SetPrivateData = (void *)stub_SetPrivateData,
	.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface,
	.SetName = (void *)stub_SetName,
	.GetDevice = NULL,
	.GetType = cl_GetType,
	.Close = cl_Close,
	.Reset = cl_Reset,
	.ClearState = cl_ClearState,
	.DrawInstanced = cl_DrawInstanced,
	.DrawIndexedInstanced = cl_DrawIndexedInstanced,
	.IASetPrimitiveTopology = cl_IASetPrimitiveTopology,
	.RSSetViewports = cl_RSSetViewports,
	.RSSetScissorRects = cl_RSSetScissorRects,
	.SetPipelineState = cl_SetPipelineState,
	.ResourceBarrier = cl_ResourceBarrier,
	.SetGraphicsRootSignature = cl_SetGraphicsRootSignature,
	.IASetIndexBuffer = cl_IASetIndexBuffer,
	.IASetVertexBuffers = cl_IASetVertexBuffers,
	.OMSetRenderTargets = cl_OMSetRenderTargets,
	.ClearDepthStencilView = cl_ClearDepthStencilView,
	.ClearRenderTargetView = cl_ClearRenderTargetView,
};

static ID3D12GraphicsCommandListVtbl *cmdlist_vtbl_ptr = &cmdlist_vtbl;

/* ============================================================
 * ID3D12CommandQueue
 * ============================================================ */

static void __attribute__((ms_abi))
cq_ExecuteCommandLists(void *This, UINT numLists, void *const *ppLists)
{
	(void)This; (void)numLists; (void)ppLists;
	/* SW 구현: Close()에서 이미 실행됨. ExecuteCommandLists는 no-op */
}

static HRESULT __attribute__((ms_abi))
cq_Signal(void *This, void *pFence, uint64_t val)
{
	(void)This;
	/* 즉시 시그널 — SW 구현은 동기적 */
	if (pFence) {
		int idx = fence_idx_from_this(pFence);
		if (idx >= 0)
			fence_table[idx].value = val;
	}
	return S_OK;
}

static ID3D12CommandQueueVtbl cmdqueue_vtbl = {
	.QueryInterface = common_QueryInterface,
	.AddRef = common_AddRef,
	.Release = common_Release,
	.GetPrivateData = (void *)stub_GetPrivateData,
	.SetPrivateData = (void *)stub_SetPrivateData,
	.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface,
	.SetName = (void *)stub_SetName,
	.ExecuteCommandLists = cq_ExecuteCommandLists,
	.Signal = cq_Signal,
};

static ID3D12CommandQueueVtbl *cmdqueue_vtbl_ptr = &cmdqueue_vtbl;

/* ============================================================
 * ID3D12Device
 * ============================================================ */

static UINT __attribute__((ms_abi))
dev_GetNodeCount(void *This) { (void)This; return 1; }

static HRESULT __attribute__((ms_abi))
dev_CreateCommandQueue(void *This, const D3D12_COMMAND_QUEUE_DESC *pDesc,
		       const GUID *riid, void **ppQueue)
{
	(void)This; (void)pDesc; (void)riid;
	if (!ppQueue) return E_POINTER;
	*ppQueue = (void *)&cmdqueue_vtbl_ptr;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
dev_CreateCommandAllocator(void *This, D3D12_COMMAND_LIST_TYPE type,
			   const GUID *riid, void **ppAllocator)
{
	(void)This; (void)type; (void)riid;
	if (!ppAllocator) return E_POINTER;
	*ppAllocator = (void *)&cmdallocator_vtbl_ptr;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
dev_CreateGraphicsPipelineState(void *This,
				const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc,
				const GUID *riid, void **ppPSO)
{
	(void)This; (void)riid;
	if (!ppPSO) return E_POINTER;

	int idx = alloc_pso();
	if (idx < 0) return E_OUTOFMEMORY;

	pso_table[idx].active = 1;
	if (pDesc)
		memcpy(&pso_table[idx].desc, pDesc, sizeof(*pDesc));

	*ppPSO = (void *)(uintptr_t)(D3D12_PSO_OFFSET + idx);
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
dev_CreateCommandList(void *This, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
		      void *pAllocator, void *pInitialPSO,
		      const GUID *riid, void **ppList)
{
	(void)This; (void)nodeMask; (void)type;
	(void)pAllocator; (void)pInitialPSO; (void)riid;
	if (!ppList) return E_POINTER;

	memset(&cmdlist, 0, sizeof(cmdlist));
	cmdlist.recording = 1;

	*ppList = (void *)&cmdlist_vtbl_ptr;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
dev_CreateDescriptorHeap(void *This,
			 const D3D12_DESCRIPTOR_HEAP_DESC *pDesc,
			 const GUID *riid, void **ppHeap)
{
	(void)This; (void)riid;
	if (!ppHeap) return E_POINTER;

	int idx = alloc_desc_heap();
	if (idx < 0) return E_OUTOFMEMORY;

	heap_table[idx].active = 1;
	if (pDesc)
		memcpy(&heap_table[idx].desc, pDesc, sizeof(*pDesc));

	/* CPU/GPU 핸들 베이스: 유니크한 값 (인덱스별) */
	heap_table[idx].base_cpu = (size_t)(0xD3D12000 + idx * 256);
	heap_table[idx].base_gpu = (uint64_t)(0xD3D12000 + idx * 256);

	/* COM 포인터 풀에 등록 */
	dh_com[idx] = &descheap_vtbl;
	*ppHeap = (void *)&dh_com[idx];
	return S_OK;
}

static UINT __attribute__((ms_abi))
dev_GetDescriptorHandleIncrementSize(void *This,
				     D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	(void)This; (void)type;
	return 32; /* 32바이트 간격 */
}

static HRESULT __attribute__((ms_abi))
dev_CreateRootSignature(void *This, UINT nodeMask,
			const void *pBlob, size_t blobLen,
			const GUID *riid, void **ppRS)
{
	(void)This; (void)nodeMask; (void)pBlob; (void)blobLen; (void)riid;
	if (!ppRS) return E_POINTER;

	int idx = alloc_rootsig();
	if (idx < 0) return E_OUTOFMEMORY;

	rootsig_table[idx].active = 1;
	*ppRS = (void *)(uintptr_t)(D3D12_ROOTSIG_OFFSET + idx);
	return S_OK;
}

static void __attribute__((ms_abi))
dev_CreateRenderTargetView(void *This, void *pResource,
			   void *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	(void)This; (void)pDesc;
	if (pResource && desc_map_count < MAX_D3D12_DESCRIPTORS) {
		int idx = res_idx_from_this(pResource);
		desc_map[desc_map_count].handle_ptr = handle.ptr;
		desc_map[desc_map_count].res_idx = idx;
		desc_map_count++;
	}
}

static void __attribute__((ms_abi))
dev_CreateDepthStencilView(void *This, void *pResource,
			   void *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	(void)This; (void)pResource; (void)pDesc; (void)handle;
}

static void __attribute__((ms_abi))
dev_CreateConstantBufferView(void *This, void *pDesc,
			     D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	(void)This; (void)pDesc; (void)handle;
}

static void __attribute__((ms_abi))
dev_CreateShaderResourceView(void *This, void *pResource,
			     void *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	(void)This; (void)pResource; (void)pDesc; (void)handle;
}

static HRESULT __attribute__((ms_abi))
dev_CreateCommittedResource(void *This,
			    const D3D12_HEAP_PROPERTIES *pHeapProps,
			    D3D12_HEAP_FLAGS flags,
			    const D3D12_RESOURCE_DESC *pDesc,
			    D3D12_RESOURCE_STATES initialState,
			    const D3D12_CLEAR_VALUE *pOptClearValue,
			    const GUID *riid, void **ppResource)
{
	(void)This; (void)flags; (void)initialState;
	(void)pOptClearValue; (void)riid;
	if (!ppResource) return E_POINTER;

	int idx = alloc_resource();
	if (idx < 0) return E_OUTOFMEMORY;

	res_table[idx].active = 1;
	if (pDesc)
		memcpy(&res_table[idx].desc, pDesc, sizeof(*pDesc));
	if (pHeapProps)
		res_table[idx].heap_type = pHeapProps->Type;

	/* 메모리 할당 */
	if (pDesc) {
		if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
			res_table[idx].data_size = (size_t)pDesc->Width;
			res_table[idx].data = calloc(1, (size_t)pDesc->Width);
		} else if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
			int w = (int)pDesc->Width;
			int h = (int)pDesc->Height;
			res_table[idx].width = w;
			res_table[idx].height = h;
			res_table[idx].data_size = (size_t)(w * h * 4);
			res_table[idx].pixels = calloc((size_t)(w * h),
						       sizeof(uint32_t));
			res_table[idx].data = res_table[idx].pixels;
		}
	}

	/* COM 포인터 풀에 등록 */
	res_com[idx] = &res_vtbl;
	*ppResource = (void *)&res_com[idx];
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
dev_CreateFence(void *This, uint64_t initialValue,
		D3D12_FENCE_FLAGS flags, const GUID *riid, void **ppFence)
{
	(void)This; (void)flags; (void)riid;
	if (!ppFence) return E_POINTER;

	int idx = alloc_fence();
	if (idx < 0) return E_OUTOFMEMORY;

	fence_table[idx].active = 1;
	fence_table[idx].value = initialValue;

	/* COM 포인터 풀에 등록 */
	fence_com[idx] = &fence_vtbl;
	*ppFence = (void *)&fence_com[idx];
	return S_OK;
}

static ID3D12DeviceVtbl device_vtbl = {
	.QueryInterface = common_QueryInterface,
	.AddRef = common_AddRef,
	.Release = common_Release,
	.GetPrivateData = stub_GetPrivateData,
	.SetPrivateData = stub_SetPrivateData,
	.SetPrivateDataInterface = stub_SetPrivateDataInterface,
	.SetName = stub_SetName,
	.GetNodeCount = dev_GetNodeCount,
	.CreateCommandQueue = dev_CreateCommandQueue,
	.CreateCommandAllocator = dev_CreateCommandAllocator,
	.CreateGraphicsPipelineState = dev_CreateGraphicsPipelineState,
	.CreateCommandList = dev_CreateCommandList,
	.CreateDescriptorHeap = dev_CreateDescriptorHeap,
	.GetDescriptorHandleIncrementSize = dev_GetDescriptorHandleIncrementSize,
	.CreateRootSignature = dev_CreateRootSignature,
	.CreateConstantBufferView = dev_CreateConstantBufferView,
	.CreateShaderResourceView = dev_CreateShaderResourceView,
	.CreateRenderTargetView = dev_CreateRenderTargetView,
	.CreateDepthStencilView = dev_CreateDepthStencilView,
	.CreateCommittedResource = dev_CreateCommittedResource,
	.CreateFence = dev_CreateFence,
};

static ID3D12DeviceVtbl *device_vtbl_ptr = &device_vtbl;

/* ============================================================
 * vtable 초기화
 * ============================================================ */

static void init_vtables(void)
{
	/* ID3D12Fence vtable */
	memset(&fence_vtbl, 0, sizeof(fence_vtbl));
	fence_vtbl.QueryInterface = common_QueryInterface;
	fence_vtbl.AddRef = common_AddRef;
	fence_vtbl.Release = common_Release;
	fence_vtbl.GetPrivateData = (void *)stub_GetPrivateData;
	fence_vtbl.SetPrivateData = (void *)stub_SetPrivateData;
	fence_vtbl.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface;
	fence_vtbl.SetName = (void *)stub_SetName;
	fence_vtbl.GetDevice = NULL;
	fence_vtbl.GetCompletedValue = fence_GetCompletedValue;
	fence_vtbl.SetEventOnCompletion = fence_SetEventOnCompletion;
	fence_vtbl.Signal = fence_Signal_method;

	/* ID3D12Resource vtable */
	memset(&res_vtbl, 0, sizeof(res_vtbl));
	res_vtbl.QueryInterface = common_QueryInterface;
	res_vtbl.AddRef = common_AddRef;
	res_vtbl.Release = common_Release;
	res_vtbl.GetPrivateData = (void *)stub_GetPrivateData;
	res_vtbl.SetPrivateData = (void *)stub_SetPrivateData;
	res_vtbl.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface;
	res_vtbl.SetName = (void *)stub_SetName;
	res_vtbl.GetDevice = NULL;
	res_vtbl.Map = res_Map;
	res_vtbl.Unmap = res_Unmap;
	res_vtbl.GetDesc = NULL;
	res_vtbl.GetGPUVirtualAddress = res_GetGPUVirtualAddress;

	/* ID3D12DescriptorHeap vtable */
	memset(&descheap_vtbl, 0, sizeof(descheap_vtbl));
	descheap_vtbl.QueryInterface = common_QueryInterface;
	descheap_vtbl.AddRef = common_AddRef;
	descheap_vtbl.Release = common_Release;
	descheap_vtbl.GetPrivateData = (void *)stub_GetPrivateData;
	descheap_vtbl.SetPrivateData = (void *)stub_SetPrivateData;
	descheap_vtbl.SetPrivateDataInterface = (void *)stub_SetPrivateDataInterface;
	descheap_vtbl.SetName = (void *)stub_SetName;
	descheap_vtbl.GetDevice = NULL;
	descheap_vtbl.GetDesc = NULL;
	descheap_vtbl.GetCPUDescriptorHandleForHeapStart =
		dh_GetCPUDescriptorHandleForHeapStart;
	descheap_vtbl.GetGPUDescriptorHandleForHeapStart =
		dh_GetGPUDescriptorHandleForHeapStart;
}

/* ============================================================
 * D3D12CreateDevice — 엔트리 포인트
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
d3d12_CreateDevice(void *pAdapter, UINT MinFeatureLevel,
		   const GUID *riid, void **ppDevice)
{
	(void)pAdapter; (void)MinFeatureLevel; (void)riid;
	if (!ppDevice) return E_POINTER;

	if (!device_active) {
		init_vtables();
		memset(res_table, 0, sizeof(res_table));
		memset(heap_table, 0, sizeof(heap_table));
		memset(fence_table, 0, sizeof(fence_table));
		memset(rootsig_table, 0, sizeof(rootsig_table));
		memset(pso_table, 0, sizeof(pso_table));
		memset(&cmdlist, 0, sizeof(cmdlist));
		memset(res_com, 0, sizeof(res_com));
		memset(fence_com, 0, sizeof(fence_com));
		memset(dh_com, 0, sizeof(dh_com));
		memset(desc_map, 0, sizeof(desc_map));
		desc_map_count = 0;
		device_active = 1;
	}

	*ppDevice = (void *)&device_vtbl_ptr;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
d3d12_GetDebugInterface(const GUID *riid, void **ppDebug)
{
	(void)riid;
	if (ppDebug) *ppDebug = NULL;
	return E_NOINTERFACE;
}

static HRESULT __attribute__((ms_abi))
d3d12_SerializeRootSignature(const void *pDesc, UINT version,
			     void **ppBlob, void **ppError)
{
	(void)pDesc; (void)version; (void)ppError;
	/* 최소 구현: 더미 blob 반환 */
	if (ppBlob) {
		uint8_t *blob = calloc(1, 32);
		if (blob)
			*ppBlob = blob;
	}
	return S_OK;
}

/* ============================================================
 * Stub Table
 * ============================================================ */

struct stub_entry d3d12_stub_table[] = {
	{ "d3d12.dll", "D3D12CreateDevice",
	  (void *)d3d12_CreateDevice },
	{ "d3d12.dll", "D3D12GetDebugInterface",
	  (void *)d3d12_GetDebugInterface },
	{ "d3d12.dll", "D3D12SerializeRootSignature",
	  (void *)d3d12_SerializeRootSignature },
	{ NULL, NULL, NULL }
};
