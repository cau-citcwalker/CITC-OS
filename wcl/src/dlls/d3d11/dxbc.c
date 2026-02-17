/*
 * dxbc.c — DXBC 바이트코드 파서 + SM4 CPU 인터프리터
 * ====================================================
 *
 * DXBC 컨테이너를 파싱하여 ISGN/OSGN/SHDR 청크를 추출하고,
 * SHDR의 SM4 명령어를 CPU에서 해석 실행하는 소프트웨어 셰이더 VM.
 *
 * 지원 명령어:
 *   mov, add, mul, mad, dp3, dp4, ret,
 *   lt, ge, eq, ne, min, max, movc, rsq,
 *   if, else, endif, loop, endloop, break, breakc
 *
 * 지원 오퍼랜드:
 *   temp(r#), input(v#), output(o#), immediate32, constant_buffer(cb#[#])
 */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "dxbc.h"

/* ============================================================
 * DXBC 컨테이너 파서
 * ============================================================
 *
 * DXBC 컨테이너 포맷:
 *   Offset 0:   "DXBC" (4바이트 매직)
 *   Offset 4:   MD5 체크섬 (16바이트)
 *   Offset 20:  버전 (4바이트, 항상 1)
 *   Offset 24:  전체 크기 (4바이트)
 *   Offset 28:  청크 개수 (4바이트)
 *   Offset 32:  청크 오프셋 배열 (4바이트 × 개수)
 *   ...         청크 데이터
 *
 * 청크 태그: "ISGN" (입력 시그니처), "OSGN" (출력), "SHDR" (셰이더)
 */

/* 시그니처 청크 파싱 (ISGN/OSGN 공통) */
static int parse_signature(const uint8_t *data, int data_size,
			   struct dxbc_sig_element *elements,
			   int max_elements)
{
	if (data_size < 8) return 0;

	uint32_t count = *(const uint32_t *)data;
	/* uint32_t reserved = *(const uint32_t *)(data + 4); */

	if ((int)count > max_elements)
		count = (uint32_t)max_elements;

	const uint8_t *elem_base = data + 8;

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = elem_base + i * 24;
		if (e + 24 > data + data_size) break;

		uint32_t name_offset = *(const uint32_t *)(e + 0);
		elements[i].semantic_idx = *(const int *)(e + 4);
		elements[i].system_value = *(const int *)(e + 8);
		/* component_type at offset 12 (3=float) */
		elements[i].register_num = *(const int *)(e + 16);
		elements[i].mask = (int)(*(const uint8_t *)(e + 20));

		/* 이름 문자열 복사 */
		if ((int)name_offset < data_size) {
			const char *name = (const char *)(data + name_offset);
			int len = 0;
			while (len < 31 &&
			       (int)(name_offset + len) < data_size &&
			       name[len])
				len++;
			memcpy(elements[i].name, name, (size_t)len);
			elements[i].name[len] = '\0';
		} else {
			elements[i].name[0] = '\0';
		}
	}

	return (int)count;
}

int dxbc_parse(const void *bytecode, size_t size, struct dxbc_info *info)
{
	memset(info, 0, sizeof(*info));

	if (!bytecode || size < 32) return -1;

	const uint8_t *data = (const uint8_t *)bytecode;

	/* 매직 체크 */
	if (data[0] != 'D' || data[1] != 'X' ||
	    data[2] != 'B' || data[3] != 'C')
		return -1;

	/* 청크 오프셋 테이블 */
	uint32_t chunk_count = *(const uint32_t *)(data + 28);
	if (32 + chunk_count * 4 > size) return -1;

	const uint32_t *offsets = (const uint32_t *)(data + 32);

	for (uint32_t i = 0; i < chunk_count; i++) {
		uint32_t off = offsets[i];
		if (off + 8 > size) continue;

		const uint8_t *chunk = data + off;
		uint32_t tag = *(const uint32_t *)chunk;
		uint32_t chunk_size = *(const uint32_t *)(chunk + 4);

		if (off + 8 + chunk_size > size) continue;

		const uint8_t *chunk_data = chunk + 8;

		if (tag == 0x4E475349) { /* "ISGN" */
			info->num_inputs = parse_signature(
				chunk_data, (int)chunk_size,
				info->inputs, DXBC_MAX_INPUTS);
		}
		else if (tag == 0x4E47534F) { /* "OSGN" */
			info->num_outputs = parse_signature(
				chunk_data, (int)chunk_size,
				info->outputs, DXBC_MAX_OUTPUTS);
		}
		else if (tag == 0x52444853) { /* "SHDR" */
			if (chunk_size >= 8) {
				uint32_t version =
					*(const uint32_t *)chunk_data;
				info->shader_type =
					(int)((version >> 16) & 0xFFFF);
				info->version_major =
					(int)((version >> 4) & 0xF);
				info->version_minor =
					(int)(version & 0xF);

				info->shader_token_count =
					*(const int *)(chunk_data + 4);
				info->shader_tokens =
					(const uint32_t *)chunk_data;

				/* dcl_temps 스캔 */
				const uint32_t *tok =
					(const uint32_t *)(chunk_data + 8);
				int remaining =
					info->shader_token_count - 2;

				while (remaining > 0) {
					uint32_t opcode_token = *tok;
					int op = (int)(opcode_token & 0x7FF);
					int len = (int)((opcode_token >> 24)
						  & 0x7F);
					if (len == 0) break;

					if (op == 104 /* DCL_TEMPS */ &&
					    len >= 2)
						info->num_temps = (int)tok[1];

					tok += len;
					remaining -= len;
				}
			}
		}
	}

	if (info->shader_tokens && info->shader_token_count > 0)
		info->valid = 1;

	return info->valid ? 0 : -1;
}

/* ============================================================
 * SM4 명령어 인터프리터
 * ============================================================
 *
 * SM4 오퍼랜드 토큰 포맷:
 *   Bits [1:0]   = 컴포넌트 수 (0=void, 1=1, 2=4, 3=N)
 *   Bits [3:2]   = 선택 모드 (0=mask, 1=swizzle, 2=select_1)
 *   Bits [7:4]   = mask/swizzle[0..1]
 *   Bits [11:8]  = swizzle[2..3]
 *   Bits [19:12] = 오퍼랜드 타입
 *   Bits [21:20] = 인덱스 차원 (0=0D, 1=1D, 2=2D, 3=3D)
 *   Bits [24:22] = 인덱스 표현[0]
 *   Bits [27:25] = 인덱스 표현[1]
 *   Bit [31]     = 확장 오퍼랜드
 */

/* 소스 오퍼랜드 읽기 → float[4] 반환 */
static int read_operand(const uint32_t **pp, const uint32_t *end,
			const struct shader_vm *vm,
			float out[4])
{
	if (*pp >= end) return -1;

	uint32_t token = **pp; (*pp)++;

	int num_comp = (int)(token & 3);
	int sel_mode = (int)((token >> 2) & 3);
	int op_type  = (int)((token >> 12) & 0xFF);
	int idx_dim  = (int)((token >> 20) & 3);

	/* 확장 오퍼랜드 스킵 */
	if (token & 0x80000000) {
		if (*pp >= end) return -1;
		(*pp)++;
	}

	/* 인덱스 읽기 */
	int idx[3] = {0, 0, 0};
	for (int d = 0; d < idx_dim; d++) {
		if (*pp >= end) return -1;
		idx[d] = (int)**pp; (*pp)++;
	}

	/* 소스 float4 가져오기 */
	const float *src = NULL;
	float imm[4] = {0, 0, 0, 0};

	switch (op_type) {
	case SM4_OPERAND_TEMP:
		if (idx[0] >= 0 && idx[0] < DXBC_MAX_TEMPS)
			src = vm->temps[idx[0]];
		break;
	case SM4_OPERAND_INPUT:
		if (idx[0] >= 0 && idx[0] < DXBC_MAX_INPUTS)
			src = vm->inputs[idx[0]];
		break;
	case SM4_OPERAND_OUTPUT:
		if (idx[0] >= 0 && idx[0] < DXBC_MAX_OUTPUTS)
			src = vm->outputs[idx[0]];
		break;
	case SM4_OPERAND_IMM32:
		if (num_comp == 2) { /* 4 컴포넌트 */
			if (*pp + 4 > end) return -1;
			memcpy(imm, *pp, 16);
			*pp += 4;
		} else if (num_comp == 1) { /* 1 컴포넌트 */
			if (*pp >= end) return -1;
			memcpy(&imm[0], *pp, 4);
			imm[1] = imm[2] = imm[3] = imm[0];
			*pp += 1;
		}
		src = imm;
		break;
	case SM4_OPERAND_CB:
		if (idx[0] >= 0 && idx[0] < 4 && vm->cb[idx[0]]) {
			int float_offset = idx[1] * 4; /* float4 단위 */
			int byte_size = vm->cb_size[idx[0]];
			if ((float_offset + 4) * (int)sizeof(float)
			    <= byte_size)
				src = &vm->cb[idx[0]][float_offset];
		}
		break;
	default:
		out[0] = out[1] = out[2] = out[3] = 0;
		return 0;
	}

	if (!src) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return 0;
	}

	/* swizzle/select 적용 */
	if (num_comp == 2 && sel_mode == 1) { /* swizzle */
		int sw[4];
		sw[0] = (int)((token >> 4) & 3);
		sw[1] = (int)((token >> 6) & 3);
		sw[2] = (int)((token >> 8) & 3);
		sw[3] = (int)((token >> 10) & 3);
		out[0] = src[sw[0]];
		out[1] = src[sw[1]];
		out[2] = src[sw[2]];
		out[3] = src[sw[3]];
	} else if (num_comp == 2 && sel_mode == 2) { /* select_1 */
		int sel = (int)((token >> 4) & 3);
		out[0] = out[1] = out[2] = out[3] = src[sel];
	} else if (num_comp == 1) { /* 스칼라 */
		out[0] = src[0];
		out[1] = out[2] = out[3] = 0;
	} else {
		memcpy(out, src, 16);
	}

	return 0;
}

/* 대상 오퍼랜드 디코딩: 쓰기 타겟 포인터 + 마스크 반환 */
static float *decode_dest(const uint32_t **pp, const uint32_t *end,
			  struct shader_vm *vm,
			  int *out_mask)
{
	if (*pp >= end) return NULL;

	uint32_t token = **pp; (*pp)++;

	int mask = (int)((token >> 4) & 0xF);
	int op_type = (int)((token >> 12) & 0xFF);
	int idx_dim = (int)((token >> 20) & 3);

	/* 확장 오퍼랜드 스킵 */
	if (token & 0x80000000) {
		if (*pp >= end) return NULL;
		(*pp)++;
	}

	int idx = 0;
	for (int d = 0; d < idx_dim; d++) {
		if (*pp >= end) return NULL;
		if (d == 0) idx = (int)**pp;
		(*pp)++;
	}

	*out_mask = mask;

	switch (op_type) {
	case SM4_OPERAND_TEMP:
		if (idx >= 0 && idx < DXBC_MAX_TEMPS)
			return vm->temps[idx];
		break;
	case SM4_OPERAND_OUTPUT:
		if (idx >= 0 && idx < DXBC_MAX_OUTPUTS)
			return vm->outputs[idx];
		break;
	}

	return NULL;
}

/* 마스크 적용 쓰기 */
static void write_masked(float *dst, int mask, const float val[4])
{
	if (!dst) return;
	if (mask & 1) dst[0] = val[0];
	if (mask & 2) dst[1] = val[1];
	if (mask & 4) dst[2] = val[2];
	if (mask & 8) dst[3] = val[3];
}

/*
 * flow control을 위한 forward-scan: 매칭되는 ELSE/ENDIF/ENDLOOP까지 스킵.
 * target: 찾으려는 opcode (SM4_OP_ELSE, SM4_OP_ENDIF, SM4_OP_ENDLOOP)
 * returns: target 명령어 다음 토큰 위치, 실패 시 NULL
 */
static const uint32_t *scan_to_matching(const uint32_t *tok,
					const uint32_t *end,
					int target)
{
	int depth = 0;

	while (tok < end) {
		uint32_t opcode_token = *tok;
		int op = (int)(opcode_token & 0x7FF);
		int len = (int)((opcode_token >> 24) & 0x7F);
		if (len == 0) break;

		if (op == SM4_OP_IF || op == SM4_OP_LOOP) {
			depth++;
		} else if (depth == 0 && op == target) {
			return tok + len;
		} else if (op == SM4_OP_ENDIF || op == SM4_OP_ENDLOOP) {
			if (depth > 0) depth--;
		} else if (depth == 0 && target == SM4_OP_ELSE &&
			   op == SM4_OP_ENDIF) {
			/* ELSE 없이 ENDIF 도달 */
			return tok + len;
		}

		tok += len;
	}

	return end;
}

/* SM4 비교 결과: 0.0 또는 0xFFFFFFFF (as float bits) */
static float cmp_true(void)
{
	uint32_t bits = 0xFFFFFFFF;
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

#define SM4_CMP_TRUE  cmp_true()
#define SM4_CMP_FALSE 0.0f

/* SM4 조건 테스트: 비트가 non-zero면 true */
static int test_condition(float val)
{
	uint32_t bits;
	memcpy(&bits, &val, 4);
	return bits != 0;
}

#define MAX_FLOW_DEPTH 16

int shader_vm_execute(struct shader_vm *vm, const struct dxbc_info *info)
{
	if (!info->valid || !info->shader_tokens)
		return -1;

	/* 명령어는 version + token_count (2 DWORD) 이후 시작 */
	const uint32_t *tok = info->shader_tokens + 2;
	const uint32_t *end = info->shader_tokens +
			      info->shader_token_count;

	/* 루프 지원: loop start 스택 */
	const uint32_t *loop_stack[MAX_FLOW_DEPTH];
	int loop_depth = 0;
	int loop_iter = 0;

	while (tok < end) {
		uint32_t opcode_token = *tok;
		int op = (int)(opcode_token & 0x7FF);
		int len = (int)((opcode_token >> 24) & 0x7F);

		if (len == 0) break;

		const uint32_t *next = tok + len;
		const uint32_t *p = tok + 1; /* opcode 토큰 스킵 */

		/* 선언문 스킵 (opcode >= 88은 DCL 계열) */
		if (op >= SM4_OP_DCL_RESOURCE) {
			tok = next;
			continue;
		}

		switch (op) {
		case SM4_OP_RET:
			return 0;

		/* === 기존 ALU === */

		case SM4_OP_MOV: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float src[4];
			read_operand(&p, end, vm, src);
			write_masked(dst, mask, src);
			break;
		}

		case SM4_OP_ADD: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = a[i] + b[i];
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_MUL: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = a[i] * b[i];
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_MAD: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], c[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			read_operand(&p, end, vm, c);
			for (int i = 0; i < 4; i++)
				result[i] = a[i] * b[i] + c[i];
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_DP3: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
			float result[4] = { dot, dot, dot, dot };
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_DP4: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			float dot = a[0]*b[0] + a[1]*b[1] +
				    a[2]*b[2] + a[3]*b[3];
			float result[4] = { dot, dot, dot, dot };
			write_masked(dst, mask, result);
			break;
		}

		/* === Class 53: 비교 연산 === */

		case SM4_OP_LT: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = (a[i] < b[i]) ?
					SM4_CMP_TRUE : SM4_CMP_FALSE;
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_GE: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = (a[i] >= b[i]) ?
					SM4_CMP_TRUE : SM4_CMP_FALSE;
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_EQ: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = (a[i] == b[i]) ?
					SM4_CMP_TRUE : SM4_CMP_FALSE;
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_NE: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = (a[i] != b[i]) ?
					SM4_CMP_TRUE : SM4_CMP_FALSE;
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_MIN: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = (a[i] < b[i]) ? a[i] : b[i];
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_MAX: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float a[4], b[4], result[4];
			read_operand(&p, end, vm, a);
			read_operand(&p, end, vm, b);
			for (int i = 0; i < 4; i++)
				result[i] = (a[i] > b[i]) ? a[i] : b[i];
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_MOVC: {
			/* movc dst, cond, true_val, false_val */
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float cond[4], t[4], f[4], result[4];
			read_operand(&p, end, vm, cond);
			read_operand(&p, end, vm, t);
			read_operand(&p, end, vm, f);
			for (int i = 0; i < 4; i++)
				result[i] = test_condition(cond[i]) ?
					t[i] : f[i];
			write_masked(dst, mask, result);
			break;
		}

		case SM4_OP_RSQ: {
			int mask;
			float *dst = decode_dest(&p, end, vm, &mask);
			float src[4], result[4];
			read_operand(&p, end, vm, src);
			for (int i = 0; i < 4; i++) {
				if (src[i] > 0.0f)
					result[i] = 1.0f / sqrtf(src[i]);
				else
					result[i] = 0.0f;
			}
			write_masked(dst, mask, result);
			break;
		}

		/* === Class 53: 흐름 제어 === */

		case SM4_OP_IF: {
			/* if_nz src0.x — 조건이 non-zero면 실행 */
			float cond[4];
			read_operand(&p, end, vm, cond);
			int nz = (opcode_token >> 18) & 1; /* 0=if_z, 1=if_nz */
			int val = test_condition(cond[0]);
			int take = nz ? val : !val;

			if (!take) {
				/* ELSE 또는 ENDIF까지 스킵 */
				next = scan_to_matching(next, end,
							SM4_OP_ELSE);
			}
			break;
		}

		case SM4_OP_ELSE:
			/* if 블록 실행 중이었으므로 ENDIF까지 스킵 */
			next = scan_to_matching(next, end, SM4_OP_ENDIF);
			break;

		case SM4_OP_ENDIF:
			/* no-op */
			break;

		case SM4_OP_LOOP:
			/* 루프 시작 — body 시작 위치 기록 */
			if (loop_depth < MAX_FLOW_DEPTH) {
				loop_stack[loop_depth++] = next;
				loop_iter = 0;
			}
			break;

		case SM4_OP_ENDLOOP:
			/* 루프 반복 — LOOP 다음으로 점프 */
			if (loop_depth > 0) {
				loop_iter++;
				if (loop_iter > 1024) {
					/* 무한 루프 방지 */
					loop_depth--;
				} else {
					next = loop_stack[loop_depth - 1];
				}
			}
			break;

		case SM4_OP_BREAK:
			/* 루프 탈출 — ENDLOOP까지 스킵 */
			if (loop_depth > 0)
				loop_depth--;
			next = scan_to_matching(next, end,
						SM4_OP_ENDLOOP);
			break;

		case SM4_OP_BREAKC: {
			/* breakc_nz/breakc_z src0.x */
			float cond[4];
			read_operand(&p, end, vm, cond);
			int nz = (opcode_token >> 18) & 1;
			int val = test_condition(cond[0]);
			int take = nz ? val : !val;

			if (take) {
				if (loop_depth > 0)
					loop_depth--;
				next = scan_to_matching(next, end,
							SM4_OP_ENDLOOP);
			}
			break;
		}

		default:
			/* 미지원 명령어 — 스킵 */
			break;
		}

		tok = next;
	}

	return 0;
}
