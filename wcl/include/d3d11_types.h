/*
 * d3d11_types.h — DirectX 11 / DXGI 타입 정의
 * ==============================================
 *
 * D3D11과 DXGI에서 사용하는 열거형, 구조체, vtable 선언.
 * win32.h의 COM 기본 타입(HRESULT, GUID, IUnknownVtbl)을 기반으로 함.
 *
 * 실제 Windows SDK의 d3d11.h + dxgi.h에 해당하는 최소 버전.
 * Phase 4에서 필요한 타입만 정의.
 *
 * 핸들 오프셋 할당:
 *   0x50000 = ID3D11Device
 *   0x51000 = ID3D11DeviceContext
 *   0x52000 = Buffer, Texture2D (리소스)
 *   0x53000 = RTV, SRV, DSV (뷰)
 *   0x54000 = VertexShader, PixelShader
 *   0x55000 = DXGI 오브젝트 (Factory, Adapter, SwapChain)
 *   0x56000 = InputLayout
 */

#ifndef CITC_D3D11_TYPES_H
#define CITC_D3D11_TYPES_H

#include "win32.h"

/* ============================================================
 * DXGI 열거형
 * ============================================================
 *
 * DXGI_FORMAT: 픽셀/버텍스 데이터의 메모리 레이아웃.
 *   R8G8B8A8_UNORM = 채널당 8비트, 0.0~1.0 정규화 (가장 흔한 텍스처 포맷)
 *   R32G32B32_FLOAT = 채널당 32비트 float (버텍스 position용)
 *   D32_FLOAT = 32비트 깊이 버퍼
 *
 * 값은 Microsoft 공식 값과 동일해야 함 (MinGW 헤더 호환).
 */
typedef enum {
	DXGI_FORMAT_UNKNOWN                = 0,
	DXGI_FORMAT_R32G32B32A32_FLOAT     = 2,
	DXGI_FORMAT_R32G32B32_FLOAT        = 6,
	DXGI_FORMAT_R32G32_FLOAT           = 16,
	DXGI_FORMAT_R8G8B8A8_UNORM         = 28,
	DXGI_FORMAT_D32_FLOAT              = 40,
	DXGI_FORMAT_R32_UINT               = 42,
	DXGI_FORMAT_R16_UINT               = 57,
	DXGI_FORMAT_B8G8R8A8_UNORM         = 87,
} DXGI_FORMAT;

/* ============================================================
 * D3D11 열거형
 * ============================================================ */

/* 리소스 사용 패턴 */
typedef enum {
	D3D11_USAGE_DEFAULT    = 0,  /* GPU 읽기/쓰기 (가장 일반적) */
	D3D11_USAGE_IMMUTABLE  = 1,  /* GPU 읽기 전용 (초기 데이터 필수) */
	D3D11_USAGE_DYNAMIC    = 2,  /* CPU 쓰기 + GPU 읽기 (매 프레임 갱신) */
	D3D11_USAGE_STAGING    = 3,  /* CPU ↔ GPU 복사용 */
} D3D11_USAGE;

/* 리소스 바인딩 플래그 (비트 OR 조합) */
typedef enum {
	D3D11_BIND_VERTEX_BUFFER    = 0x001,
	D3D11_BIND_INDEX_BUFFER     = 0x002,
	D3D11_BIND_CONSTANT_BUFFER  = 0x004,
	D3D11_BIND_SHADER_RESOURCE  = 0x008,
	D3D11_BIND_STREAM_OUTPUT    = 0x010,
	D3D11_BIND_RENDER_TARGET    = 0x020,
	D3D11_BIND_DEPTH_STENCIL    = 0x040,
	D3D11_BIND_UNORDERED_ACCESS = 0x080,
} D3D11_BIND_FLAG;

/* 프리미티브 토폴로지 (그리기 방식) */
typedef enum {
	D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED     = 0,
	D3D11_PRIMITIVE_TOPOLOGY_POINTLIST     = 1,
	D3D11_PRIMITIVE_TOPOLOGY_LINELIST      = 2,
	D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP     = 3,
	D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST  = 4,
	D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5,
} D3D11_PRIMITIVE_TOPOLOGY;

/* 드라이버 타입 */
typedef enum {
	D3D_DRIVER_TYPE_UNKNOWN   = 0,
	D3D_DRIVER_TYPE_HARDWARE  = 1,
	D3D_DRIVER_TYPE_REFERENCE = 2,
	D3D_DRIVER_TYPE_NULL      = 3,
	D3D_DRIVER_TYPE_SOFTWARE  = 4,
	D3D_DRIVER_TYPE_WARP      = 5,
} D3D_DRIVER_TYPE;

/* 기능 수준 (Feature Level) */
typedef enum {
	D3D_FEATURE_LEVEL_9_1  = 0x9100,
	D3D_FEATURE_LEVEL_9_2  = 0x9200,
	D3D_FEATURE_LEVEL_9_3  = 0x9300,
	D3D_FEATURE_LEVEL_10_0 = 0xa000,
	D3D_FEATURE_LEVEL_10_1 = 0xa100,
	D3D_FEATURE_LEVEL_11_0 = 0xb000,
	D3D_FEATURE_LEVEL_11_1 = 0xb100,
} D3D_FEATURE_LEVEL;

/* Map 타입 (리소스 CPU 접근 방식) */
typedef enum {
	D3D11_MAP_READ               = 1,
	D3D11_MAP_WRITE              = 2,
	D3D11_MAP_READ_WRITE         = 3,
	D3D11_MAP_WRITE_DISCARD      = 4,
	D3D11_MAP_WRITE_NO_OVERWRITE = 5,
} D3D11_MAP;

/* 텍스처 차원 */
typedef enum {
	D3D11_SRV_DIMENSION_TEXTURE2D = 4,
} D3D11_SRV_DIMENSION;

/* D3D11 Create Device 플래그 */
#define D3D11_CREATE_DEVICE_SINGLETHREADED  0x1
#define D3D11_CREATE_DEVICE_DEBUG           0x2

/* DXGI Usage 플래그 */
#define DXGI_USAGE_RENDER_TARGET_OUTPUT     0x020
#define DXGI_USAGE_SHADER_INPUT             0x010

/* ============================================================
 * DXGI 구조체
 * ============================================================ */

/* 디스플레이 모드 */
typedef struct {
	UINT        Width;
	UINT        Height;
	UINT        RefreshRate_Numerator;
	UINT        RefreshRate_Denominator;
	DXGI_FORMAT Format;
	UINT        ScanlineOrdering;
	UINT        Scaling;
} DXGI_MODE_DESC;

/* 멀티샘플링 설정 */
typedef struct {
	UINT Count;
	UINT Quality;
} DXGI_SAMPLE_DESC;

/* 스왑 체인 설명 */
typedef struct {
	DXGI_MODE_DESC   BufferDesc;
	DXGI_SAMPLE_DESC SampleDesc;
	UINT             BufferUsage;
	UINT             BufferCount;
	HWND             OutputWindow;
	BOOL             Windowed;
	UINT             SwapEffect;
	UINT             Flags;
} DXGI_SWAP_CHAIN_DESC;

/* 어댑터 설명 (GPU 정보) */
typedef struct {
	uint16_t Description[128];  /* WCHAR 문자열 */
	UINT     VendorId;
	UINT     DeviceId;
	UINT     SubSysId;
	UINT     Revision;
	size_t   DedicatedVideoMemory;
	size_t   DedicatedSystemMemory;
	size_t   SharedSystemMemory;
	GUID     AdapterLuid;       /* 실제로는 LUID이지만 크기 맞춤 */
} DXGI_ADAPTER_DESC;

/* ============================================================
 * D3D11 구조체
 * ============================================================ */

/* 버퍼 생성 파라미터 */
typedef struct {
	UINT        ByteWidth;
	D3D11_USAGE Usage;
	UINT        BindFlags;
	UINT        CPUAccessFlags;
	UINT        MiscFlags;
	UINT        StructureByteStride;
} D3D11_BUFFER_DESC;

/* 초기 데이터 (리소스 생성 시 제공) */
typedef struct {
	const void *pSysMem;
	UINT        SysMemPitch;
	UINT        SysMemSlicePitch;
} D3D11_SUBRESOURCE_DATA;

/* 뷰포트 (렌더링 영역 정의) */
typedef struct {
	float TopLeftX;
	float TopLeftY;
	float Width;
	float Height;
	float MinDepth;
	float MaxDepth;
} D3D11_VIEWPORT;

/* 2D 텍스처 생성 파라미터 */
typedef struct {
	UINT             Width;
	UINT             Height;
	UINT             MipLevels;
	UINT             ArraySize;
	DXGI_FORMAT      Format;
	DXGI_SAMPLE_DESC SampleDesc;
	D3D11_USAGE      Usage;
	UINT             BindFlags;
	UINT             CPUAccessFlags;
	UINT             MiscFlags;
} D3D11_TEXTURE2D_DESC;

/* 렌더 타깃 뷰 생성 파라미터 */
typedef struct {
	DXGI_FORMAT Format;
	UINT        ViewDimension;
	union {
		struct { UINT MipSlice; } Texture2D;
	};
} D3D11_RENDER_TARGET_VIEW_DESC;

/* 입력 레이아웃 요소 (버텍스 포맷 설명)
 *
 * DirectX 버텍스 셰이더의 입력을 정의:
 *   SemanticName = "POSITION", "COLOR", "TEXCOORD" 등
 *   Format = 데이터 타입 (R32G32B32_FLOAT = float3)
 *   AlignedByteOffset = 버텍스 구조체 내 오프셋
 */
typedef struct {
	LPCSTR      SemanticName;
	UINT        SemanticIndex;
	DXGI_FORMAT Format;
	UINT        InputSlot;
	UINT        AlignedByteOffset;
	UINT        InputSlotClass;
	UINT        InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;

/* Map 결과 */
typedef struct {
	void *pData;
	UINT  RowPitch;
	UINT  DepthPitch;
} D3D11_MAPPED_SUBRESOURCE;

/* ============================================================
 * COM 인터페이스 전방 선언
 * ============================================================
 *
 * C에서 COM 인터페이스를 사용하는 패턴:
 *   struct { vtbl *lpVtbl; ... };
 *
 * 앱은 포인터로만 다루므로, 여기서는 vtable만 선언.
 * 실제 구현은 dxgi.c / d3d11.c 내부.
 */

/* --- DXGI 인터페이스 --- */

/* IDXGIAdapter vtable */
typedef struct IDXGIAdapterVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *This, REFIID riid, void **ppv);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *This);
	ULONG   (__attribute__((ms_abi)) *Release)(void *This);
	/* IDXGIObject */
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *This, REFIID Name,
							  UINT DataSize, const void *pData);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *This, REFIID Name,
							  UINT *pDataSize, void *pData);
	HRESULT (__attribute__((ms_abi)) *GetParent)(void *This, REFIID riid, void **ppParent);
	/* IDXGIAdapter */
	HRESULT (__attribute__((ms_abi)) *EnumOutputs)(void *This, UINT Output, void **ppOutput);
	HRESULT (__attribute__((ms_abi)) *GetDesc)(void *This, DXGI_ADAPTER_DESC *pDesc);
	HRESULT (__attribute__((ms_abi)) *CheckInterfaceSupport)(void *This, REFIID InterfaceName,
								 void *pUMDVersion);
} IDXGIAdapterVtbl;

/* IDXGISwapChain vtable */
typedef struct IDXGISwapChainVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *This, REFIID riid, void **ppv);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *This);
	ULONG   (__attribute__((ms_abi)) *Release)(void *This);
	/* IDXGIObject */
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *This, REFIID Name,
							  UINT DataSize, const void *pData);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *This, REFIID Name,
							  UINT *pDataSize, void *pData);
	HRESULT (__attribute__((ms_abi)) *GetParent)(void *This, REFIID riid, void **ppParent);
	/* IDXGIDeviceSubObject */
	HRESULT (__attribute__((ms_abi)) *GetDevice)(void *This, REFIID riid, void **ppDevice);
	/* IDXGISwapChain */
	HRESULT (__attribute__((ms_abi)) *Present)(void *This, UINT SyncInterval, UINT Flags);
	HRESULT (__attribute__((ms_abi)) *GetBuffer)(void *This, UINT Buffer,
						     REFIID riid, void **ppSurface);
	HRESULT (__attribute__((ms_abi)) *SetFullscreenState)(void *This, BOOL Fullscreen,
							      void *pTarget);
	HRESULT (__attribute__((ms_abi)) *GetFullscreenState)(void *This, BOOL *pFullscreen,
							      void **ppTarget);
	HRESULT (__attribute__((ms_abi)) *GetDesc)(void *This, DXGI_SWAP_CHAIN_DESC *pDesc);
	HRESULT (__attribute__((ms_abi)) *ResizeBuffers)(void *This, UINT BufferCount,
							 UINT Width, UINT Height,
							 DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	HRESULT (__attribute__((ms_abi)) *ResizeTarget)(void *This, const DXGI_MODE_DESC *pNewTargetParams);
} IDXGISwapChainVtbl;

/* IDXGIFactory vtable */
typedef struct IDXGIFactoryVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *This, REFIID riid, void **ppv);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *This);
	ULONG   (__attribute__((ms_abi)) *Release)(void *This);
	/* IDXGIObject */
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *This, REFIID Name,
							  UINT DataSize, const void *pData);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *This, REFIID Name,
							  UINT *pDataSize, void *pData);
	HRESULT (__attribute__((ms_abi)) *GetParent)(void *This, REFIID riid, void **ppParent);
	/* IDXGIFactory */
	HRESULT (__attribute__((ms_abi)) *EnumAdapters)(void *This, UINT Adapter, void **ppAdapter);
	HRESULT (__attribute__((ms_abi)) *MakeWindowAssociation)(void *This, HWND WindowHandle,
								 UINT Flags);
	HRESULT (__attribute__((ms_abi)) *GetWindowAssociation)(void *This, HWND *pWindowHandle);
	HRESULT (__attribute__((ms_abi)) *CreateSwapChain)(void *This, void *pDevice,
							   DXGI_SWAP_CHAIN_DESC *pDesc,
							   void **ppSwapChain);
	HRESULT (__attribute__((ms_abi)) *CreateSoftwareAdapter)(void *This, void *Module,
								 void **ppAdapter);
} IDXGIFactoryVtbl;

/* --- D3D11 인터페이스 --- */

/* ID3D11Device vtable (간략화 — 실제 순서 유지) */
typedef struct ID3D11DeviceVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *This, REFIID riid, void **ppv);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *This);
	ULONG   (__attribute__((ms_abi)) *Release)(void *This);
	/* ID3D11Device — 리소스 생성 */
	HRESULT (__attribute__((ms_abi)) *CreateBuffer)(void *This,
		const D3D11_BUFFER_DESC *pDesc,
		const D3D11_SUBRESOURCE_DATA *pInitialData,
		void **ppBuffer);
	HRESULT (__attribute__((ms_abi)) *CreateTexture1D)(void *This, void *d, void *i, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateTexture2D)(void *This,
		const D3D11_TEXTURE2D_DESC *pDesc,
		const D3D11_SUBRESOURCE_DATA *pInitialData,
		void **ppTexture2D);
	HRESULT (__attribute__((ms_abi)) *CreateTexture3D)(void *This, void *d, void *i, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateShaderResourceView)(void *This,
		void *pResource, void *pDesc, void **ppSRView);
	HRESULT (__attribute__((ms_abi)) *CreateUnorderedAccessView)(void *This,
		void *pResource, void *pDesc, void **ppUAView);
	HRESULT (__attribute__((ms_abi)) *CreateRenderTargetView)(void *This,
		void *pResource,
		const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
		void **ppRTView);
	HRESULT (__attribute__((ms_abi)) *CreateDepthStencilView)(void *This,
		void *pResource, void *pDesc, void **ppDSView);
	HRESULT (__attribute__((ms_abi)) *CreateInputLayout)(void *This,
		const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs,
		UINT NumElements,
		const void *pShaderBytecodeWithInputSignature,
		size_t BytecodeLength,
		void **ppInputLayout);
	HRESULT (__attribute__((ms_abi)) *CreateVertexShader)(void *This,
		const void *pShaderBytecode, size_t BytecodeLength,
		void *pClassLinkage, void **ppVertexShader);
	HRESULT (__attribute__((ms_abi)) *CreateHullShader)(void *This,
		const void *p, size_t l, void *c, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateDomainShader)(void *This,
		const void *p, size_t l, void *c, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateGeometryShader)(void *This,
		const void *p, size_t l, void *c, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateGeometryShaderWithStreamOutput)(void *This,
		const void *p, size_t l, void *so, UINT ne, void *bs,
		UINT nb, UINT rs, void *c, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreatePixelShader)(void *This,
		const void *pShaderBytecode, size_t BytecodeLength,
		void *pClassLinkage, void **ppPixelShader);
	/* ... 이후 메서드는 Phase 4에서 사용하지 않으므로 생략 가능 ... */
	/* 하지만 vtable 순서가 중요하므로 placeholder 유지 */
	HRESULT (__attribute__((ms_abi)) *CreateBlendState)(void *T, void *d, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateDepthStencilState)(void *T, void *d, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateRasterizerState)(void *T, void *d, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateSamplerState)(void *T, void *d, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateQuery)(void *T, void *d, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreatePredicate)(void *T, void *d, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateCounter)(void *T, void *d, void **pp);
	HRESULT (__attribute__((ms_abi)) *CreateDeferredContext)(void *T, UINT f, void **pp);
	HRESULT (__attribute__((ms_abi)) *OpenSharedResource)(void *T, void *h, REFIID r, void **pp);
	HRESULT (__attribute__((ms_abi)) *CheckFormatSupport)(void *T, DXGI_FORMAT f, UINT *p);
	HRESULT (__attribute__((ms_abi)) *CheckMultisampleQualityLevels)(void *T,
		DXGI_FORMAT f, UINT sc, UINT *p);
	void    (__attribute__((ms_abi)) *CheckCounterInfo)(void *T, void *p);
	HRESULT (__attribute__((ms_abi)) *CheckCounter)(void *T, void *d, void *t,
		void *n, UINT *nl, void *u, UINT *ul, void *desc, UINT *dl);
	HRESULT (__attribute__((ms_abi)) *CheckFeatureSupport)(void *T, UINT f, void *s, UINT sz);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *T, REFIID g, UINT *s, void *d);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *T, REFIID g, UINT s, const void *d);
	HRESULT (__attribute__((ms_abi)) *SetPrivateDataInterface)(void *T, REFIID g, void *d);
	D3D_FEATURE_LEVEL (__attribute__((ms_abi)) *GetFeatureLevel)(void *T);
	UINT    (__attribute__((ms_abi)) *GetCreationFlags)(void *T);
	HRESULT (__attribute__((ms_abi)) *GetDeviceRemovedReason)(void *T);
	void    (__attribute__((ms_abi)) *GetImmediateContext)(void *T, void **pp);
	HRESULT (__attribute__((ms_abi)) *SetExceptionMode)(void *T, UINT f);
	UINT    (__attribute__((ms_abi)) *GetExceptionMode)(void *T);
} ID3D11DeviceVtbl;

/* ID3D11DeviceContext vtable (간략화 — 파이프라인 스테이지 메서드) */
typedef struct ID3D11DeviceContextVtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *This, REFIID riid, void **ppv);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *This);
	ULONG   (__attribute__((ms_abi)) *Release)(void *This);
	/* ID3D11DeviceChild */
	void    (__attribute__((ms_abi)) *GetDevice)(void *This, void **ppDevice);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *T, REFIID g, UINT *s, void *d);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *T, REFIID g, UINT s, const void *d);
	HRESULT (__attribute__((ms_abi)) *SetPrivateDataInterface)(void *T, REFIID g, void *d);
	/* ID3D11DeviceContext — VS 스테이지 */
	void (__attribute__((ms_abi)) *VSSetConstantBuffers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *PSSetShaderResources)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *PSSetShader)(void *This, void *pPS, void *const *ppCI, UINT nCI);
	void (__attribute__((ms_abi)) *PSSetSamplers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *VSSetShader)(void *This, void *pVS, void *const *ppCI, UINT nCI);
	void (__attribute__((ms_abi)) *DrawIndexed)(void *This, UINT IndexCount,
						    UINT StartIndexLocation, int BaseVertexLocation);
	void (__attribute__((ms_abi)) *Draw)(void *This, UINT VertexCount, UINT StartVertexLocation);
	HRESULT (__attribute__((ms_abi)) *Map)(void *This, void *pResource, UINT Subresource,
					       D3D11_MAP MapType, UINT MapFlags,
					       D3D11_MAPPED_SUBRESOURCE *pMapped);
	void (__attribute__((ms_abi)) *Unmap)(void *This, void *pResource, UINT Subresource);
	void (__attribute__((ms_abi)) *PSSetConstantBuffers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *IASetInputLayout)(void *This, void *pInputLayout);
	void (__attribute__((ms_abi)) *IASetVertexBuffers)(void *This, UINT StartSlot,
		UINT NumBuffers, void *const *ppVertexBuffers,
		const UINT *pStrides, const UINT *pOffsets);
	void (__attribute__((ms_abi)) *IASetIndexBuffer)(void *This, void *pIndexBuffer,
							 DXGI_FORMAT Format, UINT Offset);
	void (__attribute__((ms_abi)) *DrawIndexedInstanced)(void *T, UINT ic, UINT inst,
							     UINT si, int bv, UINT sil);
	void (__attribute__((ms_abi)) *DrawInstanced)(void *T, UINT vc, UINT ic, UINT sv, UINT si);
	void (__attribute__((ms_abi)) *GSSetConstantBuffers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *GSSetShader)(void *T, void *p, void *const *ci, UINT n);
	void (__attribute__((ms_abi)) *IASetPrimitiveTopology)(void *This,
							      D3D11_PRIMITIVE_TOPOLOGY Topology);
	void (__attribute__((ms_abi)) *VSSetShaderResources)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *VSSetSamplers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *Begin)(void *T, void *p);
	void (__attribute__((ms_abi)) *End)(void *T, void *p);
	HRESULT (__attribute__((ms_abi)) *GetData)(void *T, void *p, void *d, UINT s, UINT f);
	void (__attribute__((ms_abi)) *SetPredication)(void *T, void *p, BOOL v);
	void (__attribute__((ms_abi)) *GSSetShaderResources)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *GSSetSamplers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *OMSetRenderTargets)(void *This, UINT NumViews,
		void *const *ppRenderTargetViews, void *pDepthStencilView);
	void (__attribute__((ms_abi)) *OMSetRenderTargetsAndUnorderedAccessViews)(void *T,
		UINT nrtv, void *const *rtv, void *dsv, UINT us, UINT nu,
		void *const *uav, const UINT *ic);
	void (__attribute__((ms_abi)) *OMSetBlendState)(void *T, void *p, const float f[4], UINT m);
	void (__attribute__((ms_abi)) *OMSetDepthStencilState)(void *T, void *p, UINT r);
	void (__attribute__((ms_abi)) *SOSetTargets)(void *T, UINT n, void *const *pp, const UINT *o);
	void (__attribute__((ms_abi)) *DrawAuto)(void *T);
	void (__attribute__((ms_abi)) *DrawIndexedInstancedIndirect)(void *T, void *b, UINT o);
	void (__attribute__((ms_abi)) *DrawInstancedIndirect)(void *T, void *b, UINT o);
	void (__attribute__((ms_abi)) *Dispatch)(void *T, UINT x, UINT y, UINT z);
	void (__attribute__((ms_abi)) *DispatchIndirect)(void *T, void *b, UINT o);
	void (__attribute__((ms_abi)) *RSSetState)(void *T, void *p);
	void (__attribute__((ms_abi)) *RSSetViewports)(void *This, UINT NumViewports,
						       const D3D11_VIEWPORT *pViewports);
	void (__attribute__((ms_abi)) *RSSetScissorRects)(void *T, UINT n, const void *p);
	void (__attribute__((ms_abi)) *CopySubresourceRegion)(void *T,
		void *dst, UINT di, UINT dx, UINT dy, UINT dz,
		void *src, UINT si, void *sb);
	void (__attribute__((ms_abi)) *CopyResource)(void *T, void *dst, void *src);
	void (__attribute__((ms_abi)) *UpdateSubresource)(void *T,
		void *dst, UINT sub, void *box, const void *data, UINT rp, UINT dp);
	void (__attribute__((ms_abi)) *CopyStructureCount)(void *T, void *dst, UINT o, void *src);
	void (__attribute__((ms_abi)) *ClearRenderTargetView)(void *This,
		void *pRenderTargetView, const float ColorRGBA[4]);
	void (__attribute__((ms_abi)) *ClearUnorderedAccessViewUint)(void *T, void *v, const UINT c[4]);
	void (__attribute__((ms_abi)) *ClearUnorderedAccessViewFloat)(void *T, void *v, const float c[4]);
	void (__attribute__((ms_abi)) *ClearDepthStencilView)(void *T, void *v,
		UINT cf, float d, uint8_t s);
	void (__attribute__((ms_abi)) *GenerateMips)(void *T, void *v);
	void (__attribute__((ms_abi)) *SetResourceMinLOD)(void *T, void *r, float l);
	float (__attribute__((ms_abi)) *GetResourceMinLOD)(void *T, void *r);
	void (__attribute__((ms_abi)) *ResolveSubresource)(void *T,
		void *dst, UINT di, void *src, UINT si, DXGI_FORMAT f);
	void (__attribute__((ms_abi)) *ExecuteCommandList)(void *T, void *cl, BOOL r);
	void (__attribute__((ms_abi)) *HSSetShaderResources)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *HSSetShader)(void *T, void *p, void *const *ci, UINT n);
	void (__attribute__((ms_abi)) *HSSetSamplers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *HSSetConstantBuffers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *DSSetShaderResources)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *DSSetShader)(void *T, void *p, void *const *ci, UINT n);
	void (__attribute__((ms_abi)) *DSSetSamplers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *DSSetConstantBuffers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *CSSetShaderResources)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *CSSetUnorderedAccessViews)(void *T, UINT s, UINT n,
		void *const *pp, const UINT *ic);
	void (__attribute__((ms_abi)) *CSSetShader)(void *T, void *p, void *const *ci, UINT n);
	void (__attribute__((ms_abi)) *CSSetSamplers)(void *T, UINT s, UINT n, void *const *pp);
	void (__attribute__((ms_abi)) *CSSetConstantBuffers)(void *T, UINT s, UINT n, void *const *pp);
	/* Get 메서드 다수 생략 — Phase 4에서 호출하지 않음 */
	void (__attribute__((ms_abi)) *VSGetConstantBuffers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *PSGetShaderResources)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *PSGetShader)(void *T, void **pp, void **ci, UINT *n);
	void (__attribute__((ms_abi)) *PSGetSamplers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *VSGetShader)(void *T, void **pp, void **ci, UINT *n);
	void (__attribute__((ms_abi)) *PSGetConstantBuffers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *IAGetInputLayout)(void *T, void **pp);
	void (__attribute__((ms_abi)) *IAGetVertexBuffers)(void *T, UINT s, UINT n,
		void **pp, UINT *st, UINT *of);
	void (__attribute__((ms_abi)) *IAGetIndexBuffer)(void *T, void **pp, DXGI_FORMAT *f, UINT *o);
	void (__attribute__((ms_abi)) *GSGetConstantBuffers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *GSGetShader)(void *T, void **pp, void **ci, UINT *n);
	void (__attribute__((ms_abi)) *IAGetPrimitiveTopology)(void *T, D3D11_PRIMITIVE_TOPOLOGY *p);
	void (__attribute__((ms_abi)) *VSGetShaderResources)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *VSGetSamplers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *GetPredication)(void *T, void **pp, BOOL *v);
	void (__attribute__((ms_abi)) *GSGetShaderResources)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *GSGetSamplers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *OMGetRenderTargets)(void *T, UINT n, void **rtv, void **dsv);
	void (__attribute__((ms_abi)) *OMGetRenderTargetsAndUnorderedAccessViews)(void *T,
		UINT nrtv, void **rtv, void **dsv, UINT us, UINT nu, void **uav);
	void (__attribute__((ms_abi)) *OMGetBlendState)(void *T, void **pp, float f[4], UINT *m);
	void (__attribute__((ms_abi)) *OMGetDepthStencilState)(void *T, void **pp, UINT *r);
	void (__attribute__((ms_abi)) *SOGetTargets)(void *T, UINT n, void **pp);
	void (__attribute__((ms_abi)) *RSGetState)(void *T, void **pp);
	void (__attribute__((ms_abi)) *RSGetViewports)(void *T, UINT *n, D3D11_VIEWPORT *p);
	void (__attribute__((ms_abi)) *RSGetScissorRects)(void *T, UINT *n, void *p);
	/* 나머지 Get 메서드... */
	void (__attribute__((ms_abi)) *HSGetShaderResources)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *HSGetShader)(void *T, void **pp, void **ci, UINT *n);
	void (__attribute__((ms_abi)) *HSGetSamplers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *HSGetConstantBuffers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *DSGetShaderResources)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *DSGetShader)(void *T, void **pp, void **ci, UINT *n);
	void (__attribute__((ms_abi)) *DSGetSamplers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *DSGetConstantBuffers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *CSGetShaderResources)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *CSGetUnorderedAccessViews)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *CSGetShader)(void *T, void **pp, void **ci, UINT *n);
	void (__attribute__((ms_abi)) *CSGetSamplers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *CSGetConstantBuffers)(void *T, UINT s, UINT n, void **pp);
	void (__attribute__((ms_abi)) *ClearState)(void *This);
	void (__attribute__((ms_abi)) *Flush)(void *This);
	UINT (__attribute__((ms_abi)) *GetType)(void *This);
	UINT (__attribute__((ms_abi)) *GetContextFlags)(void *This);
	HRESULT (__attribute__((ms_abi)) *FinishCommandList)(void *T, BOOL r, void **pp);
} ID3D11DeviceContextVtbl;

#endif /* CITC_D3D11_TYPES_H */
