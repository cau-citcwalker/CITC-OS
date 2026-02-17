/*
 * psf2.h — PSF2 폰트 파서 (header-only)
 * ========================================
 *
 * PSF2 (PC Screen Font version 2)란?
 *   Linux 콘솔에서 사용하는 비트맵 폰트 형식.
 *   각 글리프가 고정 크기 비트맵으로 저장됨.
 *
 *   예: Terminus 8x16
 *     - 글리프당 16바이트 (8px 너비 × 16px 높이)
 *     - 각 행이 1바이트 (8비트 = 8픽셀)
 *     - 비트 7(MSB)이 왼쪽 픽셀
 *
 *   PSF1은 매직 0x0436, PSF2는 0x864ab572.
 *   PSF2가 더 유연 (가변 크기 글리프, 유니코드 테이블 가능).
 *
 * 사용법:
 *   struct psf2_font font;
 *   if (psf2_load(&font, "/usr/share/fonts/ter-116n.psf") == 0) {
 *       psf2_draw_char(buf, stride, x, y, 'A', 0xFFFFFF, &font);
 *   }
 *
 * Fallback:
 *   PSF2 파일이 없으면 font8x8_basic 사용 (기존 동작).
 */

#ifndef PSF2_H
#define PSF2_H

#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

/* PSF2 매직 넘버 (리틀 엔디안) */
#define PSF2_MAGIC 0x864ab572

/*
 * PSF2 파일 헤더 (32바이트)
 *
 * 파일 구조:
 *   [헤더 32바이트] [글리프 데이터] [유니코드 테이블(옵션)]
 *
 * 글리프 데이터:
 *   numglyph × bytesperglyph 바이트
 *   각 글리프 = height행, 각 행 = ceil(width/8) 바이트
 */
struct psf2_header {
	uint32_t magic;          /* 0x864ab572 */
	uint32_t version;        /* 보통 0 */
	uint32_t headersize;     /* 헤더 크기 (보통 32) */
	uint32_t flags;          /* 1이면 유니코드 테이블 있음 */
	uint32_t numglyph;       /* 글리프 개수 (보통 256 또는 512) */
	uint32_t bytesperglyph;  /* 글리프당 바이트 수 */
	uint32_t height;         /* 글리프 높이 (픽셀) */
	uint32_t width;          /* 글리프 너비 (픽셀) */
};

/*
 * 로드된 PSF2 폰트 구조체
 */
struct psf2_font {
	uint32_t width;          /* 글리프 너비 */
	uint32_t height;         /* 글리프 높이 */
	uint32_t numglyph;       /* 글리프 수 */
	uint32_t bytesperglyph;  /* 글리프당 바이트 */
	uint8_t *glyphs;         /* 글리프 데이터 (malloc) */
	int loaded;              /* 로드 성공 여부 */
};

/*
 * PSF2 폰트 로드
 *
 * 파일을 열고 헤더를 검증한 뒤 글리프 데이터를 메모리에 로드.
 * 성공 시 0, 실패 시 -1 반환.
 */
static inline int psf2_load(struct psf2_font *font, const char *path)
{
	memset(font, 0, sizeof(*font));

	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;

	struct psf2_header hdr;

	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return -1;
	}

	if (hdr.magic != PSF2_MAGIC) {
		close(fd);
		return -1;
	}

	/* 헤더 뒤에 글리프 데이터 위치로 이동 */
	if (hdr.headersize > sizeof(hdr))
		lseek(fd, hdr.headersize, SEEK_SET);

	uint32_t data_size = hdr.numglyph * hdr.bytesperglyph;

	font->glyphs = (uint8_t *)malloc(data_size);
	if (!font->glyphs) {
		close(fd);
		return -1;
	}

	if ((uint32_t)read(fd, font->glyphs, data_size) != data_size) {
		free(font->glyphs);
		font->glyphs = NULL;
		close(fd);
		return -1;
	}

	close(fd);

	font->width = hdr.width;
	font->height = hdr.height;
	font->numglyph = hdr.numglyph;
	font->bytesperglyph = hdr.bytesperglyph;
	font->loaded = 1;

	return 0;
}

/*
 * PSF2 폰트 해제
 */
static inline void psf2_free(struct psf2_font *font)
{
	if (font->glyphs) {
		free(font->glyphs);
		font->glyphs = NULL;
	}
	font->loaded = 0;
}

/*
 * PSF2 글자 하나 그리기
 *
 * buf: XRGB8888 프레임버퍼
 * stride: 한 줄의 uint32_t 수 (= pitch / 4)
 * x, y: 좌상단 좌표
 * ch: ASCII 문자
 * color: XRGB 색상
 *
 * PSF2 비트 순서: MSB = 왼쪽 (font8x8과 반대!)
 *   font8x8: bit 0 = 왼쪽
 *   PSF2:    bit 7 = 왼쪽 (표준 비트맵 순서)
 */
static inline void psf2_draw_char(uint32_t *buf, int stride,
				  int x, int y, char ch, uint32_t color,
				  const struct psf2_font *font)
{
	unsigned char c = (unsigned char)ch;

	if (c >= font->numglyph)
		return;

	const uint8_t *glyph = font->glyphs + c * font->bytesperglyph;
	uint32_t bytes_per_row = (font->width + 7) / 8;

	for (uint32_t row = 0; row < font->height; row++) {
		int py = y + (int)row;

		if (py < 0)
			continue;

		const uint8_t *row_data = glyph + row * bytes_per_row;

		for (uint32_t col = 0; col < font->width; col++) {
			int px = x + (int)col;

			if (px < 0)
				continue;

			/* MSB = 왼쪽 픽셀 */
			if (row_data[col / 8] & (0x80 >> (col % 8)))
				buf[py * stride + px] = color;
		}
	}
}

/*
 * PSF2 문자열 그리기
 */
static inline void psf2_draw_string(uint32_t *buf, int stride,
				    int x, int y, const char *str,
				    uint32_t color,
				    const struct psf2_font *font)
{
	while (*str) {
		psf2_draw_char(buf, stride, x, y, *str, color, font);
		x += (int)font->width;
		str++;
	}
}

#endif /* PSF2_H */
