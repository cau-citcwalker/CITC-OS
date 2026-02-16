/*
 * dxgi.h — CITC OS dxgi.dll
 * ==========================
 *
 * DXGI (DirectX Graphics Infrastructure) 구현.
 *
 * DXGI의 역할:
 *   - GPU 열거 (EnumAdapters)
 *   - 스왑 체인 관리 (화면 출력)
 *   - 디스플레이 모드 제어
 *
 * Phase 4에서 구현하는 함수:
 *   CreateDXGIFactory
 *   IDXGIFactory::EnumAdapters, CreateSwapChain
 *   IDXGIAdapter::GetDesc
 *   IDXGISwapChain::Present, GetBuffer, GetDesc, ResizeBuffers
 */

#ifndef CITC_DXGI_H
#define CITC_DXGI_H

#include "../../../include/win32.h"
#include "../../../include/d3d11_types.h"
#include "../../../include/stub_entry.h"

/* 스텁 테이블 (citcrun이 참조) */
extern struct stub_entry dxgi_stub_table[];

/*
 * DXGI 내부 API — d3d11.c에서 사용
 *
 * SwapChain의 백버퍼를 D3D11 리소스로 등록할 때 호출.
 */

/* SwapChain의 백버퍼 픽셀 포인터/크기 획득 */
int dxgi_get_swapchain_backbuffer(void *pSwapChain, uint32_t **pixels,
				  int *width, int *height);

/* SwapChain의 백버퍼 인덱스 설정 (d3d11 리소스와 연동) */
void dxgi_set_swapchain_resource(void *pSwapChain, int resource_idx);

/*
 * 내부 API — D3D11CreateDeviceAndSwapChain에서 사용
 *
 * ms_abi 함수에서 직접 호출 가능 (ABI 불일치 방지).
 * Factory 생성 → SwapChain 생성을 한 번에 수행.
 */
HRESULT dxgi_create_swapchain_for_d3d11(void *pDevice,
					DXGI_SWAP_CHAIN_DESC *pDesc,
					void **ppSwapChain);

#endif /* CITC_DXGI_H */
