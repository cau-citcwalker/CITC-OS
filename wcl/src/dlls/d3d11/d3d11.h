/*
 * d3d11.h — CITC OS d3d11.dll
 * ============================
 *
 * Direct3D 11 구현 (소프트웨어 래스터라이저).
 *
 * D3D11의 핵심 역할:
 *   - 3D 렌더링 파이프라인 (IA → VS → RS → PS → OM)
 *   - GPU 리소스 관리 (버퍼, 텍스처, 셰이더)
 *   - 드로우콜 (Draw, DrawIndexed)
 *
 * Phase 4에서 구현하는 함수:
 *   D3D11CreateDevice, D3D11CreateDeviceAndSwapChain
 *   ID3D11Device: CreateBuffer, CreateTexture2D, CreateRenderTargetView,
 *                 CreateVertexShader, CreatePixelShader, CreateInputLayout
 *   ID3D11DeviceContext: Draw, ClearRenderTargetView, IASet*, VSSet*, PSSet*,
 *                        OMSetRenderTargets, RSSetViewports, Map, Unmap
 */

#ifndef CITC_D3D11_H
#define CITC_D3D11_H

#include "../../../include/win32.h"
#include "../../../include/d3d11_types.h"
#include "../../../include/stub_entry.h"

/* 스텁 테이블 (citcrun이 참조) */
extern struct stub_entry d3d11_stub_table[];

/*
 * 내부 API — dxgi.c의 GetBuffer 연동
 *
 * SwapChain의 백버퍼를 D3D11 텍스처 리소스로 등록.
 * CreateRenderTargetView에서 이 리소스를 RTV로 생성.
 */
int d3d11_register_swapchain_texture(void *pSwapChain,
				     uint32_t *pixels,
				     int width, int height);

/*
 * Vulkan 렌더 타깃 생성 (SwapChain 생성 시 호출)
 * Vulkan 비활성이면 아무것도 하지 않음.
 */
void d3d11_vk_create_rt(int width, int height);

/*
 * Vulkan readback. GPU 픽셀을 CPU 버퍼에 복사.
 * 반환: 1이면 readback 수행함, 0이면 SW 모드 (readback 불필요)
 */
int d3d11_vk_readback(uint32_t *pixels, int width, int height);

#endif /* CITC_D3D11_H */
