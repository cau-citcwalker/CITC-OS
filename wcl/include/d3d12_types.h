/*
 * d3d12_types.h — DirectX 12 타입 정의
 * ======================================
 *
 * D3D12에서 사용하는 열거형, 구조체, COM vtable 선언.
 * D3D11과 달리 D3D12는 명시적 커맨드 모델:
 *   CommandAllocator → CommandList → Close → Execute on CommandQueue
 *   Fence로 GPU 동기화 관리.
 *
 * 핸들 오프셋 할당:
 *   0x60000 = ID3D12Device
 *   0x61000 = ID3D12CommandQueue
 *   0x62000 = ID3D12CommandAllocator
 *   0x63000 = ID3D12GraphicsCommandList
 *   0x64000 = ID3D12Resource (Buffer, Texture)
 *   0x65000 = ID3D12DescriptorHeap
 *   0x66000 = ID3D12Fence
 *   0x67000 = ID3D12RootSignature
 *   0x68000 = ID3D12PipelineState
 */

#ifndef CITC_D3D12_TYPES_H
#define CITC_D3D12_TYPES_H

#include "win32.h"
#include "d3d11_types.h" /* DXGI_FORMAT, DXGI_SAMPLE_DESC 등 재사용 */

/* ============================================================
 * D3D12 열거형
 * ============================================================ */

typedef enum {
	D3D12_COMMAND_LIST_TYPE_DIRECT  = 0,
	D3D12_COMMAND_LIST_TYPE_BUNDLE  = 1,
	D3D12_COMMAND_LIST_TYPE_COMPUTE = 2,
	D3D12_COMMAND_LIST_TYPE_COPY    = 3,
} D3D12_COMMAND_LIST_TYPE;

typedef enum {
	D3D12_COMMAND_QUEUE_PRIORITY_NORMAL = 0,
	D3D12_COMMAND_QUEUE_PRIORITY_HIGH   = 100,
} D3D12_COMMAND_QUEUE_PRIORITY;

typedef enum {
	D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 0,
	D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER     = 1,
	D3D12_DESCRIPTOR_HEAP_TYPE_RTV         = 2,
	D3D12_DESCRIPTOR_HEAP_TYPE_DSV         = 3,
} D3D12_DESCRIPTOR_HEAP_TYPE;

typedef enum {
	D3D12_DESCRIPTOR_HEAP_FLAG_NONE           = 0,
	D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE = 1,
} D3D12_DESCRIPTOR_HEAP_FLAGS;

typedef enum {
	D3D12_HEAP_TYPE_DEFAULT  = 1,
	D3D12_HEAP_TYPE_UPLOAD   = 2,
	D3D12_HEAP_TYPE_READBACK = 3,
} D3D12_HEAP_TYPE;

typedef enum {
	D3D12_HEAP_FLAG_NONE = 0,
} D3D12_HEAP_FLAGS;

typedef enum {
	D3D12_RESOURCE_STATE_COMMON                 = 0,
	D3D12_RESOURCE_STATE_VERTEX_AND_CB          = 0x2,
	D3D12_RESOURCE_STATE_RENDER_TARGET          = 0x4,
	D3D12_RESOURCE_STATE_DEPTH_WRITE            = 0x10,
	D3D12_RESOURCE_STATE_COPY_DEST              = 0x400,
	D3D12_RESOURCE_STATE_COPY_SOURCE            = 0x800,
	D3D12_RESOURCE_STATE_GENERIC_READ           = 0x1,
	D3D12_RESOURCE_STATE_PRESENT                = 0,
} D3D12_RESOURCE_STATES;

typedef enum {
	D3D12_RESOURCE_DIMENSION_UNKNOWN  = 0,
	D3D12_RESOURCE_DIMENSION_BUFFER   = 1,
	D3D12_RESOURCE_DIMENSION_TEXTURE1D = 2,
	D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
	D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4,
} D3D12_RESOURCE_DIMENSION;

typedef enum {
	D3D12_TEXTURE_LAYOUT_UNKNOWN       = 0,
	D3D12_TEXTURE_LAYOUT_ROW_MAJOR     = 1,
} D3D12_TEXTURE_LAYOUT;

typedef enum {
	D3D12_RESOURCE_FLAG_NONE                  = 0,
	D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET   = 0x1,
	D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL   = 0x2,
} D3D12_RESOURCE_FLAGS;

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
	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA   = 0,
	D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA = 1,
} D3D12_INPUT_CLASSIFICATION;

typedef enum {
	D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED  = 0,
	D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT      = 1,
	D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE       = 2,
	D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE   = 3,
} D3D12_PRIMITIVE_TOPOLOGY_TYPE;

typedef enum {
	D3D12_FILL_MODE_WIREFRAME = 2,
	D3D12_FILL_MODE_SOLID     = 3,
} D3D12_FILL_MODE;

typedef enum {
	D3D12_CULL_MODE_NONE  = 1,
	D3D12_CULL_MODE_FRONT = 2,
	D3D12_CULL_MODE_BACK  = 3,
} D3D12_CULL_MODE;

typedef enum {
	D3D12_BLEND_ZERO          = 1,
	D3D12_BLEND_ONE           = 2,
	D3D12_BLEND_SRC_ALPHA     = 5,
	D3D12_BLEND_INV_SRC_ALPHA = 6,
} D3D12_BLEND;

typedef enum {
	D3D12_BLEND_OP_ADD = 1,
} D3D12_BLEND_OP;

/* ============================================================
 * D3D12 구조체
 * ============================================================ */

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

typedef struct {
	/* CPU handle */
	size_t ptr;
} D3D12_CPU_DESCRIPTOR_HANDLE;

typedef struct {
	/* GPU handle */
	uint64_t ptr;
} D3D12_GPU_DESCRIPTOR_HANDLE;

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
	float left, top, right, bottom;
	float minDepth, maxDepth;
} D3D12_VIEWPORT;

typedef struct {
	long left, top, right, bottom;
} D3D12_RECT;

typedef struct {
	void *pResource;	/* ID3D12Resource* */
	UINT Subresource;
	D3D12_RESOURCE_STATES StateBefore;
	D3D12_RESOURCE_STATES StateAfter;
} D3D12_RESOURCE_TRANSITION_BARRIER;

typedef struct {
	D3D12_RESOURCE_BARRIER_TYPE Type;
	D3D12_RESOURCE_BARRIER_FLAGS Flags;
	D3D12_RESOURCE_TRANSITION_BARRIER Transition;
} D3D12_RESOURCE_BARRIER;

typedef struct {
	uint64_t BufferLocation;  /* GPU virtual address */
	UINT SizeInBytes;
	UINT StrideInBytes;
} D3D12_VERTEX_BUFFER_VIEW;

typedef struct {
	uint64_t BufferLocation;
	UINT SizeInBytes;
	DXGI_FORMAT Format;
} D3D12_INDEX_BUFFER_VIEW;

/* Clear value */
typedef struct {
	DXGI_FORMAT Format;
	union {
		float Color[4];
		struct { float Depth; unsigned char Stencil; } DepthStencil;
	};
} D3D12_CLEAR_VALUE;

/* Input element */
typedef struct {
	const char *SemanticName;
	UINT SemanticIndex;
	DXGI_FORMAT Format;
	UINT InputSlot;
	UINT AlignedByteOffset;
	D3D12_INPUT_CLASSIFICATION InputSlotClass;
	UINT InstanceDataStepRate;
} D3D12_INPUT_ELEMENT_DESC;

typedef struct {
	const D3D12_INPUT_ELEMENT_DESC *pInputElementDescs;
	UINT NumElements;
} D3D12_INPUT_LAYOUT_DESC;

/* Shader bytecode */
typedef struct {
	const void *pShaderBytecode;
	size_t BytecodeLength;
} D3D12_SHADER_BYTECODE;

/* Rasterizer desc */
typedef struct {
	D3D12_FILL_MODE FillMode;
	D3D12_CULL_MODE CullMode;
	int FrontCounterClockwise;
	int DepthBias;
	float DepthBiasClamp;
	float SlopeScaledDepthBias;
	int DepthClipEnable;
	int MultisampleEnable;
	int AntialiasedLineEnable;
	UINT ForcedSampleCount;
	int ConservativeRaster;
} D3D12_RASTERIZER_DESC;

/* Blend */
typedef struct {
	int BlendEnable;
	int LogicOpEnable;
	D3D12_BLEND SrcBlend;
	D3D12_BLEND DestBlend;
	D3D12_BLEND_OP BlendOp;
	D3D12_BLEND SrcBlendAlpha;
	D3D12_BLEND DestBlendAlpha;
	D3D12_BLEND_OP BlendOpAlpha;
	int LogicOp;
	unsigned char RenderTargetWriteMask;
} D3D12_RENDER_TARGET_BLEND_DESC;

typedef struct {
	int AlphaToCoverageEnable;
	int IndependentBlendEnable;
	D3D12_RENDER_TARGET_BLEND_DESC RenderTarget[8];
} D3D12_BLEND_DESC;

/* Depth stencil */
typedef struct {
	int DepthEnable;
	int DepthWriteMask;
	int DepthFunc;
	int StencilEnable;
	unsigned char StencilReadMask;
	unsigned char StencilWriteMask;
	/* stencil ops omitted for brevity */
} D3D12_DEPTH_STENCIL_DESC;

/* Root signature (simplified) */
typedef struct {
	const void *pBlobWithRootSignature;
	size_t BlobLengthInBytes;
} D3D12_ROOT_SIGNATURE_DESC;

/* Graphics PSO */
typedef struct {
	void *pRootSignature;
	D3D12_SHADER_BYTECODE VS;
	D3D12_SHADER_BYTECODE PS;
	D3D12_BLEND_DESC BlendState;
	UINT SampleMask;
	D3D12_RASTERIZER_DESC RasterizerState;
	D3D12_DEPTH_STENCIL_DESC DepthStencilState;
	D3D12_INPUT_LAYOUT_DESC InputLayout;
	int IBStripCutValue;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType;
	UINT NumRenderTargets;
	DXGI_FORMAT RTVFormats[8];
	DXGI_FORMAT DSVFormat;
	DXGI_SAMPLE_DESC SampleDesc;
	UINT NodeMask;
	/* CachedPSO, Flags omitted */
} D3D12_GRAPHICS_PIPELINE_STATE_DESC;

/* ============================================================
 * D3D12 COM vtable
 * ============================================================ */

/* --- ID3D12Device --- */
typedef struct ID3D12DeviceVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);

	/* ID3D12Object */
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *, const GUID *, UINT *, void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *, const GUID *, UINT, const void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateDataInterface)(void *, const GUID *, void *);
	HRESULT (__attribute__((ms_abi)) *SetName)(void *, const void *);

	/* ID3D12Device */
	UINT    (__attribute__((ms_abi)) *GetNodeCount)(void *);
	HRESULT (__attribute__((ms_abi)) *CreateCommandQueue)(void *, const D3D12_COMMAND_QUEUE_DESC *, const GUID *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateCommandAllocator)(void *, D3D12_COMMAND_LIST_TYPE, const GUID *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateGraphicsPipelineState)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, const GUID *, void **);
	void *  ComputePipelineState; /* stub */
	HRESULT (__attribute__((ms_abi)) *CreateCommandList)(void *, UINT, D3D12_COMMAND_LIST_TYPE, void *, void *, const GUID *, void **);
	void *  CheckFeatureSupport; /* stub */
	HRESULT (__attribute__((ms_abi)) *CreateDescriptorHeap)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *, const GUID *, void **);
	UINT    (__attribute__((ms_abi)) *GetDescriptorHandleIncrementSize)(void *, D3D12_DESCRIPTOR_HEAP_TYPE);
	HRESULT (__attribute__((ms_abi)) *CreateRootSignature)(void *, UINT, const void *, size_t, const GUID *, void **);
	void    (__attribute__((ms_abi)) *CreateConstantBufferView)(void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);
	void    (__attribute__((ms_abi)) *CreateShaderResourceView)(void *, void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);
	void *  CreateUnorderedAccessView; /* stub */
	void    (__attribute__((ms_abi)) *CreateRenderTargetView)(void *, void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);
	void    (__attribute__((ms_abi)) *CreateDepthStencilView)(void *, void *, void *, D3D12_CPU_DESCRIPTOR_HANDLE);
	void *  CreateSampler; /* stub */
	void *  CopyDescriptors; /* stub */
	void *  CopyDescriptorsSimple; /* stub */
	void *  GetResourceAllocationInfo; /* stub */
	void *  GetCustomHeapProperties; /* stub */
	HRESULT (__attribute__((ms_abi)) *CreateCommittedResource)(void *,
		const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
		const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
		const D3D12_CLEAR_VALUE *, const GUID *, void **);
	void *  CreateHeap; /* stub */
	void *  CreatePlacedResource; /* stub */
	void *  CreateReservedResource; /* stub */
	void *  CreateSharedHandle; /* stub */
	void *  OpenSharedHandle; /* stub */
	void *  OpenSharedHandleByName; /* stub */
	void *  MakeResident; /* stub */
	void *  Evict; /* stub */
	HRESULT (__attribute__((ms_abi)) *CreateFence)(void *, uint64_t, D3D12_FENCE_FLAGS, const GUID *, void **);
	void *  GetDeviceRemovedReason; /* stub */
	void *  GetCopyableFootprints; /* stub */
	void *  CreateQueryHeap; /* stub */
	void *  SetStablePowerState; /* stub */
	void *  CreateCommandSignature; /* stub */
} ID3D12DeviceVtbl;

/* --- ID3D12CommandQueue --- */
typedef struct ID3D12CommandQueueVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	/* ID3D12Object */
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	/* ID3D12CommandQueue */
	void    (__attribute__((ms_abi)) *UpdateTileMappings)(void *);
	void    (__attribute__((ms_abi)) *CopyTileMappings)(void *);
	void    (__attribute__((ms_abi)) *ExecuteCommandLists)(void *, UINT, void *const *);
	void    (__attribute__((ms_abi)) *SetMarker)(void *, UINT, const void *, UINT);
	void    (__attribute__((ms_abi)) *BeginEvent)(void *, UINT, const void *, UINT);
	void    (__attribute__((ms_abi)) *EndEvent)(void *);
	HRESULT (__attribute__((ms_abi)) *Signal)(void *, void *, uint64_t);
	HRESULT (__attribute__((ms_abi)) *Wait)(void *, void *, uint64_t);
	void *  GetTimestampFrequency;
	void *  GetClockCalibration;
	void *  GetDesc;
} ID3D12CommandQueueVtbl;

/* --- ID3D12CommandAllocator --- */
typedef struct ID3D12CommandAllocatorVtbl {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	HRESULT (__attribute__((ms_abi)) *Reset)(void *);
} ID3D12CommandAllocatorVtbl;

/* --- ID3D12GraphicsCommandList --- */
typedef struct ID3D12GraphicsCommandListVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	/* ID3D12Object */
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	/* ID3D12DeviceChild */
	void *  GetDevice;
	/* ID3D12CommandList */
	int     (__attribute__((ms_abi)) *GetType)(void *);
	/* ID3D12GraphicsCommandList */
	HRESULT (__attribute__((ms_abi)) *Close)(void *);
	HRESULT (__attribute__((ms_abi)) *Reset)(void *, void *, void *);
	void    (__attribute__((ms_abi)) *ClearState)(void *, void *);
	void    (__attribute__((ms_abi)) *DrawInstanced)(void *, UINT, UINT, UINT, UINT);
	void    (__attribute__((ms_abi)) *DrawIndexedInstanced)(void *, UINT, UINT, UINT, int, UINT);
	void *  Dispatch;
	void *  CopyBufferRegion;
	void *  CopyTextureRegion;
	void *  CopyResource;
	void *  CopyTiles;
	void *  ResolveSubresource;
	void    (__attribute__((ms_abi)) *IASetPrimitiveTopology)(void *, int);
	void    (__attribute__((ms_abi)) *RSSetViewports)(void *, UINT, const D3D12_VIEWPORT *);
	void    (__attribute__((ms_abi)) *RSSetScissorRects)(void *, UINT, const D3D12_RECT *);
	void *  OMSetBlendFactor;
	void *  OMSetStencilRef;
	void    (__attribute__((ms_abi)) *SetPipelineState)(void *, void *);
	void    (__attribute__((ms_abi)) *ResourceBarrier)(void *, UINT, const D3D12_RESOURCE_BARRIER *);
	void *  ExecuteBundle;
	void *  SetDescriptorHeaps;
	void *  SetComputeRootSignature;
	void    (__attribute__((ms_abi)) *SetGraphicsRootSignature)(void *, void *);
	void *  SetComputeRootDescriptorTable;
	void *  SetGraphicsRootDescriptorTable;
	void *  SetComputeRoot32BitConstant;
	void *  SetGraphicsRoot32BitConstant;
	void *  SetComputeRoot32BitConstants;
	void *  SetGraphicsRoot32BitConstants;
	void *  SetComputeRootConstantBufferView;
	void *  SetGraphicsRootConstantBufferView;
	void *  SetComputeRootShaderResourceView;
	void *  SetGraphicsRootShaderResourceView;
	void *  SetComputeRootUnorderedAccessView;
	void *  SetGraphicsRootUnorderedAccessView;
	void    (__attribute__((ms_abi)) *IASetIndexBuffer)(void *, const D3D12_INDEX_BUFFER_VIEW *);
	void    (__attribute__((ms_abi)) *IASetVertexBuffers)(void *, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW *);
	void *  SOSetTargets;
	void    (__attribute__((ms_abi)) *OMSetRenderTargets)(void *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, int, const D3D12_CPU_DESCRIPTOR_HANDLE *);
	void    (__attribute__((ms_abi)) *ClearDepthStencilView)(void *, D3D12_CPU_DESCRIPTOR_HANDLE, UINT, float, unsigned char, UINT, const D3D12_RECT *);
	void    (__attribute__((ms_abi)) *ClearRenderTargetView)(void *, D3D12_CPU_DESCRIPTOR_HANDLE, const float *, UINT, const D3D12_RECT *);
	void *  ClearUnorderedAccessViewUint;
	void *  ClearUnorderedAccessViewFloat;
	void *  DiscardResource;
	void *  BeginQuery;
	void *  EndQuery;
	void *  ResolveQueryData;
	void *  SetPredication;
	void *  SetMarker;
	void *  BeginEvent;
	void *  EndEvent;
	void *  ExecuteIndirect;
} ID3D12GraphicsCommandListVtbl;

/* --- ID3D12Resource --- */
typedef struct ID3D12ResourceVtbl {
	HRESULT  (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG    (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG    (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *   GetDevice;
	HRESULT  (__attribute__((ms_abi)) *Map)(void *, UINT, const void *, void **);
	void     (__attribute__((ms_abi)) *Unmap)(void *, UINT, const void *);
	void *   GetDesc;
	uint64_t (__attribute__((ms_abi)) *GetGPUVirtualAddress)(void *);
	void *   WriteToSubresource;
	void *   ReadFromSubresource;
	void *   GetHeapProperties;
} ID3D12ResourceVtbl;

/* --- ID3D12Fence --- */
typedef struct ID3D12FenceVtbl {
	HRESULT  (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG    (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG    (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *   GetDevice;
	uint64_t (__attribute__((ms_abi)) *GetCompletedValue)(void *);
	HRESULT  (__attribute__((ms_abi)) *SetEventOnCompletion)(void *, uint64_t, void *);
	HRESULT  (__attribute__((ms_abi)) *Signal)(void *, uint64_t);
} ID3D12FenceVtbl;

/* --- ID3D12DescriptorHeap --- */
typedef struct ID3D12DescriptorHeapVtbl {
	HRESULT  (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG    (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG    (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *   GetDevice;
	void *   GetDesc;
	D3D12_CPU_DESCRIPTOR_HANDLE (__attribute__((ms_abi)) *GetCPUDescriptorHandleForHeapStart)(void *);
	D3D12_GPU_DESCRIPTOR_HANDLE (__attribute__((ms_abi)) *GetGPUDescriptorHandleForHeapStart)(void *);
} ID3D12DescriptorHeapVtbl;

/* --- ID3D12RootSignature --- */
typedef struct ID3D12RootSignatureVtbl {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *  GetDevice;
} ID3D12RootSignatureVtbl;

/* --- ID3D12PipelineState --- */
typedef struct ID3D12PipelineStateVtbl {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, const GUID *, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void *GetPrivateData, *SetPrivateData, *SetPrivateDataInterface, *SetName;
	void *  GetDevice;
	void *  GetCachedBlob;
} ID3D12PipelineStateVtbl;

#endif /* CITC_D3D12_TYPES_H */
