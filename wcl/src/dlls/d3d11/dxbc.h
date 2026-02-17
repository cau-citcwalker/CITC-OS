/*
 * dxbc.h — DXBC 바이트코드 파서 + SM4 인터프리터
 * ================================================
 *
 * DirectX 셰이더 컴파일러(fxc.exe)가 생성하는 DXBC 컨테이너를
 * 파싱하고 SM4 명령어를 CPU에서 해석 실행.
 *
 * DXBC 컨테이너:
 *   "DXBC" 매직 → MD5 → 버전 → 청크 목록
 *   ├─ ISGN: 입력 시맨틱 (POSITION, COLOR, TEXCOORD)
 *   ├─ OSGN: 출력 시맨틱 (SV_Position, COLOR)
 *   └─ SHDR: SM4 바이트코드 (실제 명령어)
 */

#ifndef CITC_DXBC_H
#define CITC_DXBC_H

#include <stdint.h>
#include <stddef.h>

#define DXBC_MAX_INPUTS  8
#define DXBC_MAX_OUTPUTS 8
#define DXBC_MAX_TEMPS   32

/* SM4 opcodes */
#define SM4_OP_ADD              0
#define SM4_OP_BREAK            2
#define SM4_OP_BREAKC           3
#define SM4_OP_DP3             16
#define SM4_OP_DP4             17
#define SM4_OP_ELSE            18
#define SM4_OP_ENDIF           21
#define SM4_OP_ENDLOOP         22
#define SM4_OP_EQ              24
#define SM4_OP_GE              29
#define SM4_OP_IF              31
#define SM4_OP_LOOP            48
#define SM4_OP_LT              49
#define SM4_OP_MAD             50
#define SM4_OP_MIN             51
#define SM4_OP_MAX             52
#define SM4_OP_MOV             54
#define SM4_OP_MOVC            55
#define SM4_OP_MUL             56
#define SM4_OP_NE              57
#define SM4_OP_RET             62
#define SM4_OP_RSQ             68
#define SM4_OP_SAMPLE          69
#define SM4_OP_SAMPLE_L        72
#define SM4_OP_DCL_RESOURCE    88

/* SM4 operand types */
#define SM4_OPERAND_TEMP     0
#define SM4_OPERAND_INPUT    1
#define SM4_OPERAND_OUTPUT   2
#define SM4_OPERAND_IMM32    4
#define SM4_OPERAND_SAMPLER  6
#define SM4_OPERAND_RESOURCE 7
#define SM4_OPERAND_CB       8

/* 시그니처 엘리먼트 */
struct dxbc_sig_element {
	char name[32];
	int semantic_idx;
	int register_num;
	int system_value;   /* 0=none, 1=SV_Position */
	int mask;           /* xyzw bitmask */
};

/* 파싱된 DXBC 정보 */
struct dxbc_info {
	int valid;
	int shader_type;    /* 0=pixel, 1=vertex */
	int version_major, version_minor;

	struct dxbc_sig_element inputs[DXBC_MAX_INPUTS];
	int num_inputs;
	struct dxbc_sig_element outputs[DXBC_MAX_OUTPUTS];
	int num_outputs;

	const uint32_t *shader_tokens;  /* SHDR 데이터 시작 포인터 */
	int shader_token_count;         /* SHDR 데이터 DWORD 수 */
	int num_temps;
};

/* 셰이더 VM 상태 */
struct shader_vm {
	float temps[DXBC_MAX_TEMPS][4];
	float inputs[DXBC_MAX_INPUTS][4];
	float outputs[DXBC_MAX_OUTPUTS][4];

	/* Constant buffers */
	const float *cb[4];
	int cb_size[4];     /* 바이트 단위 */
};

/* DXBC 컨테이너 파싱 */
int dxbc_parse(const void *bytecode, size_t size, struct dxbc_info *info);

/* 셰이더 VM 실행 — 성공 시 0 반환 */
int shader_vm_execute(struct shader_vm *vm, const struct dxbc_info *info);

#endif /* CITC_DXBC_H */
