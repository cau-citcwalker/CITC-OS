/*
 * spirv_emit.h — DXBC → SPIR-V 컴파일러
 * ========================================
 *
 * SM4 바이트코드를 SPIR-V 바이너리로 변환.
 * dxbc.c의 shader_vm_execute()와 같은 토큰 순회 로직으로
 * SPIR-V 명령어를 방출.
 *
 * 지원 SM4 명령어: mov, add, mul, mad, dp3, dp4, ret,
 *                  lt, ge, eq, ne, min, max, movc, rsq
 * 미지원 명령어는 CPU VM fallback.
 */

#ifndef CITC_SPIRV_EMIT_H
#define CITC_SPIRV_EMIT_H

#include <stdint.h>
#include <stddef.h>
#include "dxbc.h"

/* SPIR-V 매직/상수 */
#define SPIRV_MAGIC     0x07230203
#define SPIRV_VERSION   0x00010000

/* SPIR-V opcodes (필요한 것만) */
#define SpvOpCapability          17
#define SpvOpMemoryModel         14
#define SpvOpEntryPoint          15
#define SpvOpExecutionMode       16
#define SpvOpDecorate            71
#define SpvOpTypeVoid            19
#define SpvOpTypeFloat           22
#define SpvOpTypeVector          23
#define SpvOpTypePointer         32
#define SpvOpTypeFunction        33
#define SpvOpVariable            59
#define SpvOpConstant            43
#define SpvOpFunction            54
#define SpvOpFunctionEnd         56
#define SpvOpLabel              248
#define SpvOpReturn             253
#define SpvOpLoad                61
#define SpvOpStore               62
#define SpvOpFAdd               129
#define SpvOpFMul               133
#define SpvOpDot                148
#define SpvOpCompositeConstruct   80
#define SpvOpCompositeExtract     81
#define SpvOpVectorShuffle        79
#define SpvOpSelect              169
#define SpvOpFOrdEqual           180
#define SpvOpFUnordNotEqual      182
#define SpvOpFOrdLessThan        184
#define SpvOpFOrdGreaterThanEqual 190
#define SpvOpExtInstImport        11
#define SpvOpExtInst              12
#define SpvOpTypeBool             20

/* GLSL.std.450 extended instructions */
#define GLSL_STD_450_InverseSqrt  32
#define GLSL_STD_450_FMin         37
#define GLSL_STD_450_FMax         40

/* SPIR-V Decoration */
#define SpvDecorationLocation   30
#define SpvDecorationBuiltIn    11

/* SPIR-V BuiltIn */
#define SpvBuiltInPosition      0
#define SpvBuiltInFragCoord     15

/* SPIR-V Storage class */
#define SpvStorageClassInput     1
#define SpvStorageClassOutput    3
#define SpvStorageClassFunction  7
#define SpvStorageClassUniformConstant 0
#define SpvStorageClassUniform   2

/* SPIR-V Capability */
#define SpvCapabilityShader      1

/* SPIR-V Execution model */
#define SpvExecutionModelVertex    0
#define SpvExecutionModelFragment  4

/* SPIR-V Addressing/Memory model */
#define SpvAddressingModelLogical     0
#define SpvMemoryModelGLSL450         1

/* SPIR-V Execution mode */
#define SpvExecutionModeOriginUpperLeft 7

/*
 * DXBC → SPIR-V 변환.
 *
 * info:     dxbc_parse()로 파싱된 셰이더 정보
 * out_spirv: malloc'd SPIR-V 바이너리 (호출자가 free)
 * out_size:  바이트 단위 크기
 *
 * 반환: 0 성공, -1 실패
 */
int dxbc_to_spirv(const struct dxbc_info *info,
                  uint32_t **out_spirv, size_t *out_size);

#endif /* CITC_SPIRV_EMIT_H */
