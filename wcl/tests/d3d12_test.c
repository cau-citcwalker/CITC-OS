/*
 * d3d12_test.c — DirectX 12 API 테스트
 * =====================================
 *
 * D3D12 최소 구현 검증:
 *   [1] D3D12CreateDevice
 *   [2] CreateCommandQueue + CreateCommandAllocator + CreateCommandList
 *   [3] CreateDescriptorHeap(RTV) + GetCPUDescriptorHandleForHeapStart
 *   [4] CreateCommittedResource(TEXTURE2D) + CreateRenderTargetView
 *   [5] ClearRenderTargetView(red) + Close → 픽셀 확인
 *   [6] CommandList Reset + 재사용 (blue clear)
 *   [7] CreateCommittedResource(BUFFER) + Map/Unmap
 *   [8] CreateFence + Signal + GetCompletedValue
 *   [9] CommandQueue::Signal + Fence 확인
 *   [10] Release + 정리
 */

/* === 타입 정의 (MinGW 독립) === */

typedef void           *HANDLE;
typedef unsigned int    UINT;
typedef int             BOOL;
typedef unsigned long   DWORD;
typedef unsigned int    ULONG;
typedef int             HRESULT;
typedef unsigned long long uint64_t;
typedef unsigned long long uintptr_t;
typedef unsigned long long size_t;
typedef long long       ptrdiff_t;
typedef unsigned int    uint32_t;
typedef unsigned char   uint8_t;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)
#define S_OK          ((HRESULT)0)

/* GUID */
typedef struct { DWORD Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } GUID;
typedef const GUID *REFIID;

/* D3D12 enums */
typedef enum {
	D3D12_COMMAND_LIST_TYPE_DIRECT = 0,
} D3D12_COMMAND_LIST_TYPE;

typedef enum {
	D3D12_DESCRIPTOR_HEAP_TYPE_RTV = 2,
} D3D12_DESCRIPTOR_HEAP_TYPE;

typedef enum {
	D3D12_DESCRIPTOR_HEAP_FLAG_NONE = 0,
} D3D12_DESCRIPTOR_HEAP_FLAGS;

typedef enum {
	D3D12_HEAP_TYPE_DEFAULT = 1,
	D3D12_HEAP_TYPE_UPLOAD  = 2,
} D3D12_HEAP_TYPE;

typedef enum {
	D3D12_HEAP_FLAG_NONE = 0,
} D3D12_HEAP_FLAGS;

typedef enum {
	D3D12_RESOURCE_STATE_COMMON       = 0,
	D3D12_RESOURCE_STATE_RENDER_TARGET = 0x4,
	D3D12_RESOURCE_STATE_GENERIC_READ = 0x1,
} D3D12_RESOURCE_STATES;

typedef enum {
	D3D12_RESOURCE_DIMENSION_BUFFER    = 1,
	D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
} D3D12_RESOURCE_DIMENSION;

typedef enum {
	D3D12_RESOURCE_FLAG_NONE                = 0,
	D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET = 0x1,
} D3D12_RESOURCE_FLAGS;

typedef enum {
	D3D12_TEXTURE_LAYOUT_UNKNOWN = 0,
} D3D12_TEXTURE_LAYOUT;

typedef enum {
	D3D12_FENCE_FLAG_NONE = 0,
} D3D12_FENCE_FLAGS;

typedef enum {
	D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0,
} D3D12_RESOURCE_BARRIER_TYPE;

typedef enum {
	D3D12_RESOURCE_BARRIER_FLAG_NONE = 0,
} D3D12_RESOURCE_BARRIER_FLAGS;

typedef enum {
	DXGI_FORMAT_R8G8B8A8_UNORM = 28,
} DXGI_FORMAT;

/* D3D12 structs */
typedef struct { UINT Count; UINT Quality; } DXGI_SAMPLE_DESC;

typedef struct {
	D3D12_COMMAND_LIST_TYPE Type;
	int Priority;
	UINT Flags;
	UINT NodeMask;
} D3D12_COMMAND_QUEUE_DESC;

typedef struct {
	D3D12_DESCRIPTOR_HEAP_TYPE Type;
	UINT NumDescriptors;
	D3D12_DESCRIPTOR_HEAP_FLAGS Flags;
	UINT NodeMask;
} D3D12_DESCRIPTOR_HEAP_DESC;

typedef struct { size_t ptr; } D3D12_CPU_DESCRIPTOR_HANDLE;
typedef struct { uint64_t ptr; } D3D12_GPU_DESCRIPTOR_HANDLE;

typedef struct {
	D3D12_HEAP_TYPE Type;
	UINT CPUPageProperty;
	UINT MemoryPoolPreference;
	UINT CreationNodeMask;
	UINT VisibleNodeMask;
} D3D12_HEAP_PROPERTIES;

typedef struct {
	D3D12_RESOURCE_DIMENSION Dimension;
	uint64_t Alignment;
	uint64_t Width;
	UINT Height;
	unsigned short DepthOrArraySize;
	unsigned short MipLevels;
	DXGI_FORMAT Format;
	DXGI_SAMPLE_DESC SampleDesc;
	D3D12_TEXTURE_LAYOUT Layout;
	D3D12_RESOURCE_FLAGS Flags;
} D3D12_RESOURCE_DESC;

typedef struct {
	void *pResource;
	UINT Subresource;
	D3D12_RESOURCE_STATES StateBefore;
	D3D12_RESOURCE_STATES StateAfter;
} D3D12_RESOURCE_TRANSITION_BARRIER;

typedef struct {
	D3D12_RESOURCE_BARRIER_TYPE Type;
	D3D12_RESOURCE_BARRIER_FLAGS Flags;
	D3D12_RESOURCE_TRANSITION_BARRIER Transition;
} D3D12_RESOURCE_BARRIER;

typedef struct { float left, top, right, bottom, minDepth, maxDepth; } D3D12_VIEWPORT;
typedef struct { long left, top, right, bottom; } D3D12_RECT;

typedef struct {
	uint64_t BufferLocation;
	UINT SizeInBytes;
	UINT StrideInBytes;
} D3D12_VERTEX_BUFFER_VIEW;

/* === COM vtable 정의 (테스트에서 사용하는 메서드만) === */

typedef struct {
	/* IUnknown (0-2) */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	/* ID3D12Object (3-6) */
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	/* ID3D12Device (7+) */
	UINT    (__attribute__((ms_abi)) *GetNodeCount)(void *);                              /* 7 */
	HRESULT (__attribute__((ms_abi)) *CreateCommandQueue)(void *,
		const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);                           /* 8 */
	HRESULT (__attribute__((ms_abi)) *CreateCommandAllocator)(void *,
		D3D12_COMMAND_LIST_TYPE, REFIID, void **);                                    /* 9 */
	HRESULT (__attribute__((ms_abi)) *CreateGraphicsPipelineState)(void *,
		const void *, REFIID, void **);                                               /* 10 */
	void *ComputePipelineState;                                                           /* 11 */
	HRESULT (__attribute__((ms_abi)) *CreateCommandList)(void *, UINT,
		D3D12_COMMAND_LIST_TYPE, void *, void *, REFIID, void **);                    /* 12 */
	void *CheckFeatureSupport;                                                            /* 13 */
	HRESULT (__attribute__((ms_abi)) *CreateDescriptorHeap)(void *,
		const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);                         /* 14 */
	UINT    (__attribute__((ms_abi)) *GetDescriptorHandleIncrementSize)(void *,
		D3D12_DESCRIPTOR_HEAP_TYPE);                                                  /* 15 */
	HRESULT (__attribute__((ms_abi)) *CreateRootSignature)(void *, UINT,
		const void *, size_t, REFIID, void **);                                       /* 16 */
	void    (__attribute__((ms_abi)) *CreateConstantBufferView)(void *,
		void *, D3D12_CPU_DESCRIPTOR_HANDLE);                                         /* 17 */
	void    (__attribute__((ms_abi)) *CreateShaderResourceView)(void *,
		void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);                                 /* 18 */
	void *CreateUnorderedAccessView;                                                      /* 19 */
	void    (__attribute__((ms_abi)) *CreateRenderTargetView)(void *,
		void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);                                 /* 20 */
	void    (__attribute__((ms_abi)) *CreateDepthStencilView)(void *,
		void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);                                 /* 21 */
	void *CreateSampler;                                                                  /* 22 */
	void *CopyDescriptors, *CopyDescriptorsSimple;                                        /* 23-24 */
	void *GetResourceAllocationInfo, *GetCustomHeapProperties;                             /* 25-26 */
	HRESULT (__attribute__((ms_abi)) *CreateCommittedResource)(void *,
		const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
		const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
		const void *, REFIID, void **);                                               /* 27 */
	void *CreateHeap, *CreatePlacedResource, *CreateReservedResource;                      /* 28-30 */
	void *CreateSharedHandle, *OpenSharedHandle, *OpenSharedHandleByName;                  /* 31-33 */
	void *MakeResident, *Evict;                                                            /* 34-35 */
	HRESULT (__attribute__((ms_abi)) *CreateFence)(void *, uint64_t,
		D3D12_FENCE_FLAGS, REFIID, void **);                                          /* 36 */
} ID3D12DeviceVtbl;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *UpdateTileMappings, *CopyTileMappings;
	void    (__attribute__((ms_abi)) *ExecuteCommandLists)(void *, UINT, void *const *);
	void *SetMarker, *BeginEvent, *EndEvent;
	HRESULT (__attribute__((ms_abi)) *Signal)(void *, void *, uint64_t);
} ID3D12CommandQueueVtbl;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	HRESULT (__attribute__((ms_abi)) *Reset)(void *);
} ID3D12CommandAllocatorVtbl;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *GetDevice;
	int     (__attribute__((ms_abi)) *GetType)(void *);
	HRESULT (__attribute__((ms_abi)) *Close)(void *);
	HRESULT (__attribute__((ms_abi)) *Reset)(void *, void *, void *);
	void    (__attribute__((ms_abi)) *ClearState)(void *, void *);
	void    (__attribute__((ms_abi)) *DrawInstanced)(void *, UINT, UINT, UINT, UINT);
	void    (__attribute__((ms_abi)) *DrawIndexedInstanced)(void *, UINT, UINT, UINT, int, UINT);
	void *Dispatch, *CopyBufferRegion, *CopyTextureRegion, *CopyResource, *CopyTiles, *ResolveSubresource;
	void    (__attribute__((ms_abi)) *IASetPrimitiveTopology)(void *, int);
	void    (__attribute__((ms_abi)) *RSSetViewports)(void *, UINT, const D3D12_VIEWPORT *);
	void    (__attribute__((ms_abi)) *RSSetScissorRects)(void *, UINT, const D3D12_RECT *);
	void *OMSetBlendFactor, *OMSetStencilRef;
	void    (__attribute__((ms_abi)) *SetPipelineState)(void *, void *);
	void    (__attribute__((ms_abi)) *ResourceBarrier)(void *, UINT, const D3D12_RESOURCE_BARRIER *);
	void *ExecuteBundle, *SetDescriptorHeaps, *SetComputeRootSignature;
	void    (__attribute__((ms_abi)) *SetGraphicsRootSignature)(void *, void *);
	void *SetComputeRootDescriptorTable, *SetGraphicsRootDescriptorTable;
	void *SetComputeRoot32BitConstant, *SetGraphicsRoot32BitConstant;
	void *SetComputeRoot32BitConstants, *SetGraphicsRoot32BitConstants;
	void *SetComputeRootConstantBufferView, *SetGraphicsRootConstantBufferView;
	void *SetComputeRootShaderResourceView, *SetGraphicsRootShaderResourceView;
	void *SetComputeRootUnorderedAccessView, *SetGraphicsRootUnorderedAccessView;
	void *IASetIndexBuffer;
	void    (__attribute__((ms_abi)) *IASetVertexBuffers)(void *, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW *);
	void *SOSetTargets;
	void    (__attribute__((ms_abi)) *OMSetRenderTargets)(void *, UINT,
		const D3D12_CPU_DESCRIPTOR_HANDLE *, int, const D3D12_CPU_DESCRIPTOR_HANDLE *);
	void    (__attribute__((ms_abi)) *ClearDepthStencilView)(void *,
		D3D12_CPU_DESCRIPTOR_HANDLE, UINT, float, unsigned char, UINT, const D3D12_RECT *);
	void    (__attribute__((ms_abi)) *ClearRenderTargetView)(void *,
		D3D12_CPU_DESCRIPTOR_HANDLE, const float *, UINT, const D3D12_RECT *);
} ID3D12GraphicsCommandListVtbl;

typedef struct {
	HRESULT  (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG    (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG    (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *GetDevice;
	HRESULT  (__attribute__((ms_abi)) *Map)(void *, UINT, const void *, void **);
	void     (__attribute__((ms_abi)) *Unmap)(void *, UINT, const void *);
	void *GetDesc;
	uint64_t (__attribute__((ms_abi)) *GetGPUVirtualAddress)(void *);
} ID3D12ResourceVtbl;

typedef struct {
	HRESULT  (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG    (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG    (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *GetDevice;
	uint64_t (__attribute__((ms_abi)) *GetCompletedValue)(void *);
	HRESULT  (__attribute__((ms_abi)) *SetEventOnCompletion)(void *, uint64_t, void *);
	HRESULT  (__attribute__((ms_abi)) *Signal)(void *, uint64_t);
} ID3D12FenceVtbl;

typedef struct {
	HRESULT  (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG    (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG    (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *GetDevice, *GetDesc;
	D3D12_CPU_DESCRIPTOR_HANDLE (__attribute__((ms_abi)) *GetCPUDescriptorHandleForHeapStart)(void *);
	D3D12_GPU_DESCRIPTOR_HANDLE (__attribute__((ms_abi)) *GetGPUDescriptorHandleForHeapStart)(void *);
} ID3D12DescriptorHeapVtbl;

/* === 임포트 === */

__declspec(dllimport) void __stdcall ExitProcess(UINT code);
__declspec(dllimport) int  __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);

__declspec(dllimport) HRESULT __stdcall D3D12CreateDevice(
	void *pAdapter, UINT MinFeatureLevel, REFIID riid, void **ppDevice);

#define STD_OUTPUT_HANDLE ((DWORD)-11)

/* === 유틸리티 === */

static HANDLE hStdout;

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void print(const char *s)
{
	DWORD written;
	WriteFile(hStdout, s, (DWORD)my_strlen(s), &written, NULL);
}

static void print_hex(uint32_t v)
{
	char buf[12] = "0x";
	const char *hex = "0123456789ABCDEF";
	for (int i = 7; i >= 0; i--)
		buf[2 + (7 - i)] = hex[(v >> (i * 4)) & 0xF];
	buf[10] = 0;
	print(buf);
}

static void print_num(unsigned long long v)
{
	char buf[24];
	int i = 0;
	if (v == 0) { buf[i++] = '0'; }
	else {
		char tmp[24]; int j = 0;
		while (v > 0) { tmp[j++] = '0' + (char)(v % 10); v /= 10; }
		while (j > 0) buf[i++] = tmp[--j];
	}
	buf[i] = 0;
	print(buf);
}

/* COM helper: This → vtable pointer */
#define VT(obj, type) (*(type**)(obj))

static int pass_count, fail_count;

static void test_ok(int n, const char *desc)
{
	print("  ["); print_num((unsigned long long)n); print("] ");
	print(desc); print(" ... PASS\n");
	pass_count++;
}

static void test_fail(int n, const char *desc)
{
	print("  ["); print_num((unsigned long long)n); print("] ");
	print(desc); print(" ... FAIL\n");
	fail_count++;
}

/* === 메인 === */

void __attribute__((ms_abi)) _start(void)
{
	hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	print("\n=== D3D12 Test ===\n\n");
	pass_count = 0;
	fail_count = 0;

	GUID iid_device = {0};
	GUID iid_zero = {0};

	/* ---- [1] D3D12CreateDevice ---- */
	void **device = NULL;
	HRESULT hr = D3D12CreateDevice(NULL, 0, &iid_device, (void **)&device);
	if (SUCCEEDED(hr) && device != NULL)
		test_ok(1, "D3D12CreateDevice");
	else
		test_fail(1, "D3D12CreateDevice");

	ID3D12DeviceVtbl *dv = VT(device, ID3D12DeviceVtbl);

	/* ---- [2] CreateCommandQueue + CreateCommandAllocator + CreateCommandList ---- */
	D3D12_COMMAND_QUEUE_DESC qd;
	for (int i = 0; i < (int)sizeof(qd); i++) ((char *)&qd)[i] = 0;
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	void **cmdQueue = NULL;
	void **cmdAlloc = NULL;
	void **cmdList  = NULL;

	hr = dv->CreateCommandQueue(device, &qd, &iid_zero, (void **)&cmdQueue);
	HRESULT hr2 = dv->CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
						  &iid_zero, (void **)&cmdAlloc);
	HRESULT hr3 = dv->CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					     cmdAlloc, NULL, &iid_zero, (void **)&cmdList);

	if (SUCCEEDED(hr) && SUCCEEDED(hr2) && SUCCEEDED(hr3) &&
	    cmdQueue && cmdAlloc && cmdList)
		test_ok(2, "CreateCommandQueue + Allocator + CommandList");
	else
		test_fail(2, "CreateCommandQueue + Allocator + CommandList");

	ID3D12CommandQueueVtbl *cqv = VT(cmdQueue, ID3D12CommandQueueVtbl);
	ID3D12GraphicsCommandListVtbl *clv = VT(cmdList, ID3D12GraphicsCommandListVtbl);

	/* ---- [3] CreateDescriptorHeap(RTV) + GetCPUDescriptorHandleForHeapStart ---- */
	D3D12_DESCRIPTOR_HEAP_DESC dhd;
	for (int i = 0; i < (int)sizeof(dhd); i++) ((char *)&dhd)[i] = 0;
	dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	dhd.NumDescriptors = 1;
	dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	void **rtvHeap = NULL;
	hr = dv->CreateDescriptorHeap(device, &dhd, &iid_zero, (void **)&rtvHeap);

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {0};
	if (SUCCEEDED(hr) && rtvHeap) {
		ID3D12DescriptorHeapVtbl *dhv = VT(rtvHeap, ID3D12DescriptorHeapVtbl);
		rtvHandle = dhv->GetCPUDescriptorHandleForHeapStart(rtvHeap);
	}

	if (SUCCEEDED(hr) && rtvHeap && rtvHandle.ptr != 0)
		test_ok(3, "CreateDescriptorHeap(RTV) + GetCPUHandle");
	else
		test_fail(3, "CreateDescriptorHeap(RTV) + GetCPUHandle");

	/* ---- [4] CreateCommittedResource(TEXTURE2D) + CreateRenderTargetView ---- */
	D3D12_HEAP_PROPERTIES hp;
	for (int i = 0; i < (int)sizeof(hp); i++) ((char *)&hp)[i] = 0;
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC rd;
	for (int i = 0; i < (int)sizeof(rd); i++) ((char *)&rd)[i] = 0;
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = 64;
	rd.Height = 64;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rd.SampleDesc.Count = 1;
	rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	void **rtResource = NULL;
	hr = dv->CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE,
					 &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
					 NULL, &iid_zero, (void **)&rtResource);

	if (SUCCEEDED(hr) && rtResource) {
		dv->CreateRenderTargetView(device, rtResource, NULL, rtvHandle);
		test_ok(4, "CreateCommittedResource(TEX2D) + CreateRTV");
	} else {
		test_fail(4, "CreateCommittedResource(TEX2D) + CreateRTV");
	}

	/* ---- [5] ClearRenderTargetView(red) + Close → 픽셀 확인 ---- */
	{
		float clearColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f }; /* red */

		clv->ClearRenderTargetView(cmdList, rtvHandle, clearColor, 0, NULL);
		hr = clv->Close(cmdList);

		/* 실행 (no-op, Close에서 실행됨) */
		void *lists[1] = { cmdList };
		cqv->ExecuteCommandLists(cmdQueue, 1, lists);

		/* Map으로 픽셀 읽기 */
		ID3D12ResourceVtbl *rv = VT(rtResource, ID3D12ResourceVtbl);
		void *pData = NULL;
		hr2 = rv->Map(rtResource, 0, NULL, &pData);

		int pixel_ok = 0;
		if (SUCCEEDED(hr2) && pData) {
			uint32_t *pixels = (uint32_t *)pData;
			/* ARGB: A=FF R=FF G=00 B=00 → 0xFFFF0000 */
			uint32_t center = pixels[32 * 64 + 32]; /* 중앙 픽셀 */
			uint32_t corner = pixels[0];             /* 모서리 */
			if (center == 0xFFFF0000 && corner == 0xFFFF0000)
				pixel_ok = 1;
			rv->Unmap(rtResource, 0, NULL);
		}

		if (SUCCEEDED(hr) && pixel_ok)
			test_ok(5, "ClearRTV(red) + Close -> pixel check");
		else
			test_fail(5, "ClearRTV(red) + Close -> pixel check");
	}

	/* ---- [6] CommandList Reset + 재사용 (blue clear) ---- */
	{
		ID3D12CommandAllocatorVtbl *cav = VT(cmdAlloc, ID3D12CommandAllocatorVtbl);
		cav->Reset(cmdAlloc);
		hr = clv->Reset(cmdList, cmdAlloc, NULL);

		float blueColor[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		clv->ClearRenderTargetView(cmdList, rtvHandle, blueColor, 0, NULL);
		clv->Close(cmdList);

		void *lists[1] = { cmdList };
		cqv->ExecuteCommandLists(cmdQueue, 1, lists);

		ID3D12ResourceVtbl *rv = VT(rtResource, ID3D12ResourceVtbl);
		void *pData = NULL;
		rv->Map(rtResource, 0, NULL, &pData);

		int pixel_ok = 0;
		if (pData) {
			uint32_t *pixels = (uint32_t *)pData;
			/* ARGB: A=FF R=00 G=00 B=FF → 0xFF0000FF */
			if (pixels[32 * 64 + 32] == 0xFF0000FF)
				pixel_ok = 1;
			rv->Unmap(rtResource, 0, NULL);
		}

		if (SUCCEEDED(hr) && pixel_ok)
			test_ok(6, "CommandList Reset + blue clear");
		else
			test_fail(6, "CommandList Reset + blue clear");
	}

	/* ---- [7] CreateCommittedResource(BUFFER) + Map/Unmap ---- */
	{
		D3D12_HEAP_PROPERTIES bhp;
		for (int i = 0; i < (int)sizeof(bhp); i++) ((char *)&bhp)[i] = 0;
		bhp.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC brd;
		for (int i = 0; i < (int)sizeof(brd); i++) ((char *)&brd)[i] = 0;
		brd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		brd.Width = 256;
		brd.Height = 1;
		brd.DepthOrArraySize = 1;
		brd.MipLevels = 1;
		brd.SampleDesc.Count = 1;

		void **bufResource = NULL;
		hr = dv->CreateCommittedResource(device, &bhp, D3D12_HEAP_FLAG_NONE,
						 &brd, D3D12_RESOURCE_STATE_GENERIC_READ,
						 NULL, &iid_zero, (void **)&bufResource);

		int buf_ok = 0;
		if (SUCCEEDED(hr) && bufResource) {
			ID3D12ResourceVtbl *brv = VT(bufResource, ID3D12ResourceVtbl);
			void *pData = NULL;
			brv->Map(bufResource, 0, NULL, &pData);
			if (pData) {
				/* 데이터 쓰기 */
				uint32_t *p = (uint32_t *)pData;
				p[0] = 0xDEADBEEF;
				p[1] = 0xCAFEBABE;
				brv->Unmap(bufResource, 0, NULL);

				/* 다시 읽기 */
				void *pData2 = NULL;
				brv->Map(bufResource, 0, NULL, &pData2);
				if (pData2) {
					uint32_t *q = (uint32_t *)pData2;
					if (q[0] == 0xDEADBEEF && q[1] == 0xCAFEBABE)
						buf_ok = 1;
					brv->Unmap(bufResource, 0, NULL);
				}
			}
		}

		if (buf_ok)
			test_ok(7, "CreateCommittedResource(BUFFER) + Map/Unmap");
		else
			test_fail(7, "CreateCommittedResource(BUFFER) + Map/Unmap");
	}

	/* ---- [8] CreateFence + Signal + GetCompletedValue ---- */
	{
		void **fence = NULL;
		hr = dv->CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &iid_zero,
				     (void **)&fence);

		int fence_ok = 0;
		if (SUCCEEDED(hr) && fence) {
			ID3D12FenceVtbl *fv = VT(fence, ID3D12FenceVtbl);
			uint64_t val = fv->GetCompletedValue(fence);
			if (val == 0) {
				/* Signal to 42 via fence method */
				fv->Signal(fence, 42);
				val = fv->GetCompletedValue(fence);
				if (val == 42)
					fence_ok = 1;
			}
		}

		if (fence_ok)
			test_ok(8, "CreateFence + Signal + GetCompletedValue");
		else
			test_fail(8, "CreateFence + Signal + GetCompletedValue");
	}

	/* ---- [9] CommandQueue::Signal + Fence 확인 ---- */
	{
		void **fence2 = NULL;
		hr = dv->CreateFence(device, 10, D3D12_FENCE_FLAG_NONE, &iid_zero,
				     (void **)&fence2);

		int cq_ok = 0;
		if (SUCCEEDED(hr) && fence2) {
			ID3D12FenceVtbl *fv = VT(fence2, ID3D12FenceVtbl);
			uint64_t val = fv->GetCompletedValue(fence2);
			if (val == 10) {
				/* CommandQueue::Signal */
				cqv->Signal(cmdQueue, fence2, 100);
				val = fv->GetCompletedValue(fence2);
				if (val == 100)
					cq_ok = 1;
			}
		}

		if (cq_ok)
			test_ok(9, "CommandQueue::Signal -> Fence update");
		else
			test_fail(9, "CommandQueue::Signal -> Fence update");
	}

	/* ---- [10] Release ---- */
	{
		ID3D12DeviceVtbl *dvr = VT(device, ID3D12DeviceVtbl);
		dvr->Release(device);
		test_ok(10, "Release");
	}

	/* === 결과 === */
	print("\n--- d3d12_test: ");
	print_num((unsigned long long)pass_count);
	print("/");
	print_num((unsigned long long)(pass_count + fail_count));
	print(" PASS ---\n\n");

	ExitProcess(fail_count > 0 ? 1 : 0);
}
