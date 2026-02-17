/*
 * spirv_emit.c — DXBC → SPIR-V 컴파일러
 * ========================================
 *
 * SM4 바이트코드를 SPIR-V 바이너리로 직접 변환.
 * 외부 라이브러리 없이 uint32_t[] 배열로 SPIR-V를 구축.
 *
 * SM4 → SPIR-V 매핑:
 *   mov dst, src     → OpLoad + OpStore (with swizzle via OpVectorShuffle)
 *   add dst, a, b    → OpFAdd
 *   mul dst, a, b    → OpFMul
 *   mad dst, a, b, c → OpFMul + OpFAdd
 *   dp3 dst, a, b    → OpDot (vec3 extract)
 *   dp4 dst, a, b    → OpDot
 *   ret              → OpReturn
 *   CB 접근          → OpAccessChain + OpLoad (Uniform) [향후]
 *
 * 모듈 구조:
 *   Header → Capability → MemoryModel → EntryPoint → Decorations
 *   → Types → Variables → Function → Instructions → Return → End
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "spirv_emit.h"

/* ============================================================
 * SPIR-V 바이너리 빌더
 * ============================================================ */

#define SPIRV_MAX_WORDS 4096

struct spirv_builder {
	uint32_t words[SPIRV_MAX_WORDS];
	int count;
	int next_id;

	/* 타입 ID */
	int id_void;
	int id_bool;
	int id_float;
	int id_vec3;
	int id_vec4;
	int id_bvec4;
	int id_ptr_in_vec3;
	int id_ptr_in_vec4;
	int id_ptr_out_vec4;
	int id_ptr_func_vec4;
	int id_func_void;

	/* 상수 ID */
	int id_float_0;
	int id_float_1;

	/* GLSL.std.450 */
	int id_glsl_ext;

	/* 변수 ID */
	int id_inputs[DXBC_MAX_INPUTS];    /* OpVariable Input */
	int id_outputs[DXBC_MAX_OUTPUTS];  /* OpVariable Output */
	int id_temps[DXBC_MAX_TEMPS];      /* OpVariable Function */

	/* 셰이더 정보 */
	int is_vertex;                      /* 1=VS, 0=PS */
	int num_inputs;
	int num_outputs;
	int num_temps;

	/* EntryPoint에 나열할 인터페이스 변수 ID */
	int iface_ids[DXBC_MAX_INPUTS + DXBC_MAX_OUTPUTS];
	int num_iface;
};

static void emit(struct spirv_builder *b, uint32_t word)
{
	if (b->count < SPIRV_MAX_WORDS)
		b->words[b->count++] = word;
}

static int new_id(struct spirv_builder *b)
{
	return b->next_id++;
}

/* SPIR-V 명령어 워드 = (word_count << 16) | opcode */
static void emit_op(struct spirv_builder *b, int opcode, int word_count)
{
	emit(b, (uint32_t)((word_count << 16) | opcode));
}

/* ============================================================
 * Phase 1: 헤더 + 선언부 방출
 * ============================================================ */

/* 5-word SPIR-V header (bound는 나중에 패치) */
static void emit_header(struct spirv_builder *b)
{
	emit(b, SPIRV_MAGIC);
	emit(b, SPIRV_VERSION);
	emit(b, 0); /* generator */
	emit(b, 0); /* bound — 나중에 패치 */
	emit(b, 0); /* schema */
}

static void emit_capability(struct spirv_builder *b)
{
	emit_op(b, SpvOpCapability, 2);
	emit(b, SpvCapabilityShader);
}

static void emit_memory_model(struct spirv_builder *b)
{
	emit_op(b, SpvOpMemoryModel, 3);
	emit(b, SpvAddressingModelLogical);
	emit(b, SpvMemoryModelGLSL450);
}

/* "main" → 4바이트 정렬된 리터럴 문자열 */
static void emit_string_main(struct spirv_builder *b)
{
	/* "main\0" padded to 8 bytes = 2 words */
	emit(b, 0x6E69616D); /* "main" */
	emit(b, 0x00000000); /* \0 padded */
}

static void emit_types(struct spirv_builder *b)
{
	/* %void = OpTypeVoid */
	b->id_void = new_id(b);
	emit_op(b, SpvOpTypeVoid, 2);
	emit(b, (uint32_t)b->id_void);

	/* %bool = OpTypeBool */
	b->id_bool = new_id(b);
	emit_op(b, SpvOpTypeBool, 2);
	emit(b, (uint32_t)b->id_bool);

	/* %func_void = OpTypeFunction %void */
	b->id_func_void = new_id(b);
	emit_op(b, SpvOpTypeFunction, 3);
	emit(b, (uint32_t)b->id_func_void);
	emit(b, (uint32_t)b->id_void);

	/* %float = OpTypeFloat 32 */
	b->id_float = new_id(b);
	emit_op(b, SpvOpTypeFloat, 3);
	emit(b, (uint32_t)b->id_float);
	emit(b, 32);

	/* %vec3 = OpTypeVector %float 3 */
	b->id_vec3 = new_id(b);
	emit_op(b, SpvOpTypeVector, 4);
	emit(b, (uint32_t)b->id_vec3);
	emit(b, (uint32_t)b->id_float);
	emit(b, 3);

	/* %vec4 = OpTypeVector %float 4 */
	b->id_vec4 = new_id(b);
	emit_op(b, SpvOpTypeVector, 4);
	emit(b, (uint32_t)b->id_vec4);
	emit(b, (uint32_t)b->id_float);
	emit(b, 4);

	/* %bvec4 = OpTypeVector %bool 4 */
	b->id_bvec4 = new_id(b);
	emit_op(b, SpvOpTypeVector, 4);
	emit(b, (uint32_t)b->id_bvec4);
	emit(b, (uint32_t)b->id_bool);
	emit(b, 4);

	/* Pointer types */
	b->id_ptr_in_vec4 = new_id(b);
	emit_op(b, SpvOpTypePointer, 4);
	emit(b, (uint32_t)b->id_ptr_in_vec4);
	emit(b, SpvStorageClassInput);
	emit(b, (uint32_t)b->id_vec4);

	b->id_ptr_in_vec3 = new_id(b);
	emit_op(b, SpvOpTypePointer, 4);
	emit(b, (uint32_t)b->id_ptr_in_vec3);
	emit(b, SpvStorageClassInput);
	emit(b, (uint32_t)b->id_vec3);

	b->id_ptr_out_vec4 = new_id(b);
	emit_op(b, SpvOpTypePointer, 4);
	emit(b, (uint32_t)b->id_ptr_out_vec4);
	emit(b, SpvStorageClassOutput);
	emit(b, (uint32_t)b->id_vec4);

	b->id_ptr_func_vec4 = new_id(b);
	emit_op(b, SpvOpTypePointer, 4);
	emit(b, (uint32_t)b->id_ptr_func_vec4);
	emit(b, SpvStorageClassFunction);
	emit(b, (uint32_t)b->id_vec4);

	/* Constants */
	b->id_float_0 = new_id(b);
	emit_op(b, SpvOpConstant, 4);
	emit(b, (uint32_t)b->id_float);
	emit(b, (uint32_t)b->id_float_0);
	emit(b, 0x00000000); /* 0.0f */

	b->id_float_1 = new_id(b);
	emit_op(b, SpvOpConstant, 4);
	emit(b, (uint32_t)b->id_float);
	emit(b, (uint32_t)b->id_float_1);
	emit(b, 0x3F800000); /* 1.0f */
}

/* ============================================================
 * Phase 2: 변수 선언
 * ============================================================ */

static void emit_variables(struct spirv_builder *b,
                           const struct dxbc_info *info)
{
	b->num_iface = 0;

	/* Input 변수 */
	for (int i = 0; i < info->num_inputs; i++) {
		b->id_inputs[i] = new_id(b);

		/* VS: input 0은 보통 vec3(POSITION) */
		int ptr_type = b->id_ptr_in_vec4;
		if (b->is_vertex && i == 0 && info->inputs[i].mask == 0x07)
			ptr_type = b->id_ptr_in_vec3;

		emit_op(b, SpvOpVariable, 4);
		emit(b, (uint32_t)ptr_type);
		emit(b, (uint32_t)b->id_inputs[i]);
		emit(b, SpvStorageClassInput);

		b->iface_ids[b->num_iface++] = b->id_inputs[i];
	}

	/* Output 변수 */
	for (int i = 0; i < info->num_outputs; i++) {
		b->id_outputs[i] = new_id(b);

		emit_op(b, SpvOpVariable, 4);
		emit(b, (uint32_t)b->id_ptr_out_vec4);
		emit(b, (uint32_t)b->id_outputs[i]);
		emit(b, SpvStorageClassOutput);

		b->iface_ids[b->num_iface++] = b->id_outputs[i];
	}
}

/* ============================================================
 * Phase 3: Decorations (Location, BuiltIn)
 * ============================================================
 *
 * EntryPoint와 Decorations는 header 바로 뒤에 와야 하므로
 * 별도 버퍼에 생성 후 메인 빌더 앞에 삽입.
 * → 간단화: EntryPoint/Decorations를 먼저 방출하고 Types/Variables를
 *   그 뒤에 방출. SPIR-V 스펙은 섹션 순서를 강제.
 *
 * 구조:
 *   Header (5w) → Capability (2w) → MemoryModel (3w)
 *   → EntryPoint → ExecutionMode (PS만)
 *   → Decorations
 *   → Types → Constants
 *   → Variables (global)
 *   → Function → Label → [temps] → Instructions → Return → FunctionEnd
 */

/* ============================================================
 * 메인 컴파일러
 * ============================================================ */

/* SM4 오퍼랜드를 SPIR-V 변수로 매핑: Load하여 vec4 ID 반환 */
struct sm4_operand_info {
	int type;       /* SM4_OPERAND_* */
	int reg_idx;    /* register index */
	int mask;       /* write mask (dst) */
	int swizzle[4]; /* read swizzle (src) */
	float imm[4];   /* immediate values */
	int has_imm;
	int cb_idx;     /* CB index (for CB type) */
	int cb_elem;    /* CB element index */
};

/* SM4 오퍼랜드 토큰 디코딩 (dxbc.c의 read_operand와 유사) */
static int decode_sm4_operand(const uint32_t **pp, const uint32_t *end,
                              struct sm4_operand_info *op, int is_dest)
{
	if (*pp >= end) return -1;
	uint32_t token = **pp; (*pp)++;

	memset(op, 0, sizeof(*op));

	int num_comp = (int)(token & 3);
	int sel_mode = (int)((token >> 2) & 3);
	op->type  = (int)((token >> 12) & 0xFF);
	int idx_dim = (int)((token >> 20) & 3);

	/* mask / swizzle */
	if (is_dest) {
		op->mask = (int)((token >> 4) & 0xF);
	} else {
		if (num_comp == 2 && sel_mode == 1) { /* swizzle */
			op->swizzle[0] = (int)((token >> 4) & 3);
			op->swizzle[1] = (int)((token >> 6) & 3);
			op->swizzle[2] = (int)((token >> 8) & 3);
			op->swizzle[3] = (int)((token >> 10) & 3);
		} else {
			op->swizzle[0] = 0; op->swizzle[1] = 1;
			op->swizzle[2] = 2; op->swizzle[3] = 3;
		}
	}

	/* 확장 오퍼랜드 스킵 */
	if (token & 0x80000000) {
		if (*pp >= end) return -1;
		(*pp)++;
	}

	/* 인덱스 */
	int idx[3] = {0, 0, 0};
	for (int d = 0; d < idx_dim; d++) {
		if (*pp >= end) return -1;
		idx[d] = (int)**pp; (*pp)++;
	}

	op->reg_idx = idx[0];

	if (op->type == SM4_OPERAND_CB) {
		op->cb_idx = idx[0];
		op->cb_elem = idx[1];
	}

	/* Immediate */
	if (op->type == SM4_OPERAND_IMM32) {
		op->has_imm = 1;
		if (num_comp == 2) { /* 4 components */
			if (*pp + 4 > end) return -1;
			memcpy(op->imm, *pp, 16);
			*pp += 4;
		} else if (num_comp == 1) { /* 1 component */
			if (*pp >= end) return -1;
			memcpy(&op->imm[0], *pp, 4);
			op->imm[1] = op->imm[2] = op->imm[3] = op->imm[0];
			*pp += 1;
		}
	}

	return 0;
}

/* ============================================================
 * 2-pass 컴파일러
 * ============================================================
 *
 * Pass 1: SM4 토큰을 순회하여 필요한 리소스 파악
 * Pass 2: SPIR-V 명령어 방출
 *
 * 간단화를 위해 single-pass로 구현:
 *   - 헤더/타입/변수를 먼저 방출 (dxbc_info에서 추론)
 *   - SM4 명령어를 순서대로 SPIR-V로 변환
 *   - bound를 마지막에 패치
 */

int dxbc_to_spirv(const struct dxbc_info *info,
                  uint32_t **out_spirv, size_t *out_size)
{
	if (!info || !info->valid || !out_spirv || !out_size)
		return -1;

	struct spirv_builder *b = calloc(1, sizeof(*b));
	if (!b) return -1;

	b->next_id = 1;
	b->is_vertex = (info->shader_type == 1);
	b->num_inputs = info->num_inputs;
	b->num_outputs = info->num_outputs;
	b->num_temps = info->num_temps;

	/*
	 * SPIR-V 구조: header → capability → memory_model
	 *             → entry_point → execution_mode
	 *             → decorations → types → variables
	 *             → function → instructions → return → function_end
	 *
	 * 문제: entry_point에 interface 변수 ID가 필요한데,
	 * 변수 ID는 types 방출 후에야 알 수 있다.
	 *
	 * 해결: 두 개의 빌더를 사용 — decl_builder (header~decorations),
	 *       body_builder (types~function_end), 마지막에 합침.
	 */

	/* --- Phase 1: Types, Constants, Variables 방출 (ID 확정) --- */
	struct spirv_builder *body = calloc(1, sizeof(*body));
	if (!body) { free(b); return -1; }
	body->next_id = b->next_id;
	*body = *b; /* copy initial state */
	body->count = 0;

	emit_types(body);

	/* 변수 선언 전에 main 함수 ID 예약 */
	int id_main = new_id(body);

	emit_variables(body, info);

	/* Temp 변수 (Function scope — Function body 안에서 선언) */
	for (int i = 0; i < info->num_temps; i++)
		body->id_temps[i] = new_id(body);

	/* --- Phase 2: Header + EntryPoint + Decorations 방출 --- */
	b->next_id = body->next_id;
	memcpy(b->id_inputs, body->id_inputs, sizeof(body->id_inputs));
	memcpy(b->id_outputs, body->id_outputs, sizeof(body->id_outputs));
	memcpy(b->id_temps, body->id_temps, sizeof(body->id_temps));
	memcpy(b->iface_ids, body->iface_ids, sizeof(body->iface_ids));
	b->num_iface = body->num_iface;
	b->id_void = body->id_void;
	b->id_bool = body->id_bool;
	b->id_float = body->id_float;
	b->id_vec3 = body->id_vec3;
	b->id_vec4 = body->id_vec4;
	b->id_bvec4 = body->id_bvec4;
	b->id_ptr_in_vec3 = body->id_ptr_in_vec3;
	b->id_ptr_in_vec4 = body->id_ptr_in_vec4;
	b->id_ptr_out_vec4 = body->id_ptr_out_vec4;
	b->id_ptr_func_vec4 = body->id_ptr_func_vec4;
	b->id_func_void = body->id_func_void;
	b->id_float_0 = body->id_float_0;
	b->id_float_1 = body->id_float_1;

	emit_header(b);
	emit_capability(b);

	/* OpExtInstImport "GLSL.std.450" */
	b->id_glsl_ext = new_id(b);
	emit_op(b, SpvOpExtInstImport, 6);
	emit(b, (uint32_t)b->id_glsl_ext);
	/* "GLSL.std.450\0" = 13 chars → 4 words */
	emit(b, 0x4C534C47); /* "GLSL" */
	emit(b, 0x6474732E); /* ".std" */
	emit(b, 0x3035342E); /* ".450" */
	emit(b, 0x00000000); /* "\0" padded */

	emit_memory_model(b);

	/* OpEntryPoint */
	int ep_model = b->is_vertex ?
		SpvExecutionModelVertex : SpvExecutionModelFragment;
	int ep_word_count = 3 + 2 + b->num_iface; /* 3 fixed + "main"(2) + interfaces */
	emit_op(b, SpvOpEntryPoint, ep_word_count);
	emit(b, (uint32_t)ep_model);
	emit(b, (uint32_t)id_main);
	emit_string_main(b);
	for (int i = 0; i < b->num_iface; i++)
		emit(b, (uint32_t)b->iface_ids[i]);

	/* OpExecutionMode (Fragment only) */
	if (!b->is_vertex) {
		emit_op(b, SpvOpExecutionMode, 3);
		emit(b, (uint32_t)id_main);
		emit(b, SpvExecutionModeOriginUpperLeft);
	}

	/* Decorations */
	for (int i = 0; i < info->num_inputs; i++) {
		emit_op(b, SpvOpDecorate, 4);
		emit(b, (uint32_t)b->id_inputs[i]);
		emit(b, SpvDecorationLocation);
		emit(b, (uint32_t)i);
	}

	for (int i = 0; i < info->num_outputs; i++) {
		if (b->is_vertex && info->outputs[i].system_value == 1) {
			/* SV_Position → BuiltIn Position */
			emit_op(b, SpvOpDecorate, 4);
			emit(b, (uint32_t)b->id_outputs[i]);
			emit(b, SpvDecorationBuiltIn);
			emit(b, SpvBuiltInPosition);
		} else {
			/* 일반 출력 → Location */
			int loc = info->outputs[i].register_num;
			/* VS에서 SV_Position이 있으면 다른 output의 location을 조정 */
			emit_op(b, SpvOpDecorate, 4);
			emit(b, (uint32_t)b->id_outputs[i]);
			emit(b, SpvDecorationLocation);
			emit(b, (uint32_t)loc);
		}
	}

	/* --- Types, Constants, Variables (body에서 복사) --- */
	for (int i = 0; i < body->count; i++)
		emit(b, body->words[i]);

	/* --- Function body --- */
	/* OpFunction %void None %func_void */
	emit_op(b, SpvOpFunction, 5);
	emit(b, (uint32_t)b->id_void);
	emit(b, (uint32_t)id_main);
	emit(b, 0); /* None */
	emit(b, (uint32_t)b->id_func_void);

	/* OpLabel */
	int id_label = new_id(b);
	emit_op(b, SpvOpLabel, 2);
	emit(b, (uint32_t)id_label);

	/* Temp 변수 선언 (Function scope) */
	for (int i = 0; i < info->num_temps; i++) {
		emit_op(b, SpvOpVariable, 4);
		emit(b, (uint32_t)b->id_ptr_func_vec4);
		emit(b, (uint32_t)b->id_temps[i]);
		emit(b, SpvStorageClassFunction);
	}

	/* --- SM4 명령어 변환 --- */
	const uint32_t *tok = info->shader_tokens + 2; /* version + length 스킵 */
	const uint32_t *end = info->shader_tokens + info->shader_token_count;

	while (tok < end) {
		uint32_t opcode_token = *tok;
		int op = (int)(opcode_token & 0x7FF);
		int len = (int)((opcode_token >> 24) & 0x7F);
		if (len == 0) break;

		const uint32_t *next_tok = tok + len;
		const uint32_t *p = tok + 1;

		/* DCL 명령어 스킵 (이미 변수로 처리됨) */
		if (op >= 0x5F && op <= 0x68) {
			tok = next_tok;
			continue;
		}

		if (op == SM4_OP_RET) {
			emit_op(b, SpvOpReturn, 1);
			tok = next_tok;
			continue;
		}

		/* ALU 명령어 처리 */
		if (op == SM4_OP_MOV || op == SM4_OP_ADD ||
		    op == SM4_OP_MUL || op == SM4_OP_MAD ||
		    op == SM4_OP_DP3 || op == SM4_OP_DP4 ||
		    op == SM4_OP_LT  || op == SM4_OP_GE  ||
		    op == SM4_OP_EQ  || op == SM4_OP_NE  ||
		    op == SM4_OP_MIN || op == SM4_OP_MAX ||
		    op == SM4_OP_MOVC || op == SM4_OP_RSQ) {

			/* 대상 오퍼랜드 디코딩 */
			struct sm4_operand_info dst;
			if (decode_sm4_operand(&p, end, &dst, 1) < 0) break;

			/* 소스 오퍼랜드들 디코딩 */
			struct sm4_operand_info src[3];
			int num_srcs = (op == SM4_OP_MAD ||
					op == SM4_OP_MOVC) ? 3 :
				       (op == SM4_OP_MOV ||
					op == SM4_OP_RSQ) ? 1 : 2;

			for (int s = 0; s < num_srcs; s++) {
				if (decode_sm4_operand(&p, end, &src[s], 0) < 0)
					break;
			}

			/* 각 소스를 SPIR-V OpLoad → vec4 ID로 변환 */
			int src_ids[3] = {0, 0, 0};
			for (int s = 0; s < num_srcs; s++) {
				if (src[s].has_imm) {
					/* Immediate → OpConstant들로 vec4 구성 */
					int comp_ids[4];
					for (int c = 0; c < 4; c++) {
						comp_ids[c] = new_id(b);
						uint32_t bits;
						memcpy(&bits, &src[s].imm[c], 4);
						emit_op(b, SpvOpConstant, 4);
						emit(b, (uint32_t)b->id_float);
						emit(b, (uint32_t)comp_ids[c]);
						emit(b, bits);
					}
					src_ids[s] = new_id(b);
					emit_op(b, SpvOpCompositeConstruct, 7);
					emit(b, (uint32_t)b->id_vec4);
					emit(b, (uint32_t)src_ids[s]);
					emit(b, (uint32_t)comp_ids[0]);
					emit(b, (uint32_t)comp_ids[1]);
					emit(b, (uint32_t)comp_ids[2]);
					emit(b, (uint32_t)comp_ids[3]);
				} else {
					/* 레지스터 → OpLoad */
					int var_id = 0;
					int load_type = b->id_vec4;
					switch (src[s].type) {
					case SM4_OPERAND_INPUT:
						if (src[s].reg_idx < info->num_inputs) {
							var_id = b->id_inputs[src[s].reg_idx];
							/* VS input 0이 vec3이면 */
							if (b->is_vertex && src[s].reg_idx == 0 &&
							    info->inputs[0].mask == 0x07)
								load_type = b->id_vec3;
						}
						break;
					case SM4_OPERAND_OUTPUT:
						if (src[s].reg_idx < info->num_outputs)
							var_id = b->id_outputs[src[s].reg_idx];
						break;
					case SM4_OPERAND_TEMP:
						if (src[s].reg_idx < info->num_temps)
							var_id = b->id_temps[src[s].reg_idx];
						break;
					default:
						break;
					}

					if (var_id) {
						int loaded = new_id(b);
						emit_op(b, SpvOpLoad, 4);
						emit(b, (uint32_t)load_type);
						emit(b, (uint32_t)loaded);
						emit(b, (uint32_t)var_id);

						/* vec3 → vec4 확장 */
						if (load_type == b->id_vec3) {
							int ex0 = new_id(b);
							int ex1 = new_id(b);
							int ex2 = new_id(b);
							emit_op(b, SpvOpCompositeExtract, 5);
							emit(b, (uint32_t)b->id_float);
							emit(b, (uint32_t)ex0);
							emit(b, (uint32_t)loaded);
							emit(b, 0);
							emit_op(b, SpvOpCompositeExtract, 5);
							emit(b, (uint32_t)b->id_float);
							emit(b, (uint32_t)ex1);
							emit(b, (uint32_t)loaded);
							emit(b, 1);
							emit_op(b, SpvOpCompositeExtract, 5);
							emit(b, (uint32_t)b->id_float);
							emit(b, (uint32_t)ex2);
							emit(b, (uint32_t)loaded);
							emit(b, 2);
							int v4 = new_id(b);
							emit_op(b, SpvOpCompositeConstruct, 7);
							emit(b, (uint32_t)b->id_vec4);
							emit(b, (uint32_t)v4);
							emit(b, (uint32_t)ex0);
							emit(b, (uint32_t)ex1);
							emit(b, (uint32_t)ex2);
							emit(b, (uint32_t)b->id_float_1);
							loaded = v4;
						}
						src_ids[s] = loaded;
					}
				}
			}

			/* ALU 연산 수행 */
			int result_id = 0;

			switch (op) {
			case SM4_OP_MOV:
				result_id = src_ids[0];
				break;

			case SM4_OP_ADD:
				result_id = new_id(b);
				emit_op(b, SpvOpFAdd, 5);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				break;

			case SM4_OP_MUL:
				result_id = new_id(b);
				emit_op(b, SpvOpFMul, 5);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				break;

			case SM4_OP_MAD: {
				/* mad = a*b + c */
				int mul_id = new_id(b);
				emit_op(b, SpvOpFMul, 5);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)mul_id);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);

				result_id = new_id(b);
				emit_op(b, SpvOpFAdd, 5);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)mul_id);
				emit(b, (uint32_t)src_ids[2]);
				break;
			}

			case SM4_OP_DP4:
				result_id = new_id(b);
				/* OpDot returns scalar */
				emit_op(b, SpvOpDot, 5);
				emit(b, (uint32_t)b->id_float);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);

				/* scalar → vec4 broadcast */
				{
					int bcast = new_id(b);
					emit_op(b, SpvOpCompositeConstruct, 7);
					emit(b, (uint32_t)b->id_vec4);
					emit(b, (uint32_t)bcast);
					emit(b, (uint32_t)result_id);
					emit(b, (uint32_t)result_id);
					emit(b, (uint32_t)result_id);
					emit(b, (uint32_t)result_id);
					result_id = bcast;
				}
				break;

			case SM4_OP_DP3: {
				/* vec4에서 vec3 추출 후 dot */
				int a3 = new_id(b);
				emit_op(b, SpvOpVectorShuffle, 8);
				emit(b, (uint32_t)b->id_vec3);
				emit(b, (uint32_t)a3);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, 0); emit(b, 1); emit(b, 2);

				int b3 = new_id(b);
				emit_op(b, SpvOpVectorShuffle, 8);
				emit(b, (uint32_t)b->id_vec3);
				emit(b, (uint32_t)b3);
				emit(b, (uint32_t)src_ids[1]);
				emit(b, (uint32_t)src_ids[1]);
				emit(b, 0); emit(b, 1); emit(b, 2);

				int dot3 = new_id(b);
				emit_op(b, SpvOpDot, 5);
				emit(b, (uint32_t)b->id_float);
				emit(b, (uint32_t)dot3);
				emit(b, (uint32_t)a3);
				emit(b, (uint32_t)b3);

				result_id = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)dot3);
				emit(b, (uint32_t)dot3);
				emit(b, (uint32_t)dot3);
				emit(b, (uint32_t)dot3);
				break;
			}

			/* --- Class 53: 비교 연산 --- */

			case SM4_OP_LT: {
				/* lt → OpFOrdLessThan(bvec4) → OpSelect(vec4) */
				int cmp = new_id(b);
				emit_op(b, SpvOpFOrdLessThan, 5);
				emit(b, (uint32_t)b->id_bvec4);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				/* bvec4 → float mask: true=0xFFFFFFFF, false=0 */
				int id_all1 = new_id(b);
				uint32_t all_bits = 0xFFFFFFFF;
				emit_op(b, SpvOpConstant, 4);
				emit(b, (uint32_t)b->id_float);
				emit(b, (uint32_t)id_all1);
				emit(b, all_bits);
				int id_mask1 = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_mask1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				int id_zero_v = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_zero_v);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				result_id = new_id(b);
				emit_op(b, SpvOpSelect, 6);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)id_mask1);
				emit(b, (uint32_t)id_zero_v);
				break;
			}

			case SM4_OP_GE: {
				int cmp = new_id(b);
				emit_op(b, SpvOpFOrdGreaterThanEqual, 5);
				emit(b, (uint32_t)b->id_bvec4);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				int id_all1 = new_id(b);
				uint32_t allb = 0xFFFFFFFF;
				emit_op(b, SpvOpConstant, 4);
				emit(b, (uint32_t)b->id_float);
				emit(b, (uint32_t)id_all1);
				emit(b, allb);
				int id_m = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_m);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				int id_z = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_z);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				result_id = new_id(b);
				emit_op(b, SpvOpSelect, 6);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)id_m);
				emit(b, (uint32_t)id_z);
				break;
			}

			case SM4_OP_EQ: {
				int cmp = new_id(b);
				emit_op(b, SpvOpFOrdEqual, 5);
				emit(b, (uint32_t)b->id_bvec4);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				int id_all1 = new_id(b);
				uint32_t allb = 0xFFFFFFFF;
				emit_op(b, SpvOpConstant, 4);
				emit(b, (uint32_t)b->id_float);
				emit(b, (uint32_t)id_all1);
				emit(b, allb);
				int id_m = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_m);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				int id_z = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_z);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				result_id = new_id(b);
				emit_op(b, SpvOpSelect, 6);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)id_m);
				emit(b, (uint32_t)id_z);
				break;
			}

			case SM4_OP_NE: {
				int cmp = new_id(b);
				emit_op(b, SpvOpFUnordNotEqual, 5);
				emit(b, (uint32_t)b->id_bvec4);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				int id_all1 = new_id(b);
				uint32_t allb = 0xFFFFFFFF;
				emit_op(b, SpvOpConstant, 4);
				emit(b, (uint32_t)b->id_float);
				emit(b, (uint32_t)id_all1);
				emit(b, allb);
				int id_m = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_m);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				emit(b, (uint32_t)id_all1);
				int id_z = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_z);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				result_id = new_id(b);
				emit_op(b, SpvOpSelect, 6);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)id_m);
				emit(b, (uint32_t)id_z);
				break;
			}

			case SM4_OP_MIN:
				result_id = new_id(b);
				emit_op(b, SpvOpExtInst, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)b->id_glsl_ext);
				emit(b, GLSL_STD_450_FMin);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				break;

			case SM4_OP_MAX:
				result_id = new_id(b);
				emit_op(b, SpvOpExtInst, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)b->id_glsl_ext);
				emit(b, GLSL_STD_450_FMax);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)src_ids[1]);
				break;

			case SM4_OP_MOVC: {
				/* movc dst, cond, true, false
				 * cond is float mask — non-zero=true */
				int id_z = new_id(b);
				emit_op(b, SpvOpCompositeConstruct, 7);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)id_z);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				emit(b, (uint32_t)b->id_float_0);
				int cmp = new_id(b);
				emit_op(b, SpvOpFUnordNotEqual, 5);
				emit(b, (uint32_t)b->id_bvec4);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)src_ids[0]);
				emit(b, (uint32_t)id_z);
				result_id = new_id(b);
				emit_op(b, SpvOpSelect, 6);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)cmp);
				emit(b, (uint32_t)src_ids[1]);
				emit(b, (uint32_t)src_ids[2]);
				break;
			}

			case SM4_OP_RSQ:
				result_id = new_id(b);
				emit_op(b, SpvOpExtInst, 6);
				emit(b, (uint32_t)b->id_vec4);
				emit(b, (uint32_t)result_id);
				emit(b, (uint32_t)b->id_glsl_ext);
				emit(b, GLSL_STD_450_InverseSqrt);
				emit(b, (uint32_t)src_ids[0]);
				break;
			}

			/* 결과를 대상에 OpStore */
			if (result_id) {
				int dst_var = 0;
				switch (dst.type) {
				case SM4_OPERAND_OUTPUT:
					if (dst.reg_idx < info->num_outputs)
						dst_var = b->id_outputs[dst.reg_idx];
					break;
				case SM4_OPERAND_TEMP:
					if (dst.reg_idx < info->num_temps)
						dst_var = b->id_temps[dst.reg_idx];
					break;
				default:
					break;
				}

				if (dst_var) {
					emit_op(b, SpvOpStore, 3);
					emit(b, (uint32_t)dst_var);
					emit(b, (uint32_t)result_id);
				}
			}
		}

		tok = next_tok;
	}

	/* OpReturn이 아직 없으면 추가 */
	if (b->count > 0 &&
	    b->words[b->count - 1] != (uint32_t)((1 << 16) | SpvOpReturn)) {
		emit_op(b, SpvOpReturn, 1);
	}

	/* OpFunctionEnd */
	emit_op(b, SpvOpFunctionEnd, 1);

	/* bound 패치 (header word 3) */
	b->words[3] = (uint32_t)b->next_id;

	/* 출력 */
	*out_size = (size_t)b->count * sizeof(uint32_t);
	*out_spirv = malloc(*out_size);
	if (!*out_spirv) {
		free(body);
		free(b);
		return -1;
	}
	memcpy(*out_spirv, b->words, *out_size);

	free(body);
	free(b);
	return 0;
}
