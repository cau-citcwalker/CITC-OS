/*
 * fbdraw.c - CITC OS 프레임버퍼 그래픽 데모
 * ============================================
 *
 * Linux 프레임버퍼(/dev/fb0)를 사용하여 화면에 직접 그리는 프로그램.
 *
 * 프레임버퍼란?
 *   화면의 각 픽셀 데이터를 담고 있는 메모리 영역.
 *   이 메모리에 값을 쓰면 → 화면에 그 색이 나타남!
 *
 *   800×600 해상도, 32비트 컬러:
 *   - 총 480,000 픽셀 (800 × 600)
 *   - 각 픽셀 = 4바이트 (B, G, R, A)
 *   - 총 메모리 = 1,920,000 바이트 ≈ 1.8MB
 *
 *   메모리 레이아웃 (32bpp BGRA):
 *     fb[0]  fb[1]  fb[2]  fb[3]  fb[4]  fb[5]  fb[6]  fb[7] ...
 *      B      G      R      A      B      G      R      A
 *     └──── 픽셀(0,0) ────┘ └──── 픽셀(1,0) ────┘
 *
 *   오프셋 계산:
 *     offset = y × stride + x × bytes_per_pixel
 *     (stride = 한 줄의 바이트 수, 보통 width × bpp/8)
 *
 * mmap (Memory-Mapped I/O):
 *   파일이나 장치를 프로세스의 메모리 공간에 직접 매핑.
 *   read()/write() 시스템콜 없이 배열처럼 접근 가능!
 *
 *   fb[offset] = 0xFF;  → 화면에 즉시 반영!
 *
 *   GPU 드라이버가 이 메모리 변경을 감지하여
 *   실제 디스플레이 신호로 변환.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "font8x8.h"

/* ============================================================
 * 프레임버퍼 컨텍스트 (전역 상태)
 * ============================================================
 *
 * 프레임버퍼에 그리기 위해 필요한 정보들.
 * 모든 그리기 함수가 이 구조체를 참조.
 */
static struct {
	int      fd;       /* /dev/fb0 파일 디스크립터 */
	uint8_t *mem;      /* mmap된 프레임버퍼 메모리 포인터 */
	uint32_t width;    /* 화면 가로 픽셀 수 */
	uint32_t height;   /* 화면 세로 픽셀 수 */
	uint32_t bpp;      /* bits per pixel (16 또는 32) */
	uint32_t stride;   /* 한 줄의 바이트 수 (line_length) */
	uint32_t size;     /* 전체 프레임버퍼 메모리 크기 */
	struct fb_var_screeninfo vinfo; /* 화면 변수 정보 */
} fb;

/* ============================================================
 * 프레임버퍼 초기화
 * ============================================================
 *
 * 1. /dev/fb0 열기
 * 2. ioctl로 화면 정보 쿼리 (해상도, bpp, 색상 레이아웃)
 * 3. mmap으로 프레임버퍼 메모리 매핑
 *
 * ioctl (Input/Output Control):
 *   장치에 특수 명령을 보내는 시스템콜.
 *   FBIOGET_VSCREENINFO → "화면 정보를 알려줘"
 *   FBIOGET_FSCREENINFO → "메모리 정보를 알려줘"
 */
static int fb_init(void)
{
	struct fb_fix_screeninfo finfo;

	fb.fd = open("/dev/fb0", O_RDWR);
	if (fb.fd < 0) {
		perror("open /dev/fb0");
		printf("\n프레임버퍼 장치를 열 수 없습니다.\n");
		printf("QEMU를 --gui 옵션으로 실행했는지 확인하세요:\n");
		printf("  bash tools/run-qemu.sh --gui\n");
		return -1;
	}

	/*
	 * fb_var_screeninfo: 변경 가능한 화면 정보
	 *   xres, yres     → 해상도 (800, 600)
	 *   bits_per_pixel  → 색 깊이 (16, 24, 32)
	 *   red/green/blue  → 각 색상 채널의 비트 위치와 길이
	 *
	 * 예시 (32bpp BGRA):
	 *   red:   offset=16, length=8  → 비트 16-23
	 *   green: offset=8,  length=8  → 비트 8-15
	 *   blue:  offset=0,  length=8  → 비트 0-7
	 */
	if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.vinfo) < 0) {
		perror("ioctl VSCREENINFO");
		close(fb.fd);
		return -1;
	}

	/*
	 * fb_fix_screeninfo: 고정된 하드웨어 정보
	 *   smem_len     → 프레임버퍼 전체 메모리 크기
	 *   line_length  → 한 줄(행)의 바이트 수
	 *
	 * line_length가 width × bpp/8과 다를 수 있음!
	 * (메모리 정렬 때문에 패딩이 들어갈 수 있다)
	 * → 항상 line_length를 사용해야 함.
	 */
	if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		perror("ioctl FSCREENINFO");
		close(fb.fd);
		return -1;
	}

	fb.width  = fb.vinfo.xres;
	fb.height = fb.vinfo.yres;
	fb.bpp    = fb.vinfo.bits_per_pixel;
	fb.stride = finfo.line_length;
	fb.size   = finfo.smem_len;

	printf("프레임버퍼 정보:\n");
	printf("  해상도:  %dx%d\n", fb.width, fb.height);
	printf("  색 깊이: %d bpp\n", fb.bpp);
	printf("  stride:  %d bytes/line\n", fb.stride);
	printf("  메모리:  %d bytes (%.1f MB)\n",
	       fb.size, fb.size / 1048576.0);
	printf("  R: offset=%d len=%d\n",
	       fb.vinfo.red.offset, fb.vinfo.red.length);
	printf("  G: offset=%d len=%d\n",
	       fb.vinfo.green.offset, fb.vinfo.green.length);
	printf("  B: offset=%d len=%d\n",
	       fb.vinfo.blue.offset, fb.vinfo.blue.length);

	/*
	 * mmap: 프레임버퍼를 프로세스 메모리에 매핑
	 *
	 * NULL       → 커널이 주소를 알아서 선택
	 * smem_len   → 매핑할 크기
	 * PROT_READ|WRITE → 읽기/쓰기 모두 허용
	 * MAP_SHARED → 변경이 장치에 즉시 반영 (핵심!)
	 * fb.fd, 0   → /dev/fb0의 오프셋 0부터
	 */
	fb.mem = mmap(NULL, fb.size,
		      PROT_READ | PROT_WRITE,
		      MAP_SHARED, fb.fd, 0);
	if (fb.mem == MAP_FAILED) {
		perror("mmap");
		close(fb.fd);
		return -1;
	}

	printf("  mmap:    %p\n\n", (void *)fb.mem);
	return 0;
}

/*
 * 프레임버퍼 화면 갱신 (flush)
 *
 * DRM fbdev 에뮬레이션에서는 mmap에 쓴 내용이
 * 자동으로 화면에 반영되지 않을 수 있음.
 *
 * 해결법:
 *   1. msync() - mmap 변경사항을 장치에 동기화
 *   2. write() - 버퍼 내용을 직접 /dev/fb0에 쓰기
 *   3. FBIOPAN_DISPLAY - 디스플레이 갱신 ioctl
 *
 * 가장 확실한 방법: write()로 전체 버퍼를 다시 쓰기.
 */
static void fb_flush(void)
{
	/* 방법 1: msync로 mmap 동기화 시도 */
	msync(fb.mem, fb.size, MS_SYNC);

	/* 방법 2: write()로 직접 쓰기 (가장 확실) */
	lseek(fb.fd, 0, SEEK_SET);
	if (write(fb.fd, fb.mem, fb.size) < 0)
		perror("write fb");

	/* 방법 3: 디스플레이 팬 (화면 갱신 트리거) */
	fb.vinfo.xoffset = 0;
	fb.vinfo.yoffset = 0;
	ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.vinfo);
}

/* 프레임버퍼 해제 */
static void fb_cleanup(void)
{
	if (fb.mem && fb.mem != MAP_FAILED)
		munmap(fb.mem, fb.size);
	if (fb.fd >= 0)
		close(fb.fd);
}

/* ============================================================
 * 픽셀 그리기 (핵심 함수!)
 * ============================================================
 *
 * 화면의 (x, y) 위치에 RGB 색상을 쓴다.
 *
 * 핵심 공식:
 *   offset = y × stride + x × (bpp ÷ 8)
 *
 * 32bpp 예시 (800×600):
 *   stride = 3200 (800 × 4)
 *   픽셀(100, 50)의 오프셋:
 *     50 × 3200 + 100 × 4 = 160,400
 *
 *   fb.mem[160400] = Blue
 *   fb.mem[160401] = Green
 *   fb.mem[160402] = Red
 *   fb.mem[160403] = Alpha (무시)
 */
static void fb_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	if (x < 0 || x >= (int)fb.width ||
	    y < 0 || y >= (int)fb.height)
		return;

	uint32_t offset = y * fb.stride + x * (fb.bpp / 8);

	if (fb.bpp == 32) {
		/*
		 * 32bpp: 4바이트/픽셀
		 *
		 * vinfo의 offset을 사용하여 R,G,B 비트 위치 결정.
		 * 대부분: B=0, G=8, R=16 (BGRA 순서)
		 *
		 * 비트 시프트로 32비트 정수에 색상 패킹:
		 *   pixel = (R << 16) | (G << 8) | (B << 0)
		 */
		uint32_t pixel = (r << fb.vinfo.red.offset) |
				 (g << fb.vinfo.green.offset) |
				 (b << fb.vinfo.blue.offset);
		*((uint32_t *)(fb.mem + offset)) = pixel;

	} else if (fb.bpp == 24) {
		/*
		 * 24bpp (RGB888): 3바이트/픽셀
		 *
		 * 32bpp처럼 보이지만 알파 바이트가 없음.
		 * B=offset 0, G=offset 1, R=offset 2
		 *
		 * 주의: 4바이트 단위 쓰기 불가!
		 * 바이트 단위로 하나씩 써야 함.
		 */
		fb.mem[offset]     = b;
		fb.mem[offset + 1] = g;
		fb.mem[offset + 2] = r;

	} else if (fb.bpp == 16) {
		/*
		 * 16bpp (RGB565): 2바이트/픽셀
		 *
		 * R: 5비트 (0-31), G: 6비트 (0-63), B: 5비트 (0-31)
		 * 총 65,536색 (2^16)
		 */
		uint16_t pixel = ((r >> 3) << 11) |
				 ((g >> 2) << 5) |
				 (b >> 3);
		*((uint16_t *)(fb.mem + offset)) = pixel;
	}
}

/* ============================================================
 * 그리기 기본 도구 (primitives)
 * ============================================================
 *
 * 모든 그래픽은 결국 fb_pixel()의 조합!
 * 사각형 = 가로 × 세로 만큼 fb_pixel() 반복.
 * 문자 = 8×8 비트맵의 켜진 비트마다 fb_pixel().
 */

/* 채워진 사각형 */
static void fb_rect(int x, int y, int w, int h,
		    uint8_t r, uint8_t g, uint8_t b)
{
	for (int dy = 0; dy < h; dy++)
		for (int dx = 0; dx < w; dx++)
			fb_pixel(x + dx, y + dy, r, g, b);
}

/*
 * 8×8 비트맵 문자 그리기
 *
 * scale: 확대 배율.
 *   scale=1 → 8×8 (원본)
 *   scale=2 → 16×16 (2배)
 *   scale=3 → 24×24 (3배)
 *
 * 비트맵 렌더링:
 *   font8x8_basic['A'] = {0x0C, 0x1E, 0x33, ...}
 *   각 바이트의 각 비트를 검사:
 *     if (row_byte & (1 << col)) → 이 픽셀은 켜짐!
 */
static void fb_char(int x, int y, char c,
		    uint8_t r, uint8_t g, uint8_t b, int scale)
{
	if (c < 0 || c > 126)
		c = '?';

	const uint8_t *glyph = font8x8_basic[(int)c];

	for (int row = 0; row < 8; row++) {
		for (int col = 0; col < 8; col++) {
			if (glyph[row] & (1 << col)) {
				/* scale배 확대: 각 픽셀을 scale×scale 블록으로 */
				for (int sy = 0; sy < scale; sy++)
					for (int sx = 0; sx < scale; sx++)
						fb_pixel(x + col * scale + sx,
							 y + row * scale + sy,
							 r, g, b);
			}
		}
	}
}

/* 문자열 그리기 */
static void fb_string(int x, int y, const char *str,
		      uint8_t r, uint8_t g, uint8_t b, int scale)
{
	int cx = x;

	while (*str) {
		if (*str == '\n') {
			cx = x;
			y += 8 * scale + scale;
		} else {
			fb_char(cx, y, *str, r, g, b, scale);
			cx += 8 * scale;
		}
		str++;
	}
}

/* 수직 그라데이션 (색상 보간) */
static void fb_gradient(int y1, int y2,
			uint8_t r1, uint8_t g1, uint8_t b1,
			uint8_t r2, uint8_t g2, uint8_t b2)
{
	int range = y2 - y1;

	if (range <= 0)
		return;

	/*
	 * 선형 보간 (linear interpolation):
	 *   t가 0 → 1로 변할 때, 색이 color1 → color2로 변함.
	 *
	 *   color = color1 + (color2 - color1) × t
	 *
	 *   여기서 t = (y - y1) / range
	 *   정수 연산: color1 + (color2 - color1) × (y - y1) / range
	 */
	for (int y = y1; y < y2 && y < (int)fb.height; y++) {
		int t = y - y1;
		uint8_t r = r1 + (r2 - r1) * t / range;
		uint8_t g = g1 + (g2 - g1) * t / range;
		uint8_t b = b1 + (b2 - b1) * t / range;

		for (uint32_t x = 0; x < fb.width; x++)
			fb_pixel(x, y, r, g, b);
	}
}

/* ============================================================
 * 메인: CITC OS 그래픽 데모
 * ============================================================ */
int main(void)
{
	printf("CITC OS Framebuffer Demo\n");
	printf("========================\n\n");

	if (fb_init() != 0)
		return 1;

	/* === 1. 배경: 어두운 파랑 → 검정 그라데이션 === */
	fb_gradient(0, fb.height, 0, 20, 60, 0, 0, 10);

	/* === 2. 상단 바 === */
	fb_rect(0, 0, fb.width, 50, 20, 40, 100);

	/* === 3. 타이틀 === */
	fb_string(20, 12, "CITC OS", 255, 255, 255, 3);
	fb_string(200, 20, "v0.5", 180, 180, 200, 2);

	/* === 4. 색상 팔레트 (컴퓨터 그래픽의 기본!) === */
	struct {
		uint8_t r, g, b;
		const char *name;
	} colors[] = {
		{255, 0,   0,   "Red"},
		{0,   255, 0,   "Green"},
		{0,   0,   255, "Blue"},
		{255, 255, 0,   "Yellow"},
		{255, 0,   255, "Magenta"},
		{0,   255, 255, "Cyan"},
		{255, 128, 0,   "Orange"},
		{255, 255, 255, "White"},
	};
	int num_colors = sizeof(colors) / sizeof(colors[0]);

	int bx = 40, by = 80, bsize = 60, gap = 20;

	fb_string(bx, by - 14, "Color Palette:", 200, 200, 200, 1);

	for (int i = 0; i < num_colors; i++) {
		int cx = bx + i * (bsize + gap);

		fb_rect(cx, by, bsize, bsize,
			colors[i].r, colors[i].g, colors[i].b);
		fb_string(cx + 2, by + bsize + 4,
			  colors[i].name,
			  colors[i].r, colors[i].g, colors[i].b, 1);
	}

	/* === 5. 시스템 정보 === */
	int iy = by + bsize + 40;
	char buf[128];

	fb_string(bx, iy, "System Info:", 200, 200, 200, 2);
	iy += 24;

	snprintf(buf, sizeof(buf), "Resolution: %dx%d", fb.width, fb.height);
	fb_string(bx, iy, buf, 150, 200, 150, 1);
	iy += 12;

	snprintf(buf, sizeof(buf), "Color: %d bpp", fb.bpp);
	fb_string(bx, iy, buf, 150, 200, 150, 1);
	iy += 12;

	snprintf(buf, sizeof(buf), "Memory: %d bytes", fb.size);
	fb_string(bx, iy, buf, 150, 200, 150, 1);
	iy += 12;

	snprintf(buf, sizeof(buf), "Stride: %d bytes/line", fb.stride);
	fb_string(bx, iy, buf, 150, 200, 150, 1);

	/* === 6. 하단 메시지 === */
	fb_string(bx, fb.height - 40,
		  "Drawn directly on the Linux framebuffer!",
		  100, 150, 255, 2);
	fb_string(bx, fb.height - 14,
		  "Press Enter in the serial console to exit...",
		  120, 120, 120, 1);

	/* 화면에 반영! (DRM fbdev 에뮬레이션 대응) */
	fb_flush();

	printf("그래픽이 QEMU 창에 표시되었습니다.\n");
	printf("Enter를 누르면 종료합니다.\n");

	/* 시리얼 콘솔에서 Enter 입력 대기 */
	getchar();

	/* 화면 정리 */
	memset(fb.mem, 0, fb.size);
	fb_flush();

	fb_cleanup();
	printf("프레임버퍼 정리 완료.\n");
	return 0;
}
