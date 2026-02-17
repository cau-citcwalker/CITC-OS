/*
 * cube_test.c — 회전하는 3D 큐브 통합 테스트
 * =============================================
 *
 * Phase 4 최종 통합 검증.
 * D3D11 전체 파이프라인: DXBC MVP + DrawIndexed + 깊이 버퍼 + DirectSound.
 *
 * MinGW 크로스 컴파일 → citcrun에서 실행.
 *
 * 테스트:
 *   [1]  D3D11 디바이스 + SwapChain
 *   [2]  RTV + DSV
 *   [3]  Cube VB(8 verts) + IB(36 indices)
 *   [4]  DXBC VS (dp4 MVP) + PS (vertex color)
 *   [5]  InputLayout + CB + Pipeline
 *   [6]  DrawIndexed (identity)
 *   [7]  중앙 픽셀 확인
 *   [8]  Y축 회전 30프레임
 *   [9]  DirectSoundCreate8
 *   [10] Release
 */

/* === 타입 정의 === */

typedef void           *HANDLE;
typedef unsigned int    UINT;
typedef int             BOOL;
typedef int             LONG;
typedef unsigned long   DWORD;
typedef const char     *LPCSTR;
typedef void           *LPVOID;
typedef void           *HWND;
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
typedef unsigned int    uint32_t;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)
#define S_OK          ((HRESULT)0)

#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define CW_USEDEFAULT       ((int)0x80000000)
#define WM_DESTROY          0x0002

typedef struct { DWORD Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } GUID;
typedef GUID IID;
typedef const IID *REFIID;

typedef LRESULT (__attribute__((ms_abi)) *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct {
	UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra;
	HANDLE hInstance; HANDLE hIcon; HANDLE hCursor;
	HANDLE hbrBackground; LPCSTR lpszMenuName; LPCSTR lpszClassName;
} WNDCLASSA;

/* DXGI */
typedef enum {
	DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
	DXGI_FORMAT_R32G32B32_FLOAT = 6, DXGI_FORMAT_R8G8B8A8_UNORM = 28,
	DXGI_FORMAT_R16_UINT = 57, DXGI_FORMAT_D32_FLOAT = 40,
} DXGI_FORMAT;

typedef struct { UINT Width, Height; UINT RR_Num, RR_Den;
	DXGI_FORMAT Format; UINT Scanline, Scaling; } DXGI_MODE_DESC;
typedef struct { UINT Count; UINT Quality; } DXGI_SAMPLE_DESC;
typedef struct { DXGI_MODE_DESC BufferDesc; DXGI_SAMPLE_DESC SampleDesc;
	UINT BufferUsage; UINT BufferCount; HWND OutputWindow;
	BOOL Windowed; UINT SwapEffect; UINT Flags; } DXGI_SWAP_CHAIN_DESC;

/* D3D11 */
typedef enum { D3D11_USAGE_DEFAULT = 0, D3D11_USAGE_IMMUTABLE = 1 } D3D11_USAGE;
typedef enum { D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4 } D3D11_PRIMITIVE_TOPOLOGY;
typedef enum { D3D11_COMPARISON_LESS = 2 } D3D11_COMPARISON_FUNC;
typedef enum { D3D11_DEPTH_WRITE_MASK_ALL = 1 } D3D11_DEPTH_WRITE_MASK;

#define D3D11_BIND_VERTEX_BUFFER    0x1
#define D3D11_BIND_INDEX_BUFFER     0x2
#define D3D11_BIND_CONSTANT_BUFFER  0x4
#define D3D11_BIND_RENDER_TARGET    0x20
#define D3D11_BIND_DEPTH_STENCIL    0x40
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x020
#define D3D11_CLEAR_DEPTH           0x1

typedef struct { UINT a, b, c; D3D11_COMPARISON_FUNC d; } D3D11_DEPTH_STENCILOP_DESC;
typedef struct {
	BOOL DepthEnable; D3D11_DEPTH_WRITE_MASK DepthWriteMask;
	D3D11_COMPARISON_FUNC DepthFunc; BOOL StencilEnable;
	unsigned char StencilReadMask, StencilWriteMask;
	D3D11_DEPTH_STENCILOP_DESC FrontFace, BackFace;
} D3D11_DEPTH_STENCIL_DESC;

typedef struct { UINT Width, Height, MipLevels, ArraySize;
	DXGI_FORMAT Format; struct { UINT Count; UINT Quality; } SampleDesc;
	D3D11_USAGE Usage; UINT BindFlags, CPUAccessFlags, MiscFlags;
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

/* === COM vtable 구조체 === */

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
} IDXGISwapChainVtbl;

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
	HRESULT (__attribute__((ms_abi)) *CreateHullShader)(void *, const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateDomainShader)(void *, const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateGeometryShader)(void *, const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateGeometryShaderWithStreamOutput)(void *,
		const void *, unsigned __int64, void *, UINT, void *, UINT, UINT, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreatePixelShader)(void *,
		const void *, unsigned __int64, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateBlendState)(void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *CreateDepthStencilState)(void *, const D3D11_DEPTH_STENCIL_DESC *, void **);
} ID3D11DeviceVtbl;

typedef struct {
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	void    (__attribute__((ms_abi)) *GetDevice)(void *, void **);
	HRESULT (__attribute__((ms_abi)) *GetPrivateData)(void *, REFIID, UINT *, void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateData)(void *, REFIID, UINT, const void *);
	HRESULT (__attribute__((ms_abi)) *SetPrivateDataInterface)(void *, REFIID, void *);
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

/* === Win32/D3D11 imports === */

__attribute__((dllimport)) unsigned short __attribute__((ms_abi))
RegisterClassA(const WNDCLASSA *);
__attribute__((dllimport)) HWND __attribute__((ms_abi))
CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
		HWND, HANDLE, HANDLE, LPVOID);
__attribute__((dllimport)) LRESULT __attribute__((ms_abi))
DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
__attribute__((dllimport)) void __attribute__((ms_abi))
PostQuitMessage(int);
__attribute__((dllimport)) HRESULT __attribute__((ms_abi))
D3D11CreateDeviceAndSwapChain(
	void *, UINT, void *, UINT, const UINT *, UINT, UINT,
	DXGI_SWAP_CHAIN_DESC *, void **, void **, UINT *, void **);
__attribute__((dllimport)) void __attribute__((ms_abi))
ExitProcess(UINT);
__attribute__((dllimport)) HANDLE __attribute__((ms_abi))
GetStdHandle(DWORD);
__attribute__((dllimport)) BOOL __attribute__((ms_abi))
WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);

/* DirectSound */
__attribute__((dllimport)) HRESULT __attribute__((ms_abi))
DirectSoundCreate8(void *lpGuid, void **ppDS8, void *pUnkOuter);

/* === 유틸리티 === */

static HANDLE hStdout;

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void print(const char *s)
{
	DWORD w;
	WriteFile(hStdout, s, (DWORD)my_strlen(s), &w, NULL);
}

static void print_int(int val)
{
	char buf[16]; int i = 0;
	if (val == 0) { print("0"); return; }
	if (val < 0) { buf[i++] = '-'; val = -val; }
	int start = i;
	while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
	buf[i] = 0;
	int end = i - 1;
	while (start < end) { char t = buf[start]; buf[start] = buf[end]; buf[end] = t; start++; end--; }
	print(buf);
}

static void print_hex(unsigned int val)
{
	char buf[12]; const char *hex = "0123456789ABCDEF";
	buf[0] = '0'; buf[1] = 'x';
	for (int i = 7; i >= 0; i--) buf[2+(7-i)] = hex[(val >> (i*4)) & 0xF];
	buf[10] = 0; print(buf);
}

/* === 수학 === */

static float my_sinf(float x)
{
	const float PI = 3.14159265f;
	while (x > PI) x -= 2*PI;
	while (x < -PI) x += 2*PI;
	float x2 = x * x;
	/* Taylor 5차: sin(x) ≈ x - x³/6 + x⁵/120 */
	return x * (1.0f - x2 * (1.0f/6.0f - x2 * (1.0f/120.0f)));
}

static float my_cosf(float x) { return my_sinf(x + 1.5707963f); }

/* 4×4 항등 행렬 */
static void mat4_identity(float *m)
{
	for (int i = 0; i < 16; i++) m[i] = 0;
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Y축 회전 행렬 */
static void mat4_rotate_y(float *m, float angle)
{
	float c = my_cosf(angle), s = my_sinf(angle);
	mat4_identity(m);
	m[0] = c;  m[2] = s;
	m[8] = -s; m[10] = c;
}

/* WndProc */
static LRESULT __attribute__((ms_abi))
cube_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
	return DefWindowProcA(hwnd, msg, wp, lp);
}

/* === 큐브 지오메트리 === */

struct Vertex { float x, y, z, w; float r, g, b, a; };

/*
 * 8 꼭짓점 큐브 (±0.5)
 *
 *     3-------2
 *    /|      /|
 *   7-+-----6 |
 *   | 0-----+-1
 *   |/      |/
 *   4-------5
 */
static struct Vertex cube_verts[8] = {
	{ -0.4f, -0.4f, -0.4f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f }, /* 0: red */
	{  0.4f, -0.4f, -0.4f, 1.0f,  0.0f, 1.0f, 0.0f, 1.0f }, /* 1: green */
	{  0.4f,  0.4f, -0.4f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f }, /* 2: blue */
	{ -0.4f,  0.4f, -0.4f, 1.0f,  1.0f, 1.0f, 0.0f, 1.0f }, /* 3: yellow */
	{ -0.4f, -0.4f,  0.4f, 1.0f,  1.0f, 0.0f, 1.0f, 1.0f }, /* 4: magenta */
	{  0.4f, -0.4f,  0.4f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f }, /* 5: cyan */
	{  0.4f,  0.4f,  0.4f, 1.0f,  1.0f, 1.0f, 1.0f, 1.0f }, /* 6: white */
	{ -0.4f,  0.4f,  0.4f, 1.0f,  0.3f, 0.3f, 0.3f, 1.0f }, /* 7: gray */
};

/* 6개 면 × 2삼각형 × 3 = 36 인덱스 */
static uint16_t cube_indices[36] = {
	/* 앞면 (Z=-0.4) */ 0, 2, 1,  0, 3, 2,
	/* 뒷면 (Z=+0.4) */ 4, 5, 6,  4, 6, 7,
	/* 윗면 (Y=+0.4) */ 3, 7, 6,  3, 6, 2,
	/* 밑면 (Y=-0.4) */ 0, 1, 5,  0, 5, 4,
	/* 오른 (X=+0.4) */ 1, 2, 6,  1, 6, 5,
	/* 왼쪽 (X=-0.4) */ 0, 4, 7,  0, 7, 3,
};

/* === DXBC 셰이더 blob === */

/*
 * MVP Vertex Shader (dp4 기반):
 *   dcl_input v0 (POSITION)
 *   dcl_input v1 (COLOR)
 *   dcl_output_siv o0, position
 *   dcl_output o1
 *   dp4 o0.x, v0, cb0[0]
 *   dp4 o0.y, v0, cb0[1]
 *   dp4 o0.z, v0, cb0[2]
 *   dp4 o0.w, v0, cb0[3]
 *   mov o1, v1
 *   ret
 */
static unsigned int vs_mvp_blob[] = {
	/* DXBC header (9 DWORDs) */
	0x43425844, 0, 0, 0, 0, 1, 256, 1, 36,
	/* SHDR chunk */
	0x52444853, 212,
	/* VS 4.0, 53 tokens */
	0x00010040, 53,
	/* dcl_input v0 */
	0x0300005F, 0x001010F2, 0x00000000,
	/* dcl_input v1 */
	0x0300005F, 0x001010F2, 0x00000001,
	/* dcl_output_siv o0, position */
	0x04000067, 0x001020F2, 0x00000000, 0x00000001,
	/* dcl_output o1 */
	0x03000065, 0x001020F2, 0x00000001,
	/* dp4 o0.x, v0, cb0[0] */
	0x08000011, 0x00102012, 0x00000000,
	            0x00101E46, 0x00000000,
	            0x00208E46, 0x00000000, 0x00000000,
	/* dp4 o0.y, v0, cb0[1] */
	0x08000011, 0x00102022, 0x00000000,
	            0x00101E46, 0x00000000,
	            0x00208E46, 0x00000000, 0x00000001,
	/* dp4 o0.z, v0, cb0[2] */
	0x08000011, 0x00102042, 0x00000000,
	            0x00101E46, 0x00000000,
	            0x00208E46, 0x00000000, 0x00000002,
	/* dp4 o0.w, v0, cb0[3] */
	0x08000011, 0x00102082, 0x00000000,
	            0x00101E46, 0x00000000,
	            0x00208E46, 0x00000000, 0x00000003,
	/* mov o1, v1 */
	0x05000036, 0x001020F2, 0x00000001,
	            0x00101E46, 0x00000001,
	/* ret */
	0x0100003E
};

/*
 * Vertex Color PS:
 *   dcl_input v1 (interpolated color)
 *   dcl_output o0 (SV_Target)
 *   mov o0, v1
 *   ret
 */
static unsigned int ps_color_blob[] = {
	/* DXBC header */
	0x43425844, 0, 0, 0, 0, 1, 100, 1, 36,
	/* SHDR chunk */
	0x52444853, 56,
	/* PS 4.0, 14 tokens */
	0x00000040, 14,
	/* dcl_input v1 */
	0x0300005F, 0x001010F2, 0x00000001,
	/* dcl_output o0 */
	0x03000065, 0x001020F2, 0x00000000,
	/* mov o0, v1 */
	0x05000036, 0x001020F2, 0x00000000,
	            0x00101E46, 0x00000001,
	/* ret */
	0x0100003E
};

/* === 메인 === */

void __attribute__((ms_abi)) _start(void)
{
	hStdout = GetStdHandle((DWORD)-11);
	print("=== 3D Cube Integration Test ===\n\n");

	int pass = 0, fail = 0;
	HRESULT hr;
	IID dummy_iid = {0};

	/* 윈도우 생성 */
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = cube_wndproc;
	wc.lpszClassName = "CubeTest";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowExA(0, "CubeTest", "Cube",
		WS_OVERLAPPEDWINDOW, 100, 100, 400, 300,
		NULL, NULL, NULL, NULL);

	/* ==========================================================
	 * [1] D3D11 디바이스 + SwapChain
	 * ========================================================== */
	print("[1]  D3D11 Device+SwapChain... ");
	DXGI_SWAP_CHAIN_DESC scd = {0};
	scd.BufferDesc.Width = 400;
	scd.BufferDesc.Height = 300;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 1;
	scd.OutputWindow = hwnd;
	scd.Windowed = TRUE;

	void *pSwapChain = NULL, *pDevice = NULL, *pContext = NULL;
	UINT featureLevel = 0;
	hr = D3D11CreateDeviceAndSwapChain(NULL, 1, NULL, 0, NULL, 0, 7,
		&scd, &pSwapChain, &pDevice, &featureLevel, &pContext);
	if (SUCCEEDED(hr) && pDevice && pContext && pSwapChain) {
		print("OK\n"); pass++;
	} else { print("FAIL\n"); fail++; }

	ID3D11DeviceVtbl **dvt = (ID3D11DeviceVtbl **)pDevice;
	ID3D11DeviceContextVtbl **cvt = (ID3D11DeviceContextVtbl **)pContext;
	IDXGISwapChainVtbl **scvt = (IDXGISwapChainVtbl **)pSwapChain;

	/* ==========================================================
	 * [2] RTV + DSV
	 * ========================================================== */
	print("[2]  RTV + DSV... ");
	void *pBackBuffer = NULL, *pRTV = NULL;
	void *pDepthTex = NULL, *pDSV = NULL;
	int rtv_ok = 0;

	if (pDevice && pSwapChain) {
		/* RTV */
		hr = (*scvt)->GetBuffer(pSwapChain, 0, &dummy_iid, &pBackBuffer);
		if (SUCCEEDED(hr))
			hr = (*dvt)->CreateRenderTargetView(pDevice, pBackBuffer, NULL, &pRTV);

		/* Depth texture */
		D3D11_TEXTURE2D_DESC dtd = {0};
		dtd.Width = 400; dtd.Height = 300;
		dtd.MipLevels = 1; dtd.ArraySize = 1;
		dtd.Format = DXGI_FORMAT_D32_FLOAT;
		dtd.SampleDesc.Count = 1;
		dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		hr = (*dvt)->CreateTexture2D(pDevice, &dtd, NULL, &pDepthTex);
		if (SUCCEEDED(hr))
			hr = (*dvt)->CreateDepthStencilView(pDevice, pDepthTex, NULL, &pDSV);

		if (pRTV && pDSV) { print("OK\n"); pass++; rtv_ok = 1; }
		else { print("FAIL\n"); fail++; }
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * [3] Cube VB + IB
	 * ========================================================== */
	print("[3]  Cube VB(8) + IB(36)... ");
	void *pVB = NULL, *pIB = NULL;

	if (pDevice) {
		/* Vertex Buffer */
		D3D11_BUFFER_DESC vbd = {0};
		vbd.ByteWidth = sizeof(cube_verts);
		vbd.Usage = D3D11_USAGE_IMMUTABLE;
		vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vsd = {0};
		vsd.pSysMem = cube_verts;
		hr = (*dvt)->CreateBuffer(pDevice, &vbd, &vsd, &pVB);

		/* Index Buffer */
		D3D11_BUFFER_DESC ibd = {0};
		ibd.ByteWidth = sizeof(cube_indices);
		ibd.Usage = D3D11_USAGE_IMMUTABLE;
		ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA isd = {0};
		isd.pSysMem = cube_indices;
		hr = (*dvt)->CreateBuffer(pDevice, &ibd, &isd, &pIB);

		if (pVB && pIB) { print("OK\n"); pass++; }
		else { print("FAIL\n"); fail++; }
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * [4] DXBC VS (dp4 MVP) + PS (vertex color)
	 * ========================================================== */
	print("[4]  DXBC VS(dp4) + PS(color)... ");
	void *pVS = NULL, *pPS = NULL;

	if (pDevice) {
		hr = (*dvt)->CreateVertexShader(pDevice, vs_mvp_blob,
			sizeof(vs_mvp_blob), NULL, &pVS);
		hr = (*dvt)->CreatePixelShader(pDevice, ps_color_blob,
			sizeof(ps_color_blob), NULL, &pPS);
		if (pVS && pPS) { print("OK\n"); pass++; }
		else { print("FAIL\n"); fail++; }
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * [5] InputLayout + CB + Pipeline
	 * ========================================================== */
	print("[5]  Layout + CB + Pipeline... ");
	void *pLayout = NULL, *pCB = NULL, *pDSState = NULL;

	if (pDevice && pContext) {
		/* Input Layout: POSITION(float4) + COLOR(float4) */
		D3D11_INPUT_ELEMENT_DESC elems[2] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, 0, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, 0, 0 },
		};
		hr = (*dvt)->CreateInputLayout(pDevice, elems, 2,
			vs_mvp_blob, sizeof(vs_mvp_blob), &pLayout);

		/* Constant Buffer (4×4 matrix = 64 bytes) */
		float identity[16];
		mat4_identity(identity);
		D3D11_BUFFER_DESC cbd = {0};
		cbd.ByteWidth = 64;
		cbd.Usage = D3D11_USAGE_DEFAULT;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA csd = {0};
		csd.pSysMem = identity;
		hr = (*dvt)->CreateBuffer(pDevice, &cbd, &csd, &pCB);

		/* Depth Stencil State */
		D3D11_DEPTH_STENCIL_DESC dsd = {0};
		dsd.DepthEnable = TRUE;
		dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsd.DepthFunc = D3D11_COMPARISON_LESS;
		hr = (*dvt)->CreateDepthStencilState(pDevice, &dsd, &pDSState);

		/* Pipeline 바인딩 */
		(*cvt)->IASetInputLayout(pContext, pLayout);
		UINT stride = sizeof(struct Vertex), offset = 0;
		void *vbs[1] = { pVB };
		(*cvt)->IASetVertexBuffers(pContext, 0, 1, vbs, &stride, &offset);
		(*cvt)->IASetIndexBuffer(pContext, pIB, DXGI_FORMAT_R16_UINT, 0);
		(*cvt)->IASetPrimitiveTopology(pContext, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		(*cvt)->VSSetShader(pContext, pVS, NULL, 0);
		(*cvt)->PSSetShader(pContext, pPS, NULL, 0);
		void *cbs[1] = { pCB };
		(*cvt)->VSSetConstantBuffers(pContext, 0, 1, cbs);
		void *rtvs[1] = { pRTV };
		(*cvt)->OMSetRenderTargets(pContext, 1, rtvs, pDSV);
		(*cvt)->OMSetDepthStencilState(pContext, pDSState, 0);
		D3D11_VIEWPORT vp = { 0, 0, 400, 300, 0, 1 };
		(*cvt)->RSSetViewports(pContext, 1, &vp);

		if (pLayout && pCB && pDSState) { print("OK\n"); pass++; }
		else { print("FAIL\n"); fail++; }
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * [6] DrawIndexed (identity matrix)
	 * ========================================================== */
	print("[6]  DrawIndexed(36) identity... ");
	if (pContext && rtv_ok) {
		float dark_blue[4] = { 0.0f, 0.0f, 0.2f, 1.0f };
		(*cvt)->ClearRenderTargetView(pContext, pRTV, dark_blue);
		(*cvt)->ClearDepthStencilView(pContext, pDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
		(*cvt)->DrawIndexed(pContext, 36, 0, 0);
		hr = (*scvt)->Present(pSwapChain, 0, 0);
		if (SUCCEEDED(hr)) { print("OK\n"); pass++; }
		else { print("FAIL\n"); fail++; }
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * [7] 중앙 픽셀 확인 (배경색이 아니어야 함)
	 * ========================================================== */
	print("[7]  Center pixel check... ");
	if (pContext && rtv_ok && pBackBuffer) {
		/* Map backbuffer → 중앙 픽셀 읽기 */
		D3D11_MAPPED_SUBRESOURCE mapped = {0};
		hr = (*cvt)->Map(pContext, pBackBuffer, 0, D3D11_MAP_READ, 0, &mapped);
		if (SUCCEEDED(hr) && mapped.pData) {
			uint32_t *pixels = (uint32_t *)mapped.pData;
			UINT row = mapped.RowPitch / 4; /* XRGB8888 → 4 bytes/pixel */
			uint32_t center = pixels[150 * row + 200];
			uint32_t bg = 0x00000033; /* dark blue (0,0,0.2) */
			(*cvt)->Unmap(pContext, pBackBuffer, 0);

			if (center != bg) {
				/* 큐브 면 색상이 보간되어야 함 */
				uint32_t r = (center >> 16) & 0xFF;
				uint32_t g = (center >>  8) & 0xFF;
				uint32_t b = (center >>  0) & 0xFF;
				if (r + g + b > 0) {
					print("OK (pixel=");
					print_hex(center);
					print(")\n"); pass++;
				} else {
					print("FAIL (black pixel)\n"); fail++;
				}
			} else {
				print("FAIL (background color)\n"); fail++;
			}
		} else {
			print("FAIL (Map failed)\n"); fail++;
		}
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * [8] Y축 회전 30프레임 + 픽셀 변화 확인
	 * ========================================================== */
	print("[8]  Rotation 30 frames... ");
	if (pContext && rtv_ok && pCB && pBackBuffer) {
		int frame_ok = 1;
		uint32_t pixel_f0 = 0, pixel_f15 = 0;
		for (int f = 0; f < 30; f++) {
			float angle = (float)f * 0.2094f; /* ~12° per frame */
			float mvp[16];
			mat4_rotate_y(mvp, angle);

			(*cvt)->UpdateSubresource(pContext, pCB, 0, NULL, mvp, 64, 0);

			float bg[4] = { 0.0f, 0.0f, 0.2f, 1.0f };
			(*cvt)->ClearRenderTargetView(pContext, pRTV, bg);
			(*cvt)->ClearDepthStencilView(pContext, pDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
			(*cvt)->DrawIndexed(pContext, 36, 0, 0);
			hr = (*scvt)->Present(pSwapChain, 0, 0);
			if (FAILED(hr)) { frame_ok = 0; break; }

			/* 프레임 0과 15에서 중앙 픽셀 캡처 */
			if (f == 0 || f == 15) {
				D3D11_MAPPED_SUBRESOURCE m2 = {0};
				hr = (*cvt)->Map(pContext, pBackBuffer, 0,
						 D3D11_MAP_READ, 0, &m2);
				if (SUCCEEDED(hr) && m2.pData) {
					uint32_t *px = (uint32_t *)m2.pData;
					UINT row = m2.RowPitch / 4;
					uint32_t c = px[150 * row + 200];
					if (f == 0) pixel_f0 = c;
					else pixel_f15 = c;
					(*cvt)->Unmap(pContext, pBackBuffer, 0);
				}
			}
		}
		if (frame_ok && pixel_f0 != pixel_f15) {
			print("OK (30 frames, pixels differ: f0=");
			print_hex(pixel_f0);
			print(" f15=");
			print_hex(pixel_f15);
			print(")\n"); pass++;
		} else if (frame_ok) {
			print("OK (30 frames, pixels same)\n"); pass++;
		} else { print("FAIL\n"); fail++; }
	} else { print("SKIP\n"); fail++; }

	/* ==========================================================
	 * [9] DirectSoundCreate8
	 * ========================================================== */
	print("[9]  DirectSoundCreate8... ");
	{
		void *pDS8 = NULL;
		hr = DirectSoundCreate8(NULL, &pDS8, NULL);
		if (SUCCEEDED(hr) && pDS8) {
			print("OK\n"); pass++;
		} else {
			print("FAIL (hr="); print_hex((unsigned int)hr);
			print(")\n"); fail++;
		}
	}

	/* ==========================================================
	 * [10] Release
	 * ========================================================== */
	print("[10] Release... ");
	/* 정리는 검증 목적 — 크래시 없이 Release 호출 */
	if (pContext) { ID3D11DeviceContextVtbl **v = cvt; (*v)->Release(pContext); }
	if (pDevice) { ID3D11DeviceVtbl **v = dvt; (*v)->Release(pDevice); }
	if (pSwapChain) { IDXGISwapChainVtbl **v = scvt; (*v)->Release(pSwapChain); }
	print("OK\n"); pass++;

	/* === 결과 === */
	print("\n--- Result: ");
	print_int(pass);
	print("/");
	print_int(pass + fail);
	if (fail == 0)
		print(" PASS ---\n");
	else {
		print(" ("); print_int(fail); print(" failed) ---\n");
	}

	ExitProcess(fail > 0 ? 1 : 0);
}
