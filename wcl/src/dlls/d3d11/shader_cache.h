/*
 * shader_cache.h — DXBC → SPIR-V 디스크 캐시
 * ==============================================
 *
 * DXBC blob의 FNV-1a 해시로 SPIR-V 바이너리를 캐시.
 * 경로: ~/.citc/shader_cache/<hex_hash>.spv
 */

#ifndef CITC_SHADER_CACHE_H
#define CITC_SHADER_CACHE_H

#include <stdint.h>
#include <stddef.h>

/*
 * 캐시 조회.
 * dxbc/dxbc_size: 원본 DXBC blob.
 * out_spirv:      히트 시 malloc'd SPIR-V (호출자 free).
 * out_size:       바이트 크기.
 * 반환: 0=히트, -1=미스.
 */
int shader_cache_lookup(const uint8_t *dxbc, size_t dxbc_size,
			uint32_t **out_spirv, size_t *out_size);

/*
 * 캐시 저장.
 * dxbc/dxbc_size: 원본 DXBC blob (해시 키).
 * spirv/spirv_size: SPIR-V 바이너리.
 */
void shader_cache_store(const uint8_t *dxbc, size_t dxbc_size,
			const uint32_t *spirv, size_t spirv_size);

#endif /* CITC_SHADER_CACHE_H */
