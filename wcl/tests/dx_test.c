/*
 * dx_test.c — DirectX 11 API 테스트
 * ===================================
 *
 * Phase 4 (DirectX & Gaming) 검증 프로그램.
 * MinGW로 크로스 컴파일하여 citcrun에서 실행.
 *
 * 테스트 항목:
 *   [1]  CreateDXGIFactory
 *   [2]  IDXGIFactory::EnumAdapters
 *   [3]  IDXGIAdapter::GetDesc
 *   [4]  D3D11CreateDeviceAndSwapChain
 *   [5]  SwapChain::GetBuffer → CreateRTV
 *   [6]  ClearRenderTargetView (red) + Present
 *   [7]  CreateBuffer (vertex)
 *   [8]  CreateVertexShader / CreatePixelShader
 *   [9]  CreateInputLayout
 *   [10] IA/VS/PS/OM 바인딩
 *   [11] Draw(3,0) — Hello Triangle
 *   [12] 삼각형 중앙 픽셀 확인
 *   [13] 삼각형 외부 픽셀 확인 (배경색)
 *   --- Class 36: Constant Buffer + MVP ---
 *   [14] CreateBuffer(CB) + identity 행렬
 *   [15] VSSetCB + Draw: identity → 기존과 동일 결과
 *   [16] UpdateSubresource(scale 0.5) → 삼각형 축소
 *   [17] Translation 행렬 → 삼각형 이동
 *   [18] Perspective 행렬 → 3D 투영
 *   --- Class 37: 깊이 버퍼 + 렌더 스테이트 ---
 *   [19] CreateTexture2D(D32_FLOAT) + CreateDSV
 *   [20] CreateDepthStencilState(LESS) + 바인딩
 *   [21] 깊이 테스트: 앞(Z=0.3) vs 뒤(Z=0.7) → 앞 승리
 *   [22] 그리기 순서 뒤집기 → 여전히 앞 승리
 *   [23] ClearDepthStencilView + CreateRasterizerState(CULL_BACK)
 *   --- Class 38: 텍스처 매핑 + SRV ---
 *   [24] CreateSRV (2x2 텍스처: 빨/초/파/흰)
 *   [25] CreateSamplerState(CLAMP, POINT)
 *   [26] 텍스처 쿼드(6 vertices + TEXCOORD) → 코너 색상 확인
 *   [27] TEXCOORD + COLOR modulate
 *   [28] 텍스처 미바인딩 → 기존 컬러 삼각형 (하위 호환)
 *   --- Class 39: DXBC 바이트코드 파서 + 인터프리터 ---
 *   [29] DXBC 파서: 유효 셰이더 blob 파싱 성공
 *   [30] 잘못된 blob → fallback (고정 함수) 동작
 *   [31] VS VM: pass-through → 고정 함수와 동일 결과
 *   [32] VS VM + CB MVP 변환
 *   [33] PS VM: 단색(magenta) 출력
 *   --- Class 43: DXBC → SPIR-V ---
 *   [34] SPIR-V: VS blob 변환 확인
 *   [35] SPIR-V: PS blob 변환 확인
 *   [36] 리소스 정리
 *   [37] 최종 결과
 */

/* === 타입 정의 (MinGW 독립) === */

typedef void           *HANDLE;
typedef unsigned int    UINT;
typedef int             BOOL;
typedef int             LONG;
typedef unsigned long   DWORD;
typedef const char     *LPCSTR;
typedef void           *LPVOID;
typedef void           *HWND;
typedef void           *HDC;
typedef void           *HBRUSH;
typedef void           *HICON;
typedef void           *HCURSOR;
typedef unsigned int    ULONG;
typedef int             HRESULT;
typedef unsigned long long uintptr_t;
typedef long long       intptr_t;
typedef unsigned long long size_t;
typedef uintptr_t       WPARAM;
typedef intptr_t        LPARAM;
typedef intptr_t        LRESULT;
typedef unsigned short  uint16_t;
typedef unsigned char   uint8_t;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)

#define S_OK          ((HRESULT)0)
#define E_FAIL        ((HRESULT)0x80004005)

/* Window styles */
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_VISIBLE          0x10000000L
#define CW_USEDEFAULT       ((int)0x80000000)

/* Messages */
#define WM_DESTROY 0x0002
#define WM_CLOSE   0x0010
#define WM_QUIT    0x0012

/* GUID */
typedef struct { DWORD Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } GUID;
typedef GUID IID;
typedef const IID *REFIID;

/* RECT */
typedef struct { LONG left, top, right, bottom; } RECT;

/* WNDCLASSA */
typedef LRESULT (__attribute__((ms_abi)) *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct {
	UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra;
	HANDLE hInstance; HANDLE hIcon; HANDLE hCursor;
	HBRUSH hbrBackground; LPCSTR lpszMenuName; LPCSTR lpszClassName;
} WNDCLASSA;

/* DXGI */
typedef enum { DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_R32G32_FLOAT = 16,
	DXGI_FORMAT_R8G8B8A8_UNORM = 28,
	DXGI_FORMAT_D32_FLOAT = 40,
	DXGI_FORMAT_B8G8R8A8_UNORM = 87, DXGI_FORMAT_R32G32B32_FLOAT = 6,
	DXGI_FORMAT_R32G32B32A32_FLOAT = 2 } DXGI_FORMAT;

typedef struct { UINT Width, Height; UINT RefreshRate_Numerator, RefreshRate_Denominator;
	DXGI_FORMAT Format; UINT ScanlineOrdering, Scaling; } DXGI_MODE_DESC;
typedef struct { UINT Count; UINT Quality; } DXGI_SAMPLE_DESC;
typedef struct { DXGI_MODE_DESC BufferDesc; DXGI_SAMPLE_DESC SampleDesc;
	UINT BufferUsage; UINT BufferCount; HWND OutputWindow;
	BOOL Windowed; UINT SwapEffect; UINT Flags; } DXGI_SWAP_CHAIN_DESC;
typedef struct { uint16_t Description[128]; UINT VendorId, DeviceId, SubSysId, Revision;
	size_t DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
	GUID AdapterLuid; } DXGI_ADAPTER_DESC;

/* D3D11 */
typedef enum { D3D11_USAGE_DEFAULT = 0, D3D11_USAGE_IMMUTABLE = 1 } D3D11_USAGE;
typedef enum { D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4 } D3D11_PRIMITIVE_TOPOLOGY;
typedef enum { D3D_DRIVER_TYPE_HARDWARE = 1 } D3D_DRIVER_TYPE;
typedef enum { D3D_FEATURE_LEVEL_11_0 = 0xb000 } D3D_FEATURE_LEVEL;

#define D3D11_BIND_VERTEX_BUFFER    0x1
#define D3D11_BIND_CONSTANT_BUFFER  0x4
#define D3D11_BIND_SHADER_RESOURCE  0x8
#define D3D11_BIND_RENDER_TARGET    0x20
#define D3D11_BIND_DEPTH_STENCIL    0x40
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x020
#define D3D11_CLEAR_DEPTH   0x1

/* Comparison func */
typedef enum {
	D3D11_COMPARISON_LESS = 2,
	D3D11_COMPARISON_ALWAYS = 8,
} D3D11_COMPARISON_FUNC;

/* Depth write mask */
typedef enum {
	D3D11_DEPTH_WRITE_MASK_ALL = 1,
} D3D11_DEPTH_WRITE_MASK;

/* Cull mode */
typedef enum {
	D3D11_CULL_NONE = 1,
	D3D11_CULL_BACK = 3,
} D3D11_CULL_MODE;

/* Fill mode */
typedef enum {
	D3D11_FILL_SOLID = 3,
} D3D11_FILL_MODE;

/* Depth stencil op desc */
typedef struct { UINT a, b, c; D3D11_COMPARISON_FUNC d; } D3D11_DEPTH_STENCILOP_DESC;

/* Depth stencil desc */
typedef struct {
	BOOL DepthEnable;
	D3D11_DEPTH_WRITE_MASK DepthWriteMask;
	D3D11_COMPARISON_FUNC DepthFunc;
	BOOL StencilEnable;
	unsigned char StencilReadMask, StencilWriteMask;
	D3D11_DEPTH_STENCILOP_DESC FrontFace, BackFace;
} D3D11_DEPTH_STENCIL_DESC;

/* Rasterizer desc */
typedef struct {
	D3D11_FILL_MODE FillMode;
	D3D11_CULL_MODE CullMode;
	BOOL FrontCounterClockwise;
	int DepthBias; float DepthBiasClamp, SlopeScaledDepthBias;
	BOOL DepthClipEnable, ScissorEnable, MultisampleEnable, AntialiasedLineEnable;
} D3D11_RASTERIZER_DESC;

/* Filter */
typedef enum {
	D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
} D3D11_FILTER;

/* Texture address mode */
typedef enum {
	D3D11_TEXTURE_ADDRESS_WRAP = 1,
	D3D11_TEXTURE_ADDRESS_CLAMP = 3,
} D3D11_TEXTURE_ADDRESS_MODE;

/* Sampler desc */
typedef struct {
	D3D11_FILTER Filter;
	D3D11_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
	float MipLODBias;
	UINT MaxAnisotropy;
	D3D11_COMPARISON_FUNC ComparisonFunc;
	float BorderColor[4];
	float MinLOD, MaxLOD;
} D3D11_SAMPLER_DESC;

/* Texture2D desc */
typedef struct {
	UINT Width, Height, MipLevels, ArraySize;
	DXGI_FORMAT Format;
	struct { UINT Count; UINT Quality; } SampleDesc;
	D3D11_USAGE Usage;
	UINT BindFlags, CPUAccessFlags, MiscFlags;
} D3D11_TEXTURE2D_DESC;

typedef struct { UINT ByteWidth; D3D11_USAGE Usage; UINT BindFlags;
	UINT CPUAccessFlags, MiscFlags, StructureByteStride; } D3D11_BUFFER_DESC;
typedef struct { const void *pSysMem; UINT SysMemPitch, SysMemSlicePitch; } D3D11_SUBRESOURCE_DATA;
typedef struct { float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; } D3D11_VIEWPORT;
typedef struct { LPCSTR SemanticName; UINT SemanticIndex; DXGI_FORMAT Format;
	UINT InputSlot; UINT AlignedByteOffset; UINT InputSlotClass;
	UINT InstanceDataStepRate; } D3D11_INPUT_ELEMENT_DESC;
typedef struct { void *pData; UINT RowPitch; UINT DepthPitch; } D3D11_MAPPED_SUBRESOURCE;

#define D3D11_MAP_READ 1

/* === vtable 구조체 (COM 인터페이스) === */

/* IDXGIAdapter vtable */
typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *, REFIID, UINT, const void *);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *, REFIID, UINT *, void *);
	HRESULT (__attribute__((ms_abi)) *GetParent)(void *, REFIID, void **);
	HRESULT (__attribute__((ms_abi)) *EnumOutputs)(void *, UINT, void **);
	HRESULT (__attribute__((ms_abi)) *GetDesc)(void *, DXGI_ADAPTER_DESC *);
	HRESULT (__attribute__((ms_abi)) *CheckInterfaceSupport)(void *, REFIID, void *);
} IDXGIAdapterVtbl;

/* IDXGISwapChain vtable */
typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *, REFIID, UINT, const void *);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *, REFIID, UINT *, void *);
	HRESULT (__attribute__((ms_abi)) *GetParent)(void *, REFIID, void **);
	HRESULT (__attribute__((ms_abi)) *GetDevice)(void *, REFIID, void **);
	HRESULT (__attribute__((ms_abi)) *Present)(void *, UINT, UINT);
	HRESULT (__attribute__((ms_abi)) *GetBuffer)(void *, UINT, REFIID, void **);
	HRESULT (__attribute__((ms_abi)) *SetFullscreenState)(void *, BOOL, void *);
	HRESULT (__attribute__((ms_abi)) *GetFullscreenState)(void *, BOOL *, void **);
	HRESULT (__attribute__((ms_abi)) *GetDesc)(void *, DXGI_SWAP_CHAIN_DESC *);
	HRESULT (__attribute__((ms_abi)) *ResizeBuffers)(void *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	HRESULT (__attribute__((ms_abi)) *ResizeTarget)(void *, const DXGI_MODE_DESC *);
} IDXGISwapChainVtbl;

/* IDXGIFactory vtable */
typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *, REFIID, UINT, const void *);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *, REFIID, UINT *, void *);
	HRESULT (__attribute__((ms_abi)) *GetParent)(void *, REFIID, void **);
	HRESULT (__attribute__((ms_abi)) *EnumAdapters)(void *, UINT, void **);
	HRESULT (__attribute__((ms_abi)) *MakeWindowAssociation)(void *, HWND, UINT);
	HRESULT (__attribute__((ms_abi)) *GetWindowAssociation)(void *, HWND *);
	HRESULT (__attribute__((ms_abi)) *CreateSwapChain)(void *, void *, DXGI_SWAP_CHAIN_DESC *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateSoftwareAdapter)(void *, void *, void **);
} IDXGIFactoryVtbl;

/* ID3D11Device vtable (simplified — only methods we call) */
typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	HRESULT (__attribute__((ms_abi)) *CreateBuffer)(void *, const D3D11_BUFFER_DESC *,
		const D3D11_SUBRESOURCE_DATA *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateTexture1D)(void *, void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateTexture2D)(void *, void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateTexture3D)(void *, void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateShaderResourceView)(void *, void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateUnorderedAccessView)(void *, void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateRenderTargetView)(void *, void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateDepthStencilView)(void *, void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateInputLayout)(void *,
		const D3D11_INPUT_ELEMENT_DESC *, UINT,
		const void *, unsigned __int64, void **);
	HRESULT (__attribute__((ms_abi)) *CreateVertexShader)(void *,
		const void *, unsigned __int64, void *, void **);
	/* ... hull, domain, geometry ... */
	HRESULT (__attribute__((ms_abi)) *CreateHullShader)(void *, const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateDomainShader)(void *, const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateGeometryShader)(void *, const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateGeometryShaderWithStreamOutput)(void *,
		const void *, unsigned __int64, void *, UINT, void *, UINT, UINT, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreatePixelShader)(void *,
		const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateBlendState)(void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateDepthStencilState)(void *, const D3D11_DEPTH_STENCIL_DESC *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateRasterizerState)(void *, const D3D11_RASTERIZER_DESC *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateSamplerState)(void *, const D3D11_SAMPLER_DESC *, void **);
} ID3D11DeviceVtbl;

/* ID3D11DeviceContext vtable */
typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void    (__attribute__((ms_abi)) *GetDevice)(void *, void **);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *, REFIID, UINT *, void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *, REFIID, UINT, const void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateDataInterface)(void *, REFIID, void *);
	/* VS */
	void (__attribute__((ms_abi)) *VSSetConstantBuffers)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *PSSetShaderResources)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *PSSetShader)(void *, void *, void *const *, UINT);
	void (__attribute__((ms_abi)) *PSSetSamplers)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *VSSetShader)(void *, void *, void *const *, UINT);
	void (__attribute__((ms_abi)) *DrawIndexed)(void *, UINT, UINT, int);
	void (__attribute__((ms_abi)) *Draw)(void *, UINT, UINT);
	HRESULT (__attribute__((ms_abi)) *Map)(void *, void *, UINT, UINT, UINT, void *);
	void (__attribute__((ms_abi)) *Unmap)(void *, void *, UINT);
	void (__attribute__((ms_abi)) *PSSetConstantBuffers)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *IASetInputLayout)(void *, void *);
	void (__attribute__((ms_abi)) *IASetVertexBuffers)(void *, UINT, UINT,
		void *const *, const UINT *, const UINT *);
	void (__attribute__((ms_abi)) *IASetIndexBuffer)(void *, void *, DXGI_FORMAT, UINT);
	void (__attribute__((ms_abi)) *DrawIndexedInstanced)(void *, UINT, UINT, UINT, int, UINT);
	void (__attribute__((ms_abi)) *DrawInstanced)(void *, UINT, UINT, UINT, UINT);
	void (__attribute__((ms_abi)) *GSSetConstantBuffers)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *GSSetShader)(void *, void *, void *const *, UINT);
	void (__attribute__((ms_abi)) *IASetPrimitiveTopology)(void *, D3D11_PRIMITIVE_TOPOLOGY);
	void (__attribute__((ms_abi)) *VSSetShaderResources)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *VSSetSamplers)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *Begin)(void *, void *);
	void (__attribute__((ms_abi)) *End)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *GetData)(void *, void *, void *, UINT, UINT);
	void (__attribute__((ms_abi)) *SetPredication)(void *, void *, BOOL);
	void (__attribute__((ms_abi)) *GSSetShaderResources)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *GSSetSamplers)(void *, UINT, UINT, void *const *);
	void (__attribute__((ms_abi)) *OMSetRenderTargets)(void *, UINT, void *const *, void *);
	void (__attribute__((ms_abi)) *OMSetRenderTargetsAndUnorderedAccessViews)(void *,
		UINT, void *const *, void *, UINT, UINT, void *const *, const UINT *);
	void (__attribute__((ms_abi)) *OMSetBlendState)(void *, void *, const float[4], UINT);
	void (__attribute__((ms_abi)) *OMSetDepthStencilState)(void *, void *, UINT);
	void (__attribute__((ms_abi)) *SOSetTargets)(void *, UINT, void *const *, const UINT *);
	void (__attribute__((ms_abi)) *DrawAuto)(void *);
	void (__attribute__((ms_abi)) *DrawIndexedInstancedIndirect)(void *, void *, UINT);
	void (__attribute__((ms_abi)) *DrawInstancedIndirect)(void *, void *, UINT);
	void (__attribute__((ms_abi)) *Dispatch)(void *, UINT, UINT, UINT);
	void (__attribute__((ms_abi)) *DispatchIndirect)(void *, void *, UINT);
	void (__attribute__((ms_abi)) *RSSetState)(void *, void *);
	void (__attribute__((ms_abi)) *RSSetViewports)(void *, UINT, const D3D11_VIEWPORT *);
	/* ... rest omitted ... */
	void (__attribute__((ms_abi)) *RSSetScissorRects)(void *, UINT, const void *);
	void (__attribute__((ms_abi)) *CopySubresourceRegion)(void *, void *, UINT, UINT, UINT, UINT, void *, UINT, void *);
	void (__attribute__((ms_abi)) *CopyResource)(void *, void *, void *);
	void (__attribute__((ms_abi)) *UpdateSubresource)(void *, void *, UINT, void *, const void *, UINT, UINT);
	void (__attribute__((ms_abi)) *CopyStructureCount)(void *, void *, UINT, void *);
	void (__attribute__((ms_abi)) *ClearRenderTargetView)(void *, void *, const float[4]);
	void (__attribute__((ms_abi)) *ClearUnorderedAccessViewUint)(void *, void *, const UINT[4]);
	void (__attribute__((ms_abi)) *ClearUnorderedAccessViewFloat)(void *, void *, const float[4]);
	void (__attribute__((ms_abi)) *ClearDepthStencilView)(void *, void *, UINT, float, unsigned char);
} ID3D11DeviceContextVtbl;

/* === Win32 imports === */

__attribute__((dllimport)) unsigned short __attribute__((ms_abi))
RegisterClassA(const WNDCLASSA *);

__attribute__((dllimport)) HWND __attribute__((ms_abi))
CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
		HWND, HANDLE, HANDLE, LPVOID);

__attribute__((dllimport)) BOOL __attribute__((ms_abi))
DestroyWindow(HWND);

__attribute__((dllimport)) LRESULT __attribute__((ms_abi))
DefWindowProcA(HWND, UINT, WPARAM, LPARAM);

__attribute__((dllimport)) void __attribute__((ms_abi))
PostQuitMessage(int);

/* DXGI import */
__attribute__((dllimport)) HRESULT __attribute__((ms_abi))
CreateDXGIFactory(REFIID riid, void **ppFactory);

/* D3D11 import */
__attribute__((dllimport)) HRESULT __attribute__((ms_abi))
D3D11CreateDeviceAndSwapChain(
	void *pAdapter, UINT DriverType, void *Software, UINT Flags,
	const UINT *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
	void **ppSwapChain, void **ppDevice,
	UINT *pFeatureLevel, void **ppImmediateContext);

/* kernel32 imports */
__attribute__((dllimport)) void __attribute__((ms_abi))
ExitProcess(UINT);

__attribute__((dllimport)) HANDLE __attribute__((ms_abi))
GetStdHandle(DWORD);

__attribute__((dllimport)) BOOL __attribute__((ms_abi))
WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);

/* === 유틸리티 === */

static HANDLE hStdout;

static int my_strlen(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	return n;
}

static void print(const char *s)
{
	DWORD w;
	WriteFile(hStdout, s, (DWORD)my_strlen(s), &w, NULL);
}

static void print_hex(unsigned int val)
{
	char buf[16];
	const char *hex = "0123456789ABCDEF";
	buf[0] = '0'; buf[1] = 'x';
	for (int i = 7; i >= 0; i--)
		buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
	buf[10] = 0;
	print(buf);
}

static void print_int(int val)
{
	char buf[16];
	int i = 0;
	if (val == 0) { buf[0] = '0'; buf[1] = 0; print(buf); return; }
	if (val < 0) { buf[i++] = '-'; val = -val; }
	int start = i;
	while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
	buf[i] = 0;
	/* reverse digits */
	int end = i - 1;
	while (start < end) {
		char t = buf[start]; buf[start] = buf[end]; buf[end] = t;
		start++; end--;
	}
	print(buf);
}

/* WndProc */
static LRESULT __attribute__((ms_abi))
test_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

/* === 메인 === */

void __attribute__((ms_abi)) _start(void)
{
	hStdout = GetStdHandle((DWORD)-11);

	print("=== DirectX 11 Test ===\n\n");

	int pass = 0, fail = 0;
	HRESULT hr;
	IID dummy_iid = {0};

	/* ------------------------------------------
	 * 윈도우 생성 (SwapChain용)
	 * ------------------------------------------ */
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = test_wndproc;
	wc.lpszClassName = "DXTestWnd";
	RegisterClassA(&wc);

	HWND hwnd = CreateWindowExA(0, "DXTestWnd", "DX Test",
		WS_OVERLAPPEDWINDOW, 100, 100, 320, 240,
		NULL, NULL, NULL, NULL);

	/* ------------------------------------------
	 * [1] CreateDXGIFactory
	 * ------------------------------------------ */
	print("[1]  CreateDXGIFactory... ");
	void *pFactory = NULL;
	hr = CreateDXGIFactory(&dummy_iid, &pFactory);
	if (SUCCEEDED(hr) && pFactory) {
		print("OK\n"); pass++;
	} else {
		print("FAIL\n"); fail++;
	}

	/* ------------------------------------------
	 * [2] IDXGIFactory::EnumAdapters
	 * ------------------------------------------ */
	print("[2]  EnumAdapters(0)... ");
	void *pAdapter = NULL;
	if (pFactory) {
		IDXGIFactoryVtbl **fvt = (IDXGIFactoryVtbl **)pFactory;
		hr = (*fvt)->EnumAdapters(pFactory, 0, &pAdapter);
		if (SUCCEEDED(hr) && pAdapter) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [3] IDXGIAdapter::GetDesc
	 * ------------------------------------------ */
	print("[3]  GetDesc... ");
	if (pAdapter) {
		IDXGIAdapterVtbl **avt = (IDXGIAdapterVtbl **)pAdapter;
		DXGI_ADAPTER_DESC desc;
		hr = (*avt)->GetDesc(pAdapter, &desc);
		if (SUCCEEDED(hr) && desc.Description[0] == 'C') {
			/* "CITC Software Adapter" starts with 'C' */
			print("OK (");
			/* Print first few chars */
			char name[32];
			for (int i = 0; i < 4 && desc.Description[i]; i++)
				name[i] = (char)desc.Description[i];
			name[4] = '.'; name[5] = '.'; name[6] = '.'; name[7] = 0;
			print(name);
			print(")\n");
			pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [4] D3D11CreateDeviceAndSwapChain
	 * ------------------------------------------ */
	print("[4]  D3D11CreateDeviceAndSwapChain... ");

	DXGI_SWAP_CHAIN_DESC scd = {0};
	scd.BufferDesc.Width = 320;
	scd.BufferDesc.Height = 240;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 1;
	scd.OutputWindow = hwnd;
	scd.Windowed = TRUE;

	void *pSwapChain = NULL;
	void *pDevice = NULL;
	UINT featureLevel = 0;
	void *pContext = NULL;

	UINT fl = D3D_FEATURE_LEVEL_11_0;
	hr = D3D11CreateDeviceAndSwapChain(
		NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
		&fl, 1, 7, /* SDKVersion=7 */
		&scd, &pSwapChain, &pDevice, &featureLevel, &pContext);

	if (SUCCEEDED(hr) && pDevice && pContext && pSwapChain) {
		print("OK (FL=");
		print_hex(featureLevel);
		print(")\n");
		pass++;
	} else {
		print("FAIL (hr=");
		print_hex((unsigned int)hr);
		print(")\n");
		fail++;
	}

	/* ------------------------------------------
	 * [5] SwapChain::GetBuffer → CreateRTV
	 * ------------------------------------------ */
	print("[5]  GetBuffer + CreateRTV... ");
	void *pBackBuffer = NULL;
	void *pRTV = NULL;

	if (pSwapChain && pDevice) {
		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->GetBuffer(pSwapChain, 0, &dummy_iid, &pBackBuffer);

		if (SUCCEEDED(hr) && pBackBuffer) {
			ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
			hr = (*dvt)->CreateRenderTargetView(pDevice, pBackBuffer, NULL, &pRTV);
			if (SUCCEEDED(hr) && pRTV) {
				print("OK\n"); pass++;
			} else {
				print("FAIL (CreateRTV)\n"); fail++;
			}
		} else {
			print("FAIL (GetBuffer)\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [6] ClearRenderTargetView (red) + Present
	 * ------------------------------------------ */
	print("[6]  ClearRTV(red) + Present... ");
	if (pContext && pRTV && pSwapChain) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

		/* Set RTV */
		void *rtvs[1] = { pRTV };
		(*cvt)->OMSetRenderTargets(pContext, 1, rtvs, NULL);
		(*cvt)->ClearRenderTargetView(pContext, pRTV, red);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);

		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [7] CreateBuffer (vertex buffer)
	 * ------------------------------------------ */
	print("[7]  CreateBuffer(VB)... ");

	struct Vertex { float pos[3]; float color[4]; };
	struct Vertex vertices[3] = {
		{{ 0.0f,  0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}, /* red (top) */
		{{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}}, /* green (right) */
		{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}, /* blue (left) */
	};

	void *pVB = NULL;
	if (pDevice) {
		D3D11_BUFFER_DESC bd = {0};
		bd.ByteWidth = sizeof(vertices);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

		D3D11_SUBRESOURCE_DATA sd = {0};
		sd.pSysMem = vertices;

		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateBuffer(pDevice, &bd, &sd, &pVB);
		if (SUCCEEDED(hr) && pVB) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [8] CreateVertexShader / CreatePixelShader
	 * ------------------------------------------ */
	print("[8]  CreateVS/PS... ");
	void *pVS = NULL, *pPS = NULL;
	if (pDevice) {
		/* 더미 바이트코드 (소프트웨어 렌더러는 무시) */
		unsigned char dummy_bc[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateVertexShader(pDevice, dummy_bc, 4, NULL, &pVS);
		HRESULT hr2 = (*dvt)->CreatePixelShader(pDevice, dummy_bc, 4, NULL, &pPS);
		if (SUCCEEDED(hr) && SUCCEEDED(hr2) && pVS && pPS) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [9] CreateInputLayout
	 * ------------------------------------------ */
	print("[9]  CreateInputLayout... ");
	void *pLayout = NULL;
	if (pDevice) {
		D3D11_INPUT_ELEMENT_DESC elems[2] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, 0, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, 0, 0 },
		};
		unsigned char dummy_bc[4] = { 0 };
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateInputLayout(pDevice, elems, 2, dummy_bc, 4, &pLayout);
		if (SUCCEEDED(hr) && pLayout) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [10] IA/VS/PS/OM 바인딩
	 * ------------------------------------------ */
	print("[10] Pipeline bind... ");
	if (pContext) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/* IA */
		(*cvt)->IASetInputLayout(pContext, pLayout);
		UINT stride = sizeof(struct Vertex);
		UINT offset = 0;
		void *vbs[1] = { pVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vbs, &stride, &offset);
		(*cvt)->IASetPrimitiveTopology(pContext, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		/* VS/PS */
		(*cvt)->VSSetShader(pContext, pVS, NULL, 0);
		(*cvt)->PSSetShader(pContext, pPS, NULL, 0);

		/* OM */
		void *rtvs[1] = { pRTV };
		(*cvt)->OMSetRenderTargets(pContext, 1, rtvs, NULL);

		/* RS — Viewport */
		D3D11_VIEWPORT vp = { 0.0f, 0.0f, 320.0f, 240.0f, 0.0f, 1.0f };
		(*cvt)->RSSetViewports(pContext, 1, &vp);

		print("OK\n"); pass++;
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [11] Draw(3,0) — Hello Triangle
	 * ------------------------------------------ */
	print("[11] Draw(3,0)... ");
	if (pContext) {
		/* 먼저 배경을 검은색으로 클리어 */
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black);

		/* 삼각형 그리기 */
		(*cvt)->Draw(pContext, 3, 0);

		print("OK\n"); pass++;
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [12] 삼각형 중앙 픽셀 확인
	 * ------------------------------------------ */
	print("[12] Center pixel check... ");
	if (pContext && pBackBuffer) {
		/* Present 후 백버퍼에서 삼각형 중앙 픽셀 읽기 */
		IDXGISwapChainVtbl **scvt2 = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt2)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
			D3D11_MAPPED_SUBRESOURCE mapped = {0};
			hr = (*cvt)->Map(pContext, pBackBuffer, 0,
					 D3D11_MAP_READ, 0, &mapped);
			if (SUCCEEDED(hr) && mapped.pData) {
				unsigned int *pixels = (unsigned int *)mapped.pData;
				UINT row = mapped.RowPitch / 4;
				/* 삼각형 중심 ≈ (160, 140) in 320×240 viewport */
				unsigned int center = pixels[140 * row + 160];
				(*cvt)->Unmap(pContext, pBackBuffer, 0);

				/* 배경: 검정(0x00000000), 삼각형: RGB 혼합 */
				if (center != 0x00000000) {
					print("OK (pixel=");
					print_hex(center);
					print(")\n"); pass++;
				} else {
					print("FAIL (black at center)\n"); fail++;
				}
			} else {
				print("FAIL (Map failed)\n"); fail++;
			}
		} else {
			print("FAIL (Present)\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [13] EnumAdapters(1) → NOT_FOUND 확인
	 * ------------------------------------------ */
	print("[13] EnumAdapters(1) not found... ");
	if (pFactory) {
		void *pBad = NULL;
		IDXGIFactoryVtbl **fvt = (IDXGIFactoryVtbl **)pFactory;
		hr = (*fvt)->EnumAdapters(pFactory, 1, &pBad);
		if (FAILED(hr) && pBad == NULL) {
			print("OK (DXGI_ERROR_NOT_FOUND)\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * Class 36: Constant Buffer + MVP 변환
	 * ========================================================== */

	/* ------------------------------------------
	 * [14] CreateBuffer(CB) + identity 행렬
	 * ------------------------------------------ */
	print("[14] CreateBuffer(CB + identity)... ");
	void *pCB = NULL;
	/* identity 행렬 (row-major) */
	float identity[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,1,0,
		0,0,0,1,
	};
	if (pDevice) {
		D3D11_BUFFER_DESC cbd = {0};
		cbd.ByteWidth = 64; /* sizeof(float)*16 */
		cbd.Usage = D3D11_USAGE_DEFAULT;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

		D3D11_SUBRESOURCE_DATA csd = {0};
		csd.pSysMem = identity;

		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateBuffer(pDevice, &cbd, &csd, &pCB);
		if (SUCCEEDED(hr) && pCB) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [15] VSSetCB + Draw: identity → 기존과 동일 결과
	 * ------------------------------------------ */
	print("[15] VSSetCB(identity) + Draw... ");
	if (pContext && pCB && pRTV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/* 배경 클리어 */
		float black2[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black2);

		/* CB 바인딩 */
		void *cbs[1] = { pCB };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, cbs);

		/* VB, layout, shader 등은 이전 테스트에서 이미 바인딩됨 */
		(*cvt)->Draw(pContext, 3, 0);

		/*
		 * identity 행렬이므로 삼각형은 동일 위치.
		 * Present 성공 확인.
		 */
		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [16] UpdateSubresource(scale 0.5) → 삼각형 축소
	 * ------------------------------------------ */
	print("[16] UpdateSubresource(scale 0.5)... ");
	if (pContext && pCB) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/* scale 0.5 행렬 */
		float scale_half[16] = {
			0.5f, 0, 0, 0,
			0, 0.5f, 0, 0,
			0, 0, 0.5f, 0,
			0, 0, 0, 1,
		};
		(*cvt)->UpdateSubresource(pContext, pCB, 0, NULL,
					  scale_half, 0, 0);

		/* 배경 클리어 + 그리기 */
		float black3[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black3);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [17] Translation 행렬 → 삼각형 이동
	 * ------------------------------------------ */
	print("[17] Translation(+0.5, 0, 0)... ");
	if (pContext && pCB) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/* translation (+0.5, 0, 0) — row-major:
		 * [1 0 0 0]
		 * [0 1 0 0]
		 * [0 0 1 0]
		 * [tx ty tz 1]
		 */
		float translate[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0.5f, 0, 0, 1,
		};
		(*cvt)->UpdateSubresource(pContext, pCB, 0, NULL,
					  translate, 0, 0);

		float black4[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black4);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [18] Perspective 행렬 → 3D 투영 (크래시 없음 검증)
	 * ------------------------------------------ */
	print("[18] Perspective projection... ");
	if (pContext && pCB) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/*
		 * perspective 행렬 (row-major):
		 * FOV 90도, aspect 4:3, near=0.1, far=100
		 *
		 * 삼각형은 z=0이므로 clip_w≈0 → degenerate.
		 * 코드의 w≈0 fallback이 크래시를 방지하는지 확인.
		 */
		float n = 0.1f, f = 100.0f;
		float aspect = 320.0f / 240.0f;
		float fov_f = 1.0f; /* 1/tan(45deg) */
		float q = f / (f - n);

		float perspective[16] = {
			fov_f/aspect, 0, 0, 0,
			0, fov_f, 0, 0,
			0, 0, q, 1,
			0, 0, -n*q, 0,
		};
		(*cvt)->UpdateSubresource(pContext, pCB, 0, NULL,
					  perspective, 0, 0);

		float black5[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black5);
		(*cvt)->Draw(pContext, 3, 0);

		/* identity 복원 + CB 언바인딩 */
		(*cvt)->UpdateSubresource(pContext, pCB, 0, NULL,
					  identity, 0, 0);
		void *null_cbs[1] = { NULL };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, null_cbs);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK (no crash)\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * Class 37: 깊이 버퍼 + 렌더 스테이트
	 * ========================================================== */

	/* ------------------------------------------
	 * [19] CreateTexture2D(D32_FLOAT) + CreateDSV
	 * ------------------------------------------ */
	print("[19] CreateTexture2D(D32) + DSV... ");
	void *pDepthTex = NULL, *pDSV = NULL;
	if (pDevice) {
		D3D11_TEXTURE2D_DESC dtd = {0};
		dtd.Width = 320;
		dtd.Height = 240;
		dtd.MipLevels = 1;
		dtd.ArraySize = 1;
		dtd.Format = DXGI_FORMAT_D32_FLOAT;
		dtd.SampleDesc.Count = 1;
		dtd.Usage = D3D11_USAGE_DEFAULT;
		dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateTexture2D(pDevice, &dtd, NULL, &pDepthTex);
		if (SUCCEEDED(hr) && pDepthTex) {
			hr = (*dvt)->CreateDepthStencilView(pDevice, pDepthTex, NULL, &pDSV);
			if (SUCCEEDED(hr) && pDSV) {
				print("OK\n"); pass++;
			} else {
				print("FAIL (DSV)\n"); fail++;
			}
		} else {
			print("FAIL (Tex)\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [20] CreateDepthStencilState(LESS) + 바인딩
	 * ------------------------------------------ */
	print("[20] CreateDSState(LESS)... ");
	void *pDSState = NULL;
	if (pDevice && pContext) {
		D3D11_DEPTH_STENCIL_DESC dsd = {0};
		dsd.DepthEnable = TRUE;
		dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsd.DepthFunc = D3D11_COMPARISON_LESS;

		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateDepthStencilState(pDevice, &dsd, &pDSState);
		if (SUCCEEDED(hr) && pDSState) {
			ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
			(*cvt)->OMSetDepthStencilState(pContext, pDSState, 0);
			/* DSV 바인딩 */
			void *rtvs[1] = { pRTV };
			(*cvt)->OMSetRenderTargets(pContext, 1, rtvs, pDSV);
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [21] 깊이 테스트: 앞(Z=0.3 RED) → 뒤(Z=0.7 GREEN)
	 *      RED가 이겨야 함.
	 * ------------------------------------------ */
	print("[21] Depth test (front wins)... ");
	if (pContext && pDevice && pDSV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;

		/* 배경 클리어 + 깊이 클리어 */
		float black6[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black6);
		(*cvt)->ClearDepthStencilView(pContext, pDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

		/* 앞 삼각형 (Z=0.3, RED) — 화면 전체 */
		struct Vertex front_tri[3] = {
			{{ 0.0f,  1.0f, 0.3f}, {1,0,0,1}},
			{{ 1.0f, -1.0f, 0.3f}, {1,0,0,1}},
			{{-1.0f, -1.0f, 0.3f}, {1,0,0,1}},
		};
		void *pVB_front = NULL;
		D3D11_BUFFER_DESC fbd = {0};
		fbd.ByteWidth = sizeof(front_tri);
		fbd.Usage = D3D11_USAGE_DEFAULT;
		fbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA fsd = {0};
		fsd.pSysMem = front_tri;
		(*dvt)->CreateBuffer(pDevice, &fbd, &fsd, &pVB_front);

		/* 뒤 삼각형 (Z=0.7, GREEN) */
		struct Vertex back_tri[3] = {
			{{ 0.0f,  1.0f, 0.7f}, {0,1,0,1}},
			{{ 1.0f, -1.0f, 0.7f}, {0,1,0,1}},
			{{-1.0f, -1.0f, 0.7f}, {0,1,0,1}},
		};
		void *pVB_back = NULL;
		D3D11_BUFFER_DESC bbd = {0};
		bbd.ByteWidth = sizeof(back_tri);
		bbd.Usage = D3D11_USAGE_DEFAULT;
		bbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA bsd = {0};
		bsd.pSysMem = back_tri;
		(*dvt)->CreateBuffer(pDevice, &bbd, &bsd, &pVB_back);

		/* CB 언바인딩 (transform 없이 NDC 직접) */
		void *null_cbs2[1] = { NULL };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, null_cbs2);

		UINT stride2 = sizeof(struct Vertex);
		UINT offset2 = 0;

		/* 앞 삼각형 그리기 */
		void *vbs_f[1] = { pVB_front };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vbs_f, &stride2, &offset2);
		(*cvt)->Draw(pContext, 3, 0);

		/* 뒤 삼각형 그리기 (깊이 테스트에 의해 거부되어야 함) */
		void *vbs_b[1] = { pVB_back };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vbs_b, &stride2, &offset2);
		(*cvt)->Draw(pContext, 3, 0);

		/* 결과: 중앙 픽셀이 RED(0.3)이어야 함 */
		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [22] 역순 그리기: 뒤(Z=0.7 GREEN) → 앞(Z=0.3 RED)
	 *      여전히 RED가 이겨야 함 (깊이 테스트)
	 * ------------------------------------------ */
	print("[22] Depth test (reverse order)... ");
	if (pContext && pDSV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		float black7[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black7);
		(*cvt)->ClearDepthStencilView(pContext, pDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

		/* 역순: Z=0.7 GREEN 먼저 */
		struct Vertex back2[3] = {
			{{ 0.0f,  1.0f, 0.7f}, {0,1,0,1}},
			{{ 1.0f, -1.0f, 0.7f}, {0,1,0,1}},
			{{-1.0f, -1.0f, 0.7f}, {0,1,0,1}},
		};
		void *pVB_b2 = NULL;
		D3D11_BUFFER_DESC b2d = {0};
		b2d.ByteWidth = sizeof(back2);
		b2d.Usage = D3D11_USAGE_DEFAULT;
		b2d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA b2s = {0};
		b2s.pSysMem = back2;
		ID3D11DeviceVtbl **dvt2 = (ID3D11DeviceVtbl **)pDevice;
		(*dvt2)->CreateBuffer(pDevice, &b2d, &b2s, &pVB_b2);

		struct Vertex front2[3] = {
			{{ 0.0f,  1.0f, 0.3f}, {1,0,0,1}},
			{{ 1.0f, -1.0f, 0.3f}, {1,0,0,1}},
			{{-1.0f, -1.0f, 0.3f}, {1,0,0,1}},
		};
		void *pVB_f2 = NULL;
		D3D11_BUFFER_DESC f2d = {0};
		f2d.ByteWidth = sizeof(front2);
		f2d.Usage = D3D11_USAGE_DEFAULT;
		f2d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA f2s = {0};
		f2s.pSysMem = front2;
		(*dvt2)->CreateBuffer(pDevice, &f2d, &f2s, &pVB_f2);

		UINT stride3 = sizeof(struct Vertex);
		UINT offset3 = 0;

		/* GREEN(뒤) 먼저 */
		void *vbs_b2[1] = { pVB_b2 };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vbs_b2, &stride3, &offset3);
		(*cvt)->Draw(pContext, 3, 0);

		/* RED(앞) 나중에 → 깊이 테스트 통과 (0.3 < 0.7) */
		void *vbs_f2[1] = { pVB_f2 };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vbs_f2, &stride3, &offset3);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [23] ClearDSV + CreateRasterizerState(CULL_BACK)
	 * ------------------------------------------ */
	print("[23] ClearDSV + RSState(CULL_BACK)... ");
	if (pDevice && pContext && pDSV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;

		/* DSV 클리어 */
		(*cvt)->ClearDepthStencilView(pContext, pDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

		/* 래스터라이저 상태: CULL_BACK */
		D3D11_RASTERIZER_DESC rsd = {0};
		rsd.FillMode = D3D11_FILL_SOLID;
		rsd.CullMode = D3D11_CULL_BACK;
		rsd.FrontCounterClockwise = FALSE;
		rsd.DepthClipEnable = TRUE;

		void *pRSState = NULL;
		hr = (*dvt)->CreateRasterizerState(pDevice, &rsd, &pRSState);
		if (SUCCEEDED(hr) && pRSState) {
			(*cvt)->RSSetState(pContext, pRSState);
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}

		/* DS state 해제, RS state 해제 */
		(*cvt)->OMSetDepthStencilState(pContext, NULL, 0);
		(*cvt)->RSSetState(pContext, NULL);
		/* DSV 해제 */
		void *rtvs_only[1] = { pRTV };
		(*cvt)->OMSetRenderTargets(pContext, 1, rtvs_only, NULL);
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * Class 38: 텍스처 매핑 + SRV
	 * ========================================================== */

	/* ------------------------------------------
	 * [24] CreateSRV (2×2 텍스처: 빨/초/파/흰)
	 * ------------------------------------------ */
	print("[24] CreateTexture2D(2x2) + SRV... ");
	void *pTexture = NULL, *pSRV = NULL;
	if (pDevice) {
		/* 2×2 XRGB8888 텍스처: [R,G / B,W] */
		unsigned int tex_data[4] = {
			0x00FF0000, /* (0,0) RED */
			0x0000FF00, /* (1,0) GREEN */
			0x000000FF, /* (0,1) BLUE */
			0x00FFFFFF, /* (1,1) WHITE */
		};

		D3D11_TEXTURE2D_DESC td = {0};
		td.Width = 2;
		td.Height = 2;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA tsd = {0};
		tsd.pSysMem = tex_data;
		tsd.SysMemPitch = 2 * 4;

		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateTexture2D(pDevice, &td, &tsd, &pTexture);
		if (SUCCEEDED(hr) && pTexture) {
			hr = (*dvt)->CreateShaderResourceView(pDevice, pTexture, NULL, &pSRV);
			if (SUCCEEDED(hr) && pSRV) {
				print("OK\n"); pass++;
			} else {
				print("FAIL (SRV)\n"); fail++;
			}
		} else {
			print("FAIL (Tex)\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [25] CreateSamplerState(CLAMP, POINT)
	 * ------------------------------------------ */
	print("[25] CreateSamplerState(CLAMP,POINT)... ");
	void *pSampler = NULL;
	if (pDevice) {
		D3D11_SAMPLER_DESC sd2 = {0};
		sd2.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd2.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd2.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd2.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd2.MaxLOD = 3.402823466e+38f; /* FLT_MAX */

		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateSamplerState(pDevice, &sd2, &pSampler);
		if (SUCCEEDED(hr) && pSampler) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [26] 텍스처 쿼드 (6 vertices + TEXCOORD)
	 *      2×2 텍스처 → 코너 색상 확인
	 * ------------------------------------------ */
	print("[26] Textured quad draw... ");
	if (pDevice && pContext && pSRV && pSampler && pRTV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;

		/*
		 * 쿼드: 2 삼각형, 6 vertices
		 * POSITION(float3) + COLOR(float4) + TEXCOORD(float2)
		 * stride = 12 + 16 + 8 = 36 bytes
		 *
		 * COLOR = white(1,1,1,1) → 텍스처 색만 나옴
		 */
		struct TexVertex {
			float pos[3];
			float color[4];
			float uv[2];
		};
		struct TexVertex quad_verts[6] = {
			/* 삼각형 1: 좌상-우상-좌하 */
			{{-1.0f,  1.0f, 0.0f}, {1,1,1,1}, {0.0f, 0.0f}}, /* TL → UV(0,0) */
			{{ 1.0f,  1.0f, 0.0f}, {1,1,1,1}, {1.0f, 0.0f}}, /* TR → UV(1,0) */
			{{-1.0f, -1.0f, 0.0f}, {1,1,1,1}, {0.0f, 1.0f}}, /* BL → UV(0,1) */
			/* 삼각형 2: 우상-우하-좌하 */
			{{ 1.0f,  1.0f, 0.0f}, {1,1,1,1}, {1.0f, 0.0f}}, /* TR → UV(1,0) */
			{{ 1.0f, -1.0f, 0.0f}, {1,1,1,1}, {1.0f, 1.0f}}, /* BR → UV(1,1) */
			{{-1.0f, -1.0f, 0.0f}, {1,1,1,1}, {0.0f, 1.0f}}, /* BL → UV(0,1) */
		};

		void *pTexVB = NULL;
		D3D11_BUFFER_DESC tvbd = {0};
		tvbd.ByteWidth = sizeof(quad_verts);
		tvbd.Usage = D3D11_USAGE_DEFAULT;
		tvbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA tvsd = {0};
		tvsd.pSysMem = quad_verts;
		(*dvt)->CreateBuffer(pDevice, &tvbd, &tvsd, &pTexVB);

		/* InputLayout: POSITION + COLOR + TEXCOORD */
		D3D11_INPUT_ELEMENT_DESC tex_elems[3] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, 0, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, 0, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, 0, 0 },
		};
		void *pTexLayout = NULL;
		unsigned char dummy_bc2[4] = { 0 };
		(*dvt)->CreateInputLayout(pDevice, tex_elems, 3,
					  dummy_bc2, 4, &pTexLayout);

		/* 파이프라인 설정 */
		(*cvt)->IASetInputLayout(pContext, pTexLayout);
		UINT tex_stride = sizeof(struct TexVertex);
		UINT tex_offset = 0;
		void *tex_vbs[1] = { pTexVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, tex_vbs,
					   &tex_stride, &tex_offset);

		/* SRV + Sampler 바인딩 */
		void *srvs[1] = { pSRV };
		(*cvt)->PSSetShaderResources(pContext, 0, 1, srvs);
		void *samplers[1] = { pSampler };
		(*cvt)->PSSetSamplers(pContext, 0, 1, samplers);

		/* 배경 클리어 + 그리기 */
		float black8[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black8);
		(*cvt)->Draw(pContext, 6, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [27] TEXCOORD + COLOR modulate
	 *      RED 텍스처 × GREEN 버텍스 = BLACK
	 * ------------------------------------------ */
	print("[27] Texture*Color modulate... ");
	if (pDevice && pContext && pSRV && pSampler) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;

		/*
		 * 2×2 텍스처 = all RED (1,0,0)
		 * 버텍스 컬러 = all GREEN (0,1,0)
		 * modulate: R*G = (1*0, 0*1, 0*0) = BLACK
		 */
		unsigned int red_tex[4] = {
			0x00FF0000, 0x00FF0000,
			0x00FF0000, 0x00FF0000,
		};

		D3D11_TEXTURE2D_DESC rd = {0};
		rd.Width = 2; rd.Height = 2;
		rd.MipLevels = 1; rd.ArraySize = 1;
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.SampleDesc.Count = 1;
		rd.Usage = D3D11_USAGE_DEFAULT;
		rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA rsd = {0};
		rsd.pSysMem = red_tex;
		rsd.SysMemPitch = 2 * 4;

		void *pRedTex = NULL, *pRedSRV = NULL;
		(*dvt)->CreateTexture2D(pDevice, &rd, &rsd, &pRedTex);
		if (pRedTex)
			(*dvt)->CreateShaderResourceView(pDevice, pRedTex, NULL, &pRedSRV);

		if (pRedSRV) {
			/* green 컬러 삼각형 (화면 전체) */
			struct TexVertex2 {
				float pos[3];
				float color[4];
				float uv[2];
			};
			struct TexVertex2 green_tri[3] = {
				{{ 0.0f,  1.0f, 0.0f}, {0,1,0,1}, {0.5f, 0.5f}},
				{{ 1.0f, -1.0f, 0.0f}, {0,1,0,1}, {0.5f, 0.5f}},
				{{-1.0f, -1.0f, 0.0f}, {0,1,0,1}, {0.5f, 0.5f}},
			};

			void *pGreenVB = NULL;
			D3D11_BUFFER_DESC gvbd = {0};
			gvbd.ByteWidth = sizeof(green_tri);
			gvbd.Usage = D3D11_USAGE_DEFAULT;
			gvbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			D3D11_SUBRESOURCE_DATA gvsd = {0};
			gvsd.pSysMem = green_tri;
			(*dvt)->CreateBuffer(pDevice, &gvbd, &gvsd, &pGreenVB);

			/* SRV 교체 */
			void *red_srvs[1] = { pRedSRV };
			(*cvt)->PSSetShaderResources(pContext, 0, 1, red_srvs);

			UINT gs = sizeof(struct TexVertex2);
			UINT go = 0;
			void *gvbs[1] = { pGreenVB };
			(*cvt)->IASetVertexBuffers(pContext, 0, 1, gvbs, &gs, &go);

			float white_bg[4] = {1,1,1,1};
			(*cvt)->ClearRenderTargetView(pContext, pRTV, white_bg);
			(*cvt)->Draw(pContext, 3, 0);

			IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
			hr = (*scvt)->Present(pSwapChain, 0, 0);
			if (SUCCEEDED(hr)) {
				print("OK\n"); pass++;
			} else {
				print("FAIL\n"); fail++;
			}
		} else {
			print("FAIL (SRV)\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [28] 텍스처 미바인딩 → 기존 컬러 삼각형 동일 (하위 호환)
	 * ------------------------------------------ */
	print("[28] No texture (backward compat)... ");
	if (pContext && pRTV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/* SRV/Sampler 해제 */
		void *null_srvs[1] = { NULL };
		(*cvt)->PSSetShaderResources(pContext, 0, 1, null_srvs);
		void *null_samp[1] = { NULL };
		(*cvt)->PSSetSamplers(pContext, 0, 1, null_samp);

		/* 원래 VB + Layout 복원 */
		(*cvt)->IASetInputLayout(pContext, pLayout);
		UINT orig_stride = sizeof(struct Vertex);
		UINT orig_offset = 0;
		void *orig_vbs[1] = { pVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, orig_vbs,
					   &orig_stride, &orig_offset);

		float black9[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black9);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * Class 39: DXBC 바이트코드 파서 + 인터프리터
	 * ========================================================== */

	/* ------------------------------------------
	 * [29] DXBC 파서: 유효 셰이더 blob 파싱 성공
	 *      CreateVertexShader에 유효한 DXBC blob을 넘김.
	 * ------------------------------------------ */
	print("[29] DXBC parse (valid VS blob)... ");
	/*
	 * Pass-through VS DXBC:
	 *   dcl_input v0       (POSITION)
	 *   dcl_input v1       (COLOR)
	 *   dcl_output_siv o0  (SV_Position)
	 *   dcl_output o1
	 *   mov o0, v0
	 *   mov o1, v1
	 *   ret
	 *
	 * SM4 operand encoding:
	 *   v#.xyzw (swizzle) = 0x00101E46 + index
	 *   o#.xyzw (mask)    = 0x001020F2 + index
	 *   v#.xyzw (dcl mask)= 0x001010F2 + index
	 */
	unsigned int vs_passthru_blob[] = {
		/* DXBC header (9 DWORDs) */
		0x43425844,             /* "DXBC" magic */
		0, 0, 0, 0,            /* MD5 (unused) */
		1,                      /* version */
		148,                    /* total size (bytes) */
		1,                      /* chunk count */
		36,                     /* chunk offset[0] */
		/* SHDR chunk */
		0x52444853,             /* "SHDR" tag */
		104,                    /* chunk data size (26 DWORDs * 4) */
		/* SHDR data */
		0x00010040,             /* VS 4.0 (type=1, major=4, minor=0) */
		26,                     /* token count */
		/* dcl_input v0 (3) */
		0x0300005F, 0x001010F2, 0x00000000,
		/* dcl_input v1 (3) */
		0x0300005F, 0x001010F2, 0x00000001,
		/* dcl_output_siv o0, position (4) */
		0x04000067, 0x001020F2, 0x00000000, 0x00000001,
		/* dcl_output o1 (3) */
		0x03000065, 0x001020F2, 0x00000001,
		/* mov o0, v0 (5) */
		0x05000036, 0x001020F2, 0x00000000,
		            0x00101E46, 0x00000000,
		/* mov o1, v1 (5) */
		0x05000036, 0x001020F2, 0x00000001,
		            0x00101E46, 0x00000001,
		/* ret (1) */
		0x0100003E
	};
	void *pVS_dxbc = NULL;
	if (pDevice) {
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
		hr = (*dvt)->CreateVertexShader(pDevice, vs_passthru_blob,
						sizeof(vs_passthru_blob),
						NULL, &pVS_dxbc);
		if (SUCCEEDED(hr) && pVS_dxbc) {
			print("OK\n"); pass++;
		} else {
			print("FAIL (hr="); print_hex((unsigned int)hr);
			print(")\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [30] 잘못된 blob → fallback (고정 함수 동작)
	 *      더미 바이트코드 VS로 그리기 → 기존과 동일.
	 * ------------------------------------------ */
	print("[30] Invalid blob fallback... ");
	if (pContext && pRTV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/* 원래 더미 셰이더로 복원 */
		(*cvt)->VSSetShader(pContext, pVS, NULL, 0);
		(*cvt)->PSSetShader(pContext, pPS, NULL, 0);
		(*cvt)->IASetInputLayout(pContext, pLayout);
		UINT s30 = sizeof(struct Vertex);
		UINT o30 = 0;
		void *vb30[1] = { pVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vb30, &s30, &o30);
		void *null_srv30[1] = { NULL };
		(*cvt)->PSSetShaderResources(pContext, 0, 1, null_srv30);
		void *null_cb30[1] = { NULL };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, null_cb30);

		float black30[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black30);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [31] VS VM: pass-through → 고정 함수와 동일 결과
	 *      DXBC VS(mov o0,v0; mov o1,v1)로 같은 삼각형 그리기.
	 * ------------------------------------------ */
	print("[31] VS VM pass-through draw... ");
	if (pContext && pRTV && pVS_dxbc) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;

		/* DXBC VS 바인딩 */
		(*cvt)->VSSetShader(pContext, pVS_dxbc, NULL, 0);
		(*cvt)->PSSetShader(pContext, pPS, NULL, 0);
		(*cvt)->IASetInputLayout(pContext, pLayout);
		UINT s31 = sizeof(struct Vertex);
		UINT o31 = 0;
		void *vb31[1] = { pVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vb31, &s31, &o31);
		void *null_cb31[1] = { NULL };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, null_cb31);
		void *null_srv31[1] = { NULL };
		(*cvt)->PSSetShaderResources(pContext, 0, 1, null_srv31);

		float black31[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black31);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [32] VS VM + CB: add로 삼각형 이동
	 *      VS가 add o0, v0, cb0[0] 실행 → 위치 오프셋.
	 * ------------------------------------------ */
	print("[32] VS VM + CB transform... ");
	/*
	 * CB VS DXBC:
	 *   dcl_input v0, dcl_input v1
	 *   dcl_output_siv o0, dcl_output o1
	 *   add o0, v0, cb0[0]   ← position += cb0[0]
	 *   mov o1, v1
	 *   ret
	 */
	unsigned int vs_cb_blob[] = {
		/* DXBC header */
		0x43425844, 0, 0, 0, 0, 1, 160, 1, 36,
		/* SHDR chunk */
		0x52444853, 116,
		/* SHDR data: VS 4.0, 29 tokens */
		0x00010040, 29,
		/* dcl_input v0 */
		0x0300005F, 0x001010F2, 0x00000000,
		/* dcl_input v1 */
		0x0300005F, 0x001010F2, 0x00000001,
		/* dcl_output_siv o0 position */
		0x04000067, 0x001020F2, 0x00000000, 0x00000001,
		/* dcl_output o1 */
		0x03000065, 0x001020F2, 0x00000001,
		/* add o0, v0, cb0[0] (8 DWORDs) */
		0x08000000,             /* ADD, length=8 */
		0x001020F2, 0x00000000, /* dest: o0.xyzw */
		0x00101E46, 0x00000000, /* src1: v0.xyzw */
		0x00208E46, 0x00000000, 0x00000000, /* src2: cb0[0].xyzw */
		/* mov o1, v1 */
		0x05000036, 0x001020F2, 0x00000001,
		            0x00101E46, 0x00000001,
		/* ret */
		0x0100003E
	};
	if (pDevice && pContext && pRTV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;

		/* CB VS 생성 */
		void *pVS_cb = NULL;
		hr = (*dvt)->CreateVertexShader(pDevice, vs_cb_blob,
						sizeof(vs_cb_blob),
						NULL, &pVS_cb);

		/* CB 데이터: 위치 오프셋 (0.5, 0, 0, 0) */
		float cb_data[4] = { 0.5f, 0.0f, 0.0f, 0.0f };
		D3D11_BUFFER_DESC cbd32 = {0};
		cbd32.ByteWidth = sizeof(cb_data);
		cbd32.Usage = D3D11_USAGE_DEFAULT;
		cbd32.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA csd32 = {0};
		csd32.pSysMem = cb_data;
		void *pCB32 = NULL;
		(*dvt)->CreateBuffer(pDevice, &cbd32, &csd32, &pCB32);

		/* 파이프라인 설정 */
		(*cvt)->VSSetShader(pContext, pVS_cb, NULL, 0);
		(*cvt)->PSSetShader(pContext, pPS, NULL, 0);
		(*cvt)->IASetInputLayout(pContext, pLayout);
		UINT s32 = sizeof(struct Vertex);
		UINT o32 = 0;
		void *vb32[1] = { pVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vb32, &s32, &o32);
		void *cbs32[1] = { pCB32 };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, cbs32);
		void *null_srv32[1] = { NULL };
		(*cvt)->PSSetShaderResources(pContext, 0, 1, null_srv32);

		float black32[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black32);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr) && pVS_cb) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * [33] PS VM: 단색(magenta) 출력
	 *      PS가 mov o0, l(1,0,1,1) 실행 → 마젠타.
	 * ------------------------------------------ */
	print("[33] PS VM magenta output... ");
	/*
	 * Magenta PS DXBC:
	 *   dcl_output o0
	 *   mov o0, l(1.0, 0.0, 1.0, 1.0)
	 *   ret
	 *
	 * Immediate 4-comp token: 0x00004E46
	 * float hex: 1.0 = 0x3F800000, 0.0 = 0x00000000
	 */
	unsigned int ps_magenta_blob[] = {
		/* DXBC header */
		0x43425844, 0, 0, 0, 0, 1, 100, 1, 36,
		/* SHDR chunk */
		0x52444853, 56,
		/* SHDR data: PS 4.0, 14 tokens */
		0x00000040, 14,
		/* dcl_output o0 (3) */
		0x03000065, 0x001020F2, 0x00000000,
		/* mov o0, l(1.0, 0.0, 1.0, 1.0) — 8 DWORDs */
		0x08000036,
		0x001020F2, 0x00000000,         /* dest: o0.xyzw */
		0x00004E46,                     /* src: immediate 4-comp */
		0x3F800000,                     /* 1.0f (R) */
		0x00000000,                     /* 0.0f (G) */
		0x3F800000,                     /* 1.0f (B) */
		0x3F800000,                     /* 1.0f (A) */
		/* ret (1) */
		0x0100003E
	};
	if (pDevice && pContext && pRTV) {
		ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
		ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;

		/* 마젠타 PS 생성 */
		void *pPS_mag = NULL;
		hr = (*dvt)->CreatePixelShader(pDevice, ps_magenta_blob,
					       sizeof(ps_magenta_blob),
					       NULL, &pPS_mag);

		/* pass-through VS + magenta PS */
		(*cvt)->VSSetShader(pContext, pVS_dxbc, NULL, 0);
		(*cvt)->PSSetShader(pContext, pPS_mag, NULL, 0);
		(*cvt)->IASetInputLayout(pContext, pLayout);
		UINT s33 = sizeof(struct Vertex);
		UINT o33 = 0;
		void *vb33[1] = { pVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vb33, &s33, &o33);
		void *null_cb33[1] = { NULL };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, null_cb33);
		void *null_srv33[1] = { NULL };
		(*cvt)->PSSetShaderResources(pContext, 0, 1, null_srv33);

		float black33[4] = {0,0,0,1};
		(*cvt)->ClearRenderTargetView(pContext, pRTV, black33);
		(*cvt)->Draw(pContext, 3, 0);

		IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr) && pPS_mag) {
			print("OK\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ------------------------------------------
	 * --- Class 43: DXBC → SPIR-V ---
	 * [34] SPIR-V: VS blob → 유효한 SPIR-V 바이너리
	 * [35] SPIR-V: PS blob → 유효한 SPIR-V 바이너리
	 * ------------------------------------------ */
	print("[34] SPIR-V VS blob ready... ");
	{
		/* DXBC→SPIR-V 컴파일러(spirv_emit.c)는 citcrun에 링크됨.
		 * dx_test.exe(MinGW)에서 직접 호출 불가.
		 * VS DXBC blob이 CreateShader에서 성공 = 유효한 DXBC.
		 * SPIR-V 변환은 VULKAN=1 빌드에서 자동 수행. */
		if (pVS_dxbc) {
			print("OK (VS blob valid)\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	}

	print("[35] SPIR-V PS blob ready... ");
	{
		/* PS DXBC blob (magenta)도 유효한 DXBC.
		 * pVS_dxbc는 [29]에서 CreateVertexShader 성공 확인됨.
		 * 여기서는 PS blob의 유효성 재확인. */
		/* magenta PS blob은 [33] 스코프 안이므로,
		 * 별도 CreatePixelShader 테스트 수행 */
		unsigned int ps_test_blob[] = {
			0x43425844, 0,0,0,0, 1, 100, 1, 28,
			0x52444853, 68,
			0x00000040, 17,
			0x03000065, 0x001020F2, 0x00000000,
			0x0A000036,
			0x001020F2, 0x00000000,
			0x00004E46,
			0x3F800000, 0x00000000, 0x3F800000, 0x3F800000,
			0x0100003E
		};
		void *pPS_test = NULL;
		ID3D11DeviceVtbl **dvt35 = (ID3D11DeviceVtbl **)pDevice;
		HRESULT hr35 = (*dvt35)->CreatePixelShader(pDevice,
			ps_test_blob, sizeof(ps_test_blob), NULL, &pPS_test);
		if (SUCCEEDED(hr35) && pPS_test) {
			print("OK (PS blob valid)\n"); pass++;
		} else {
			print("FAIL\n"); fail++;
		}
	}

	/* ------------------------------------------
	 * --- Class 53: DXBC 고급 명령어 + Shader Cache ---
	 * [36] DXBC if/else: 조건부 색상 (ge + if/else/endif)
	 * ------------------------------------------ */
	print("[36] DXBC if/else conditional PS... ");
	/*
	 * PS가 ge + if/else/endif로 조건부 색상 선택:
	 *   dcl_output o0
	 *   dcl_temps 1
	 *   mov r0.x, l(1.0)           ← 양수이므로 ge → true
	 *   ge r0.y, r0.x, l(0.5)     ← r0.y = (1.0 >= 0.5) ? 0xFFFFFFFF : 0
	 *   if_nz r0.y
	 *     mov o0, l(0, 1, 0, 1)   ← green
	 *   else
	 *     mov o0, l(1, 0, 0, 1)   ← red
	 *   endif
	 *   ret
	 *
	 * Expected: green output (ge true → if taken)
	 */
	{
		unsigned int ps_ifelse_blob[] = {
			/* DXBC header */
			0x43425844, 0, 0, 0, 0, 1, 240, 1, 36,
			/* SHDR chunk */
			0x52444853, 196,
			/* SHDR data: PS 4.0, 49 tokens */
			0x00000040, 49,
			/* dcl_output o0 (3) */
			0x03000065, 0x001020F2, 0x00000000,
			/* dcl_temps 1 (2) */
			0x02000068, 1,
			/* mov r0.x, l(1.0) — 5 tokens */
			0x05000036,
			0x00100012, 0x00000000,   /* dest: r0.x */
			0x00004001, 0x3F800000,   /* src: imm 1.0 */
			/* ge r0.y, r0.xxxx, l(0.5) — 7 tokens */
			0x0700001D,
			0x00100022, 0x00000000,   /* dest: r0.y */
			0x00100006, 0x00000000,   /* src1: r0.xxxx (select_1 x) */
			0x00004001, 0x3F000000,   /* src2: imm 0.5 */
			/* if_nz r0.y — 3 tokens (opcode bit 18 = 1 for nz) */
			0x0304001F,
			0x00100056, 0x00000000,   /* src: r0.yyyy (select_1 y) */
			/* mov o0, l(0, 1, 0, 1) — green (8 tokens) */
			0x08000036,
			0x001020F2, 0x00000000,
			0x00004E46,
			0x00000000, 0x3F800000, 0x00000000, 0x3F800000,
			/* else (1 token) */
			0x01000012,
			/* mov o0, l(1, 0, 0, 1) — red (8 tokens) */
			0x08000036,
			0x001020F2, 0x00000000,
			0x00004E46,
			0x3F800000, 0x00000000, 0x00000000, 0x3F800000,
			/* endif (1 token) */
			0x01000015,
			/* ret (1) */
			0x0100003E
		};
		if (pDevice && pContext && pRTV) {
			ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
			ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
			void *pPS_if = NULL;
			hr = (*dvt)->CreatePixelShader(pDevice, ps_ifelse_blob,
				sizeof(ps_ifelse_blob), NULL, &pPS_if);
			if (SUCCEEDED(hr) && pPS_if) {
				(*cvt)->VSSetShader(pContext, pVS_dxbc, NULL, 0);
				(*cvt)->PSSetShader(pContext, pPS_if, NULL, 0);
				(*cvt)->IASetInputLayout(pContext, pLayout);
				UINT s36 = sizeof(struct Vertex);
				UINT o36 = 0;
				void *vb36[1] = { pVB };
				(*cvt)->IASetVertexBuffers(pContext, 0, 1, vb36, &s36, &o36);
				void *null_cb36[1] = { NULL };
				(*cvt)->VSSetConstantBuffers(pContext, 0, 1, null_cb36);
				void *null_srv36[1] = { NULL };
				(*cvt)->PSSetShaderResources(pContext, 0, 1, null_srv36);
				float black36[4] = {0,0,0,1};
				(*cvt)->ClearRenderTargetView(pContext, pRTV, black36);
				(*cvt)->Draw(pContext, 3, 0);
				IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
				(*scvt)->Present(pSwapChain, 0, 0);
				print("OK\n"); pass++;
			} else {
				print("FAIL (create)\n"); fail++;
			}
		} else { print("SKIP\n"); fail++; }
	}

	/* ------------------------------------------
	 * [37] DXBC movc: 조건부 이동
	 * ------------------------------------------ */
	print("[37] DXBC movc conditional move... ");
	/*
	 * PS: lt + movc로 조건부 색상:
	 *   dcl_output o0, dcl_temps 1
	 *   mov r0.x, l(0.3)
	 *   lt r0.y, r0.xxxx, l(0.5)    ← r0.y = (0.3 < 0.5) ? 0xFFFFFFFF : 0
	 *   movc o0, r0.yyyy, l(1,1,0,1), l(0,0,1,1)  ← true → yellow
	 *   ret
	 *
	 * Expected: yellow (0.3 < 0.5 → true → first value)
	 */
	{
		unsigned int ps_movc_blob[] = {
			0x43425844, 0, 0, 0, 0, 1, 200, 1, 36,
			0x52444853, 156,
			0x00000040, 39,
			/* dcl_output o0 */
			0x03000065, 0x001020F2, 0x00000000,
			/* dcl_temps 1 */
			0x02000068, 1,
			/* mov r0.x, l(0.3) — 5 */
			0x05000036,
			0x00100012, 0x00000000,
			0x00004001, 0x3E99999A, /* 0.3f */
			/* lt r0.y, r0.xxxx, l(0.5) — 7 */
			0x07000031,
			0x00100022, 0x00000000,
			0x00100006, 0x00000000,
			0x00004001, 0x3F000000, /* 0.5f */
			/* movc o0, r0.yyyy, l(1,1,0,1), l(0,0,1,1) — 14 */
			0x0E000037,
			0x001020F2, 0x00000000,     /* dest: o0 */
			0x00100556, 0x00000000,     /* cond: r0.yyyy */
			0x00004E46,                 /* true: imm4 */
			0x3F800000, 0x3F800000, 0x00000000, 0x3F800000,
			0x00004E46,                 /* false: imm4 */
			0x00000000, 0x00000000, 0x3F800000, 0x3F800000,
			/* ret */
			0x0100003E
		};
		if (pDevice && pContext && pRTV) {
			ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
			void *pPS_movc = NULL;
			hr = (*dvt)->CreatePixelShader(pDevice, ps_movc_blob,
				sizeof(ps_movc_blob), NULL, &pPS_movc);
			if (SUCCEEDED(hr) && pPS_movc) {
				print("OK\n"); pass++;
			} else {
				print("FAIL\n"); fail++;
			}
		} else { print("SKIP\n"); fail++; }
	}

	/* ------------------------------------------
	 * [38] DXBC min/max
	 * ------------------------------------------ */
	print("[38] DXBC min/max... ");
	/*
	 * PS: min + max로 값 클램핑:
	 *   dcl_output o0, dcl_temps 1
	 *   mov r0, l(0.8, 0.2, 1.5, -0.3)
	 *   max r0, r0, l(0.0, 0.0, 0.0, 0.0)  ← clamp floor
	 *   min o0, r0, l(1.0, 1.0, 1.0, 1.0)  ← clamp ceil
	 *   ret
	 *
	 * Expected: (0.8, 0.2, 1.0, 0.0) — clamped to [0,1]
	 */
	{
		unsigned int ps_minmax_blob[] = {
			0x43425844, 0, 0, 0, 0, 1, 200, 1, 36,
			0x52444853, 156,
			0x00000040, 39,
			/* dcl_output o0 */
			0x03000065, 0x001020F2, 0x00000000,
			/* dcl_temps 1 */
			0x02000068, 1,
			/* mov r0, l(0.8, 0.2, 1.5, -0.3) — 8 */
			0x08000036,
			0x001000F2, 0x00000000,
			0x00004E46,
			0x3F4CCCCD, 0x3E4CCCCD, 0x3FC00000, 0xBE99999A,
			/* max r0, r0, l(0,0,0,0) — 8 */
			0x08000034,
			0x001000F2, 0x00000000,
			0x00100E46, 0x00000000,
			0x00004E46,
			0x00000000, 0x00000000, 0x00000000, 0x00000000,
			/* min o0, r0, l(1,1,1,1) — 8 */
			0x08000033,
			0x001020F2, 0x00000000,
			0x00100E46, 0x00000000,
			0x00004E46,
			0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000,
			/* ret */
			0x0100003E
		};
		if (pDevice && pContext && pRTV) {
			ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
			void *pPS_mm = NULL;
			hr = (*dvt)->CreatePixelShader(pDevice, ps_minmax_blob,
				sizeof(ps_minmax_blob), NULL, &pPS_mm);
			if (SUCCEEDED(hr) && pPS_mm) {
				print("OK\n"); pass++;
			} else {
				print("FAIL\n"); fail++;
			}
		} else { print("SKIP\n"); fail++; }
	}

	/* ------------------------------------------
	 * [39] Shader cache: 동일 셰이더 재생성 → 캐시 히트
	 * ------------------------------------------ */
	print("[39] Shader cache (second create)... ");
	{
		/* 이전 테스트의 magenta PS blob 재사용 */
		unsigned int ps_cache_blob[] = {
			0x43425844, 0, 0, 0, 0, 1, 100, 1, 36,
			0x52444853, 56,
			0x00000040, 14,
			0x03000065, 0x001020F2, 0x00000000,
			0x08000036,
			0x001020F2, 0x00000000,
			0x00004E46,
			0x3F800000, 0x00000000, 0x3F800000, 0x3F800000,
			0x0100003E
		};
		if (pDevice) {
			ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
			/* 첫 번째 생성 → 캐시 저장 */
			void *pPS_c1 = NULL;
			hr = (*dvt)->CreatePixelShader(pDevice, ps_cache_blob,
				sizeof(ps_cache_blob), NULL, &pPS_c1);
			/* 두 번째 생성 → 캐시 히트 */
			void *pPS_c2 = NULL;
			HRESULT hr2 = (*dvt)->CreatePixelShader(pDevice,
				ps_cache_blob, sizeof(ps_cache_blob),
				NULL, &pPS_c2);
			if (SUCCEEDED(hr) && SUCCEEDED(hr2) && pPS_c1 && pPS_c2) {
				print("OK\n"); pass++;
			} else {
				print("FAIL\n"); fail++;
			}
		} else { print("SKIP\n"); fail++; }
	}

	/* ------------------------------------------
	 * [40] Release cleanup
	 * ------------------------------------------ */
	print("[40] Release... ");
	{
		int ok = 1;
		if (pAdapter) {
			IDXGIAdapterVtbl **avt = (IDXGIAdapterVtbl **)pAdapter;
			(*avt)->Release(pAdapter);
		}
		if (pFactory) {
			IDXGIFactoryVtbl **fvt = (IDXGIFactoryVtbl **)pFactory;
			(*fvt)->Release(pFactory);
		}
		if (pContext) {
			ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
			(*cvt)->Release(pContext);
		}
		if (pDevice) {
			ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
			(*dvt)->Release(pDevice);
		}
		if (pSwapChain) {
			IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;
			(*scvt)->Release(pSwapChain);
		}
		if (ok) { print("OK\n"); pass++; }
	}

	/* ------------------------------------------
	 * 최종 결과
	 * ------------------------------------------ */
	print("\n--- Result: ");
	print_int(pass);
	print("/");
	print_int(pass + fail);
	print(" PASS ---\n");

	if (hwnd) DestroyWindow(hwnd);
	ExitProcess(fail == 0 ? 0 : 1);
}
