/*
 * shader_cache.c — DXBC → SPIR-V 디스크 캐시
 * ==============================================
 *
 * FNV-1a 64비트 해시로 DXBC blob을 식별하고,
 * ~/.citc/shader_cache/<hex>.spv 파일에 SPIR-V를 캐시.
 *
 * 캐시 무효화: DXBC blob 전체를 해시하므로,
 * blob이 1바이트라도 변경되면 자동으로 재컴파일.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include "shader_cache.h"

/* FNV-1a 64-bit */
#define FNV_OFFSET_BASIS 0xCBF29CE484222325ULL
#define FNV_PRIME        0x100000001B3ULL

static uint64_t fnv1a_64(const uint8_t *data, size_t len)
{
	uint64_t hash = FNV_OFFSET_BASIS;

	for (size_t i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= FNV_PRIME;
	}

	return hash;
}

/* 캐시 디렉토리 경로 (lazily created) */
static int get_cache_dir(char *buf, size_t buflen)
{
	const char *home = getenv("HOME");
	if (!home) home = "/tmp";

	int n = snprintf(buf, buflen, "%s/.citc/shader_cache", home);
	if (n < 0 || (size_t)n >= buflen) return -1;

	return 0;
}

static int ensure_cache_dir(void)
{
	char dir[512];
	if (get_cache_dir(dir, sizeof(dir)) < 0) return -1;

	/* ~/.citc 먼저 */
	char parent[512];
	const char *home = getenv("HOME");
	if (!home) home = "/tmp";
	snprintf(parent, sizeof(parent), "%s/.citc", home);
	mkdir(parent, 0755); /* 이미 존재해도 OK */

	mkdir(dir, 0755);
	return 0;
}

static void hash_to_path(uint64_t hash, char *buf, size_t buflen)
{
	char dir[512];
	get_cache_dir(dir, sizeof(dir));

	snprintf(buf, buflen, "%s/%016llx.spv",
		 dir, (unsigned long long)hash);
}

int shader_cache_lookup(const uint8_t *dxbc, size_t dxbc_size,
			uint32_t **out_spirv, size_t *out_size)
{
	if (!dxbc || dxbc_size == 0 || !out_spirv || !out_size)
		return -1;

	uint64_t hash = fnv1a_64(dxbc, dxbc_size);
	char path[600];
	hash_to_path(hash, path, sizeof(path));

	FILE *f = fopen(path, "rb");
	if (!f) return -1;

	/* 파일 크기 */
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (fsize <= 0 || fsize > 1024 * 1024) {
		fclose(f);
		return -1;
	}

	uint32_t *spirv = malloc((size_t)fsize);
	if (!spirv) {
		fclose(f);
		return -1;
	}

	size_t rd = fread(spirv, 1, (size_t)fsize, f);
	fclose(f);

	if (rd != (size_t)fsize) {
		free(spirv);
		return -1;
	}

	/* SPIR-V 매직 검증 */
	if (rd >= 4 && spirv[0] != 0x07230203) {
		free(spirv);
		return -1;
	}

	*out_spirv = spirv;
	*out_size = (size_t)fsize;
	return 0;
}

void shader_cache_store(const uint8_t *dxbc, size_t dxbc_size,
			const uint32_t *spirv, size_t spirv_size)
{
	if (!dxbc || dxbc_size == 0 || !spirv || spirv_size == 0)
		return;

	ensure_cache_dir();

	uint64_t hash = fnv1a_64(dxbc, dxbc_size);
	char path[600];
	hash_to_path(hash, path, sizeof(path));

	FILE *f = fopen(path, "wb");
	if (!f) return;

	fwrite(spirv, 1, spirv_size, f);
	fclose(f);
}
