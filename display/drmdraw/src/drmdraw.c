/*
 * drmdraw - CITC OS DRM/KMS Graphics Demo
 * =========================================
 *
 * 이전 fbdraw는 /dev/fb0 (fbdev)를 사용했습니다.
 * 이번에는 /dev/dri/card0 (DRM/KMS)를 직접 사용합니다.
 *
 * fbdev vs DRM/KMS:
 * ┌──────────────┬────────────────────┬──────────────────────┐
 * │              │ fbdev (/dev/fb0)   │ DRM (/dev/dri/card0) │
 * ├──────────────┼────────────────────┼──────────────────────┤
 * │ 역사         │ 1990년대           │ 2000년대~현재        │
 * │ 해상도       │ 부팅 시 고정       │ 런타임에 변경 가능   │
 * │ 모드 감지    │ 없음 (수동 설정)   │ EDID로 자동 감지     │
 * │ 더블 버퍼링  │ 불가능/불안정      │ 네이티브 지원        │
 * │ 페이지 플립  │ 없음 → 티어링      │ 원자적 → 티어링 없음 │
 * │ GPU 가속     │ 없음               │ GEM/DMA-BUF 가능     │
 * │ 다중 모니터  │ 별도 /dev/fb0,1..  │ 하나의 fd로 관리     │
 * │ 현대 앱      │ 사용하지 않음      │ Wayland/X11이 사용   │
 * └──────────────┴────────────────────┴──────────────────────┘
 *
 * DRM 핵심 개념:
 *
 *   모니터 ← Connector ← Encoder ← CRTC ← Framebuffer
 *   (물리)    (포트)      (변환기)   (엔진)   (픽셀 데이터)
 *
 *   Connector: 물리적 출력 포트 (HDMI, DP, VGA)
 *   Encoder:   픽셀 데이터 → 디스플레이 신호 변환
 *   CRTC:      "CRT Controller" - 프레임버퍼를 스캔아웃하는 엔진
 *   Framebuffer: GPU 메모리에 있는 픽셀 데이터
 *   Dumb Buffer: CPU가 접근 가능한 간단한 버퍼 (GPU 가속 없음)
 *
 * 더블 버퍼링이란?
 *   버퍼 2개를 번갈아 사용하는 기법.
 *   - Front buffer: 현재 화면에 표시 중
 *   - Back buffer: 다음 프레임을 그리는 중
 *   그리기 완료 → 두 버퍼를 교환 (page flip)
 *   → 그리는 도중의 불완전한 프레임이 보이지 않음!
 *
 * 빌드:
 *   gcc -static -Wall -o drmdraw drmdraw.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>

/*
 * DRM 커널 헤더
 *
 * 이 헤더들은 linux-libc-dev 패키지에 포함되어 있습니다.
 * 별도의 libdrm 설치가 필요 없습니다!
 *
 * drm.h:      기본 DRM 정의 (ioctl 번호)
 * drm_mode.h: 모드세팅 구조체 (connector, CRTC, mode 등)
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>

/*
 * DRM 커넥터 상태 상수
 *
 * 일부 시스템의 유저스페이스 헤더에 정의되어 있지 않을 수 있음.
 * 커널 소스 (include/uapi/drm/drm_mode.h)에서 값을 가져옴.
 */
#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED         1
#define DRM_MODE_DISCONNECTED      2
#define DRM_MODE_UNKNOWNCONNECTION 3
#endif

/* 8x8 비트맵 폰트 (fbdraw에서 공유) */
#include "../../fbdraw/src/font8x8.h"

/* ============================================================
 * 데이터 구조
 * ============================================================ */

/*
 * DRM Dumb Buffer
 *
 * "Dumb"인 이유: GPU 셰이더/가속을 사용하지 않음.
 * CPU가 직접 픽셀을 쓰는 가장 간단한 버퍼.
 *
 * 장점: 모든 DRM 드라이버에서 동작 (GPU 종류 무관)
 * 단점: GPU 가속 없음 (OpenGL/Vulkan은 별도)
 *
 * 나중에 Wayland 컴포지터에서는 GPU 가속 버퍼(GBM)를 씀.
 * 하지만 기본 원리는 동일: 버퍼 생성 → 그리기 → 화면에 표시.
 */
struct drm_buf {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;    /* 한 줄의 바이트 수 (padding 포함할 수 있음) */
	uint32_t size;     /* 전체 버퍼 크기 (bytes) */
	uint32_t handle;   /* GEM 핸들 (커널이 부여하는 버퍼 ID) */
	uint32_t fb_id;    /* DRM 프레임버퍼 ID */
	uint8_t *map;      /* mmap된 CPU 접근 포인터 */
};

/*
 * DRM 장치 상태
 *
 * 하나의 구조체에 모든 DRM 상태를 담습니다.
 * front/back 인덱스로 더블 버퍼링을 관리합니다.
 */
static struct {
	int fd;                          /* /dev/dri/card0 파일 디스크립터 */
	uint32_t conn_id;                /* 사용할 커넥터 ID */
	uint32_t crtc_id;                /* 사용할 CRTC ID */
	uint32_t enc_id;                 /* 인코더 ID */
	struct drm_mode_modeinfo mode;   /* 디스플레이 모드 (해상도, 주사율) */
	uint32_t saved_crtc_fb;          /* 복원용: 원래 프레임버퍼 */
	struct drm_buf bufs[2];          /* 더블 버퍼 */
	int front;                       /* 현재 front 버퍼 인덱스 (0 또는 1) */
} drm;

/* ============================================================
 * DRM Dumb Buffer 생성/삭제
 * ============================================================ */

/*
 * dumb 버퍼 생성
 *
 * 순서:
 *   1. CREATE_DUMB → GPU 메모리에 버퍼 할당 (handle 획득)
 *   2. ADDFB       → 버퍼를 DRM 프레임버퍼로 등록 (fb_id 획득)
 *   3. MAP_DUMB    → mmap 오프셋 획득
 *   4. mmap()      → CPU에서 접근 가능한 포인터 획득
 *
 * GEM (Graphics Execution Manager):
 *   GPU 메모리를 관리하는 커널 서브시스템.
 *   handle은 GEM이 부여하는 버퍼 식별자.
 */
static int buf_create(struct drm_buf *buf, uint32_t width, uint32_t height)
{
	struct drm_mode_create_dumb create = {0};
	struct drm_mode_map_dumb map_req = {0};
	struct drm_mode_fb_cmd fb_cmd = {0};

	buf->width = width;
	buf->height = height;

	/* 1단계: Dumb 버퍼 생성 */
	create.width = width;
	create.height = height;
	create.bpp = 32;   /* 32비트/픽셀 (XRGB8888) */

	if (ioctl(drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	buf->handle = create.handle;
	buf->pitch = create.pitch;
	buf->size = create.size;

	/*
	 * pitch vs width*4:
	 *   pitch = 한 줄의 실제 바이트 수
	 *   GPU가 성능을 위해 줄 사이에 패딩을 넣을 수 있음.
	 *   예: width=801 → pitch=3208 (4바이트 정렬)
	 *   항상 pitch를 사용해야 함! width*4를 쓰면 화면이 기울어짐.
	 */

	/* 2단계: DRM 프레임버퍼로 등록 */
	fb_cmd.width = width;
	fb_cmd.height = height;
	fb_cmd.pitch = buf->pitch;
	fb_cmd.bpp = 32;
	fb_cmd.depth = 24;   /* 색심도: 24비트 (RGB, 알파 제외) */
	fb_cmd.handle = buf->handle;

	if (ioctl(drm.fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) < 0) {
		perror("DRM_IOCTL_MODE_ADDFB");
		goto err_destroy;
	}

	buf->fb_id = fb_cmd.fb_id;

	/* 3단계: mmap 오프셋 요청 */
	map_req.handle = buf->handle;

	if (ioctl(drm.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		goto err_rmfb;
	}

	/*
	 * 4단계: mmap으로 CPU 접근
	 *
	 * mmap(addr, length, prot, flags, fd, offset):
	 *   DRM 장치의 GPU 메모리를 프로세스 주소 공간에 매핑.
	 *   이후 buf->map[offset] = value; 로 직접 픽셀 쓰기 가능.
	 *
	 * MAP_SHARED: 변경사항이 GPU 메모리에 직접 반영됨.
	 */
	buf->map = mmap(NULL, buf->size, PROT_READ | PROT_WRITE,
			MAP_SHARED, drm.fd, map_req.offset);

	if (buf->map == MAP_FAILED) {
		perror("mmap DRM buffer");
		goto err_rmfb;
	}

	/* 버퍼를 0으로 초기화 (검은색) */
	memset(buf->map, 0, buf->size);

	return 0;

err_rmfb:
	ioctl(drm.fd, DRM_IOCTL_MODE_RMFB, &buf->fb_id);
err_destroy:
	{
		struct drm_mode_destroy_dumb destroy = { .handle = buf->handle };
		ioctl(drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	return -1;
}

/* 버퍼 해제 */
static void buf_destroy(struct drm_buf *buf)
{
	struct drm_mode_destroy_dumb destroy = {0};

	if (buf->map && buf->map != MAP_FAILED)
		munmap(buf->map, buf->size);

	if (buf->fb_id)
		ioctl(drm.fd, DRM_IOCTL_MODE_RMFB, &buf->fb_id);

	if (buf->handle) {
		destroy.handle = buf->handle;
		ioctl(drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}

	memset(buf, 0, sizeof(*buf));
}

/* ============================================================
 * DRM 모드세팅 (디스플레이 초기화)
 * ============================================================
 *
 * 모드세팅 = 어떤 모니터에, 어떤 해상도로, 어떤 버퍼를 표시할지 설정.
 *
 * 탐색 순서:
 *   1. 리소스 조회 → 모든 커넥터/CRTC 목록
 *   2. 커넥터 순회 → "연결됨(connected)" 상태인 것을 찾음
 *   3. 해당 커넥터의 모드 목록에서 "preferred" 모드 선택
 *   4. 인코더 → CRTC 매핑
 *   5. Dumb 버퍼 생성
 *   6. CRTC에 버퍼 연결 (SETCRTC)
 */

static int drm_init(void)
{
	struct drm_mode_card_res res = {0};
	struct drm_mode_get_connector conn = {0};
	struct drm_mode_get_encoder enc = {0};
	struct drm_mode_crtc saved_crtc = {0};
	uint32_t *conn_ids = NULL;
	uint32_t *crtc_ids = NULL;
	uint32_t *enc_ids_res = NULL;  /* 리소스용 인코더 배열 */
	uint32_t *fb_ids = NULL;       /* 리소스용 FB 배열 */
	struct drm_mode_modeinfo *modes = NULL;
	uint32_t *enc_ids = NULL;     /* 커넥터용 인코더 배열 */
	uint32_t *props = NULL;       /* 커넥터 속성 ID 배열 */
	uint64_t *prop_values = NULL; /* 커넥터 속성 값 배열 */
	int found = 0;

	/* DRM 장치 열기 */
	drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm.fd < 0) {
		perror("/dev/dri/card0 열기 실패");
		printf("\n");
		printf("  DRM 장치가 없습니다.\n");
		printf("  QEMU --gui 모드에서 실행하세요.\n");
		return -1;
	}

	printf("DRM 장치 열기 성공: /dev/dri/card0 (fd=%d)\n\n", drm.fd);

	/*
	 * 1단계: 리소스 조회
	 *
	 * DRM_IOCTL_MODE_GETRESOURCES를 두 번 호출합니다:
	 *   1회차: 배열 크기만 획득 (count_*)
	 *   2회차: 실제 데이터 채우기
	 *
	 * 이 "2-pass" 패턴은 커널 ioctl에서 흔합니다.
	 * 커널이 먼저 크기를 알려주고, 유저가 메모리를 할당한 후
	 * 다시 호출하면 데이터를 채워줍니다.
	 */
	if (ioctl(drm.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("DRM_IOCTL_MODE_GETRESOURCES (1st)");
		goto err;
	}

	printf("DRM 리소스:\n");
	printf("  커넥터: %u개\n", res.count_connectors);
	printf("  CRTC:   %u개\n", res.count_crtcs);
	printf("  인코더: %u개\n\n", res.count_encoders);

	if (res.count_connectors == 0 || res.count_crtcs == 0) {
		printf("  연결된 디스플레이가 없습니다.\n");
		goto err;
	}

	/*
	 * 배열 할당
	 *
	 * 중요: 커널에 4개 배열 포인터를 모두 전달해야 합니다!
	 * connector, crtc, encoder, fb 중 하나라도 빠지면
	 * count > 0인데 포인터가 NULL이면 EFAULT(Bad address) 발생.
	 */
	conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
	crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
	enc_ids_res = calloc(res.count_encoders ? res.count_encoders : 1,
			     sizeof(uint32_t));
	fb_ids = calloc(res.count_fbs ? res.count_fbs : 1,
			sizeof(uint32_t));
	if (!conn_ids || !crtc_ids || !enc_ids_res || !fb_ids)
		goto err;

	/*
	 * 커널에 포인터 전달:
	 *   (uint64_t)(unsigned long) 캐스팅이 필요합니다.
	 *   커널 ioctl 인터페이스는 64비트 정수로 포인터를 전달합니다.
	 *   이유: 32비트/64비트 호환성 (같은 struct를 양쪽에서 사용).
	 */
	res.connector_id_ptr = (uint64_t)(unsigned long)conn_ids;
	res.crtc_id_ptr = (uint64_t)(unsigned long)crtc_ids;
	res.encoder_id_ptr = (uint64_t)(unsigned long)enc_ids_res;
	res.fb_id_ptr = (uint64_t)(unsigned long)fb_ids;

	if (ioctl(drm.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("DRM_IOCTL_MODE_GETRESOURCES (2nd)");
		goto err;
	}

	/*
	 * 2단계: 연결된 커넥터 찾기
	 *
	 * 커넥터 = 물리 포트 (HDMI, DP, VGA 등)
	 * connection == DRM_MODE_CONNECTED인 것을 찾습니다.
	 *
	 * 마찬가지로 2-pass: 먼저 크기, 그 다음 데이터.
	 */
	for (uint32_t i = 0; i < res.count_connectors && !found; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_ids[i];

		/* 1st pass: 크기 조회 */
		if (ioctl(drm.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
			printf("  커넥터 %u: GETCONNECTOR 실패\n",
			       conn_ids[i]);
			continue;
		}

		/*
		 * 연결 상태 확인:
		 *   1 = CONNECTED (물리 모니터 연결됨)
		 *   2 = DISCONNECTED (연결 안 됨)
		 *   3 = UNKNOWNCONNECTION (감지 불가)
		 *
		 * QEMU의 bochs-drm 같은 가상 디스플레이는
		 * UNKNOWNCONNECTION(3)을 보고할 수 있습니다.
		 * 가상 디스플레이에는 물리적 연결 감지가 없기 때문!
		 *
		 * 따라서 DISCONNECTED만 건너뛰고,
		 * CONNECTED와 UNKNOWNCONNECTION은 모두 시도합니다.
		 */
		printf("  커넥터 %u: connection=%u, modes=%u, encoders=%u\n",
		       conn.connector_id,
		       conn.connection,
		       conn.count_modes,
		       conn.count_encoders);

		if (conn.connection == DRM_MODE_DISCONNECTED)
			continue;

		/*
		 * 모드가 0개일 수 있는 경우:
		 * 일부 드라이버는 1st pass에서 modes 배열을 할당하지 않으면
		 * force-probe를 하지 않아 count_modes=0을 반환합니다.
		 * 이 경우 2nd pass까지 가서 확인해야 합니다.
		 */
		if (conn.count_modes == 0) {
			printf("    모드 없음 - 건너뜀\n");
			continue;
		}

		/*
		 * 모드/인코더/속성 배열 할당
		 *
		 * 중요: GETRESOURCES와 같은 규칙!
		 * count > 0인 모든 배열의 포인터를 반드시 채워야 합니다.
		 * 하나라도 빠지면 커널이 EFAULT를 반환합니다.
		 *
		 * drm_mode_get_connector 구조체의 배열 4개:
		 *   modes_ptr      / count_modes     → 디스플레이 모드
		 *   encoders_ptr   / count_encoders  → 인코더 ID
		 *   props_ptr      / count_props     → 속성 ID
		 *   prop_values_ptr / count_props    → 속성 값
		 */
		modes = calloc(conn.count_modes,
			       sizeof(struct drm_mode_modeinfo));
		enc_ids = calloc(conn.count_encoders ? conn.count_encoders : 1,
				 sizeof(uint32_t));
		props = calloc(conn.count_props ? conn.count_props : 1,
			       sizeof(uint32_t));
		prop_values = calloc(conn.count_props ? conn.count_props : 1,
				     sizeof(uint64_t));
		if (!modes || !enc_ids || !props || !prop_values) {
			free(modes); modes = NULL;
			free(enc_ids); enc_ids = NULL;
			free(props); props = NULL;
			free(prop_values); prop_values = NULL;
			continue;
		}

		conn.modes_ptr = (uint64_t)(unsigned long)modes;
		conn.encoders_ptr = (uint64_t)(unsigned long)enc_ids;
		conn.props_ptr = (uint64_t)(unsigned long)props;
		conn.prop_values_ptr = (uint64_t)(unsigned long)prop_values;

		/* 2nd pass: 데이터 채우기 */
		if (ioctl(drm.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
			perror("    GETCONNECTOR 2nd pass");
			free(modes); modes = NULL;
			free(enc_ids); enc_ids = NULL;
			free(props); props = NULL;
			free(prop_values); prop_values = NULL;
			continue;
		}

		printf("커넥터 %u: 연결됨\n", conn.connector_id);
		printf("  타입: %u\n", conn.connector_type);
		printf("  모드 수: %u\n", conn.count_modes);

		/*
		 * 3단계: 모드 선택
		 *
		 * DRM_MODE_TYPE_PREFERRED:
		 *   모니터의 EDID에 기본 해상도로 표시된 모드.
		 *   모니터 제조사가 "이 해상도가 최적"이라고 선언한 것.
		 *
		 * 없으면 첫 번째 모드 사용 (보통 가장 높은 해상도).
		 */
		drm.mode = modes[0];  /* 기본: 첫 번째 모드 */

		for (uint32_t j = 0; j < conn.count_modes; j++) {
			printf("  모드 %u: %ux%u @%uHz%s\n",
			       j,
			       modes[j].hdisplay,
			       modes[j].vdisplay,
			       modes[j].vrefresh,
			       (modes[j].type & DRM_MODE_TYPE_PREFERRED) ?
			       " (preferred)" : "");

			if (modes[j].type & DRM_MODE_TYPE_PREFERRED)
				drm.mode = modes[j];
		}

		drm.conn_id = conn.connector_id;

		/*
		 * 4단계: 인코더 → CRTC 매핑
		 *
		 * 인코더: 디지털 픽셀 → 디스플레이 신호 변환
		 * CRTC:   프레임버퍼를 스캔아웃하는 하드웨어 엔진
		 *
		 * 관계: 커넥터 → 인코더 → CRTC → 프레임버퍼
		 * 하나의 CRTC는 하나의 디스플레이에 출력.
		 */
		if (conn.encoder_id) {
			memset(&enc, 0, sizeof(enc));
			enc.encoder_id = conn.encoder_id;

			if (ioctl(drm.fd, DRM_IOCTL_MODE_GETENCODER,
				  &enc) == 0) {
				drm.crtc_id = enc.crtc_id;
				drm.enc_id = enc.encoder_id;
			}
		}

		/* CRTC를 아직 못 찾았으면 첫 번째 CRTC 사용 */
		if (!drm.crtc_id && res.count_crtcs > 0)
			drm.crtc_id = crtc_ids[0];

		found = 1;
	}

	if (!found) {
		printf("연결된 디스플레이를 찾을 수 없습니다.\n");
		goto err;
	}

	printf("\n선택된 설정:\n");
	printf("  해상도:  %ux%u @%uHz\n",
	       drm.mode.hdisplay, drm.mode.vdisplay, drm.mode.vrefresh);
	printf("  커넥터:  %u\n", drm.conn_id);
	printf("  CRTC:    %u\n", drm.crtc_id);
	printf("\n");

	/* 현재 CRTC 상태 저장 (나중에 복원용) */
	saved_crtc.crtc_id = drm.crtc_id;
	if (ioctl(drm.fd, DRM_IOCTL_MODE_GETCRTC, &saved_crtc) == 0)
		drm.saved_crtc_fb = saved_crtc.fb_id;

	/*
	 * 5단계: 더블 버퍼 생성
	 *
	 * 버퍼 2개를 만들어서 번갈아 사용합니다.
	 * bufs[0] = front (화면에 표시 중)
	 * bufs[1] = back  (다음 프레임 그리는 중)
	 */
	printf("더블 버퍼 생성 중...\n");

	if (buf_create(&drm.bufs[0], drm.mode.hdisplay,
		       drm.mode.vdisplay) < 0) {
		printf("  버퍼 0 생성 실패\n");
		goto err;
	}
	printf("  버퍼 0: %ux%u, pitch=%u, size=%u\n",
	       drm.bufs[0].width, drm.bufs[0].height,
	       drm.bufs[0].pitch, drm.bufs[0].size);

	if (buf_create(&drm.bufs[1], drm.mode.hdisplay,
		       drm.mode.vdisplay) < 0) {
		printf("  버퍼 1 생성 실패\n");
		buf_destroy(&drm.bufs[0]);
		goto err;
	}
	printf("  버퍼 1: %ux%u, pitch=%u, size=%u\n",
	       drm.bufs[1].width, drm.bufs[1].height,
	       drm.bufs[1].pitch, drm.bufs[1].size);

	/*
	 * 6단계: CRTC 설정 (첫 화면 표시)
	 *
	 * DRM_IOCTL_MODE_SETCRTC:
	 *   "이 CRTC가 이 프레임버퍼를 이 모드로 이 커넥터에 출력해라"
	 *
	 * 이 한 번의 ioctl로 실제 모니터에 영상이 나갑니다!
	 */
	{
		struct drm_mode_crtc crtc = {0};

		crtc.crtc_id = drm.crtc_id;
		crtc.fb_id = drm.bufs[0].fb_id;
		crtc.set_connectors_ptr = (uint64_t)(unsigned long)&drm.conn_id;
		crtc.count_connectors = 1;
		crtc.mode = drm.mode;
		crtc.mode_valid = 1;

		if (ioctl(drm.fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
			perror("DRM_IOCTL_MODE_SETCRTC");
			goto err_bufs;
		}
	}

	drm.front = 0;
	printf("\nDRM 초기화 완료!\n\n");

	free(conn_ids);
	free(crtc_ids);
	free(enc_ids_res);
	free(fb_ids);
	free(modes);
	free(enc_ids);
	free(props);
	free(prop_values);
	return 0;

err_bufs:
	buf_destroy(&drm.bufs[0]);
	buf_destroy(&drm.bufs[1]);
err:
	free(conn_ids);
	free(crtc_ids);
	free(enc_ids_res);
	free(fb_ids);
	free(modes);
	free(enc_ids);
	free(props);
	free(prop_values);
	if (drm.fd >= 0)
		close(drm.fd);
	drm.fd = -1;
	return -1;
}

/* DRM 정리 */
static void drm_cleanup(void)
{
	/* 원래 프레임버퍼 복원 시도 */
	if (drm.saved_crtc_fb) {
		struct drm_mode_crtc crtc = {0};
		crtc.crtc_id = drm.crtc_id;
		crtc.fb_id = drm.saved_crtc_fb;
		crtc.set_connectors_ptr =
			(uint64_t)(unsigned long)&drm.conn_id;
		crtc.count_connectors = 1;
		crtc.mode = drm.mode;
		crtc.mode_valid = 1;
		ioctl(drm.fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	}

	buf_destroy(&drm.bufs[0]);
	buf_destroy(&drm.bufs[1]);

	if (drm.fd >= 0)
		close(drm.fd);
}

/* ============================================================
 * 그리기 함수들
 * ============================================================
 * fbdraw와 비슷하지만, DRM 버퍼에 그립니다.
 * 핵심 차이: pitch를 사용한 오프셋 계산.
 */

/* 현재 back 버퍼 가져오기 */
static struct drm_buf *back_buf(void)
{
	return &drm.bufs[drm.front ^ 1];
}

/* 픽셀 그리기 (32bpp XRGB8888) */
static void drm_pixel(struct drm_buf *buf,
		      uint32_t x, uint32_t y,
		      uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t *pixel;

	if (x >= buf->width || y >= buf->height)
		return;

	/*
	 * 오프셋 계산:
	 *   y * pitch + x * 4
	 *
	 * pitch를 사용하는 이유:
	 *   GPU가 성능 최적화를 위해 줄 사이에 패딩을 넣을 수 있음.
	 *   pitch = 실제 한 줄의 바이트 수 (≥ width * 4)
	 *
	 * DRM은 항상 32bpp (XRGB8888):
	 *   X: 미사용 (8비트), R: 빨강, G: 초록, B: 파랑
	 *   fbdev의 24bpp 문제가 없음!
	 */
	pixel = (uint32_t *)(buf->map + y * buf->pitch + x * 4);
	*pixel = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* 사각형 그리기 */
static void drm_rect(struct drm_buf *buf,
		     uint32_t x, uint32_t y,
		     uint32_t w, uint32_t h,
		     uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

	for (uint32_t row = y; row < y + h && row < buf->height; row++) {
		uint32_t *line = (uint32_t *)(buf->map + row * buf->pitch);
		for (uint32_t col = x; col < x + w && col < buf->width; col++)
			line[col] = color;
	}
}

/* 그라디언트 배경 */
static void drm_gradient(struct drm_buf *buf,
			 uint8_t r1, uint8_t g1, uint8_t b1,
			 uint8_t r2, uint8_t g2, uint8_t b2)
{
	for (uint32_t y = 0; y < buf->height; y++) {
		uint32_t *line = (uint32_t *)(buf->map + y * buf->pitch);
		uint8_t r = r1 + (r2 - r1) * y / buf->height;
		uint8_t g = g1 + (g2 - g1) * y / buf->height;
		uint8_t b = b1 + (b2 - b1) * y / buf->height;
		uint32_t color = ((uint32_t)r << 16) |
				 ((uint32_t)g << 8) | (uint32_t)b;

		for (uint32_t x = 0; x < buf->width; x++)
			line[x] = color;
	}
}

/* 글자 그리기 (8x8 비트맵 폰트) */
static void drm_char(struct drm_buf *buf,
		     uint32_t x, uint32_t y, char c,
		     uint8_t r, uint8_t g, uint8_t b, int scale)
{
	unsigned char ch = (unsigned char)c;
	if (ch > 127)
		return;

	for (int row = 0; row < 8; row++) {
		uint8_t bits = font8x8_basic[ch][row];
		for (int col = 0; col < 8; col++) {
			if (bits & (1 << col)) {
				for (int sy = 0; sy < scale; sy++)
					for (int sx = 0; sx < scale; sx++)
						drm_pixel(buf,
							  x + col * scale + sx,
							  y + row * scale + sy,
							  r, g, b);
			}
		}
	}
}

/* 문자열 그리기 */
static void drm_string(struct drm_buf *buf,
		       uint32_t x, uint32_t y, const char *str,
		       uint8_t r, uint8_t g, uint8_t b, int scale)
{
	while (*str) {
		drm_char(buf, x, y, *str, r, g, b, scale);
		x += 8 * scale;
		str++;
	}
}

/* ============================================================
 * 버퍼 스왑 (더블 버퍼링의 핵심)
 * ============================================================
 *
 * SETCRTC로 CRTC가 표시할 프레임버퍼를 교체합니다.
 *
 * 실제 프로덕션에서는 DRM_IOCTL_MODE_PAGE_FLIP을 사용합니다.
 * PAGE_FLIP은 vblank에 맞춰 원자적으로 교체 → 티어링 없음.
 * 여기서는 SETCRTC을 사용하지만, 원리는 동일합니다.
 */
static void drm_swap(void)
{
	struct drm_mode_crtc crtc = {0};
	int back = drm.front ^ 1;

	crtc.crtc_id = drm.crtc_id;
	crtc.fb_id = drm.bufs[back].fb_id;
	crtc.set_connectors_ptr = (uint64_t)(unsigned long)&drm.conn_id;
	crtc.count_connectors = 1;
	crtc.mode = drm.mode;
	crtc.mode_valid = 1;

	if (ioctl(drm.fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
		perror("swap: DRM_IOCTL_MODE_SETCRTC");
		return;
	}

	drm.front = back;
}

/* ============================================================
 * 데모 장면 그리기
 * ============================================================ */

static void draw_info_scene(struct drm_buf *buf)
{
	char info[128];
	uint32_t bx = 20;  /* base x */

	/* 배경: 그라디언트 */
	drm_gradient(buf, 10, 10, 40, 30, 30, 80);

	/* 타이틀 바 */
	drm_rect(buf, 0, 0, buf->width, 50, 30, 100, 200);
	drm_string(buf, bx, 15, "CITC OS - DRM/KMS Graphics", 255, 255, 255, 3);

	/* 시스템 정보 */
	uint32_t y = 70;

	drm_string(buf, bx, y, "=== DRM/KMS Info ===", 100, 200, 255, 2);
	y += 30;

	snprintf(info, sizeof(info), "Resolution: %ux%u @%uHz",
		 drm.mode.hdisplay, drm.mode.vdisplay, drm.mode.vrefresh);
	drm_string(buf, bx, y, info, 200, 200, 200, 2);
	y += 25;

	snprintf(info, sizeof(info), "Pitch: %u bytes/line",
		 buf->pitch);
	drm_string(buf, bx, y, info, 200, 200, 200, 2);
	y += 25;

	snprintf(info, sizeof(info), "Buffer size: %u KB (%u bytes)",
		 buf->size / 1024, buf->size);
	drm_string(buf, bx, y, info, 200, 200, 200, 2);
	y += 25;

	snprintf(info, sizeof(info), "Pixel format: XRGB8888 (32bpp)");
	drm_string(buf, bx, y, info, 200, 200, 200, 2);
	y += 25;

	snprintf(info, sizeof(info), "Double buffering: ON (2 buffers)");
	drm_string(buf, bx, y, info, 100, 255, 100, 2);
	y += 40;

	/* fbdev vs DRM 비교 */
	drm_string(buf, bx, y, "=== fbdev vs DRM ===", 255, 200, 100, 2);
	y += 30;

	drm_string(buf, bx, y, "fbdev: write() copy, no vsync, tearing",
		   255, 100, 100, 2);
	y += 25;

	drm_string(buf, bx, y, "DRM:   page flip, vsync, tear-free!",
		   100, 255, 100, 2);
	y += 40;

	/* 색상 팔레트 */
	drm_string(buf, bx, y, "Color palette:", 200, 200, 200, 2);
	y += 25;

	struct { uint8_t r, g, b; const char *name; } colors[] = {
		{255, 0,   0,   "R"},
		{0,   255, 0,   "G"},
		{0,   0,   255, "B"},
		{255, 255, 0,   "Y"},
		{255, 0,   255, "M"},
		{0,   255, 255, "C"},
		{255, 255, 255, "W"},
		{128, 128, 128, "Gr"},
	};

	for (int i = 0; i < 8; i++) {
		uint32_t cx = bx + i * 90;
		drm_rect(buf, cx, y, 70, 40,
			 colors[i].r, colors[i].g, colors[i].b);
		drm_string(buf, cx + 25, y + 12, colors[i].name,
			   0, 0, 0, 2);
	}
}

/* ============================================================
 * 더블 버퍼링 애니메이션 데모
 * ============================================================
 *
 * 더블 버퍼링의 효과를 보여주는 간단한 애니메이션:
 * 사각형이 화면을 가로질러 움직입니다.
 *
 * 과정:
 *   1. Back 버퍼에 배경 + 사각형 그리기
 *   2. swap() → Back이 Front가 됨 (화면에 표시)
 *   3. 새로운 Back 버퍼에 다음 프레임 그리기
 *   4. 반복
 *
 * 티어링 없이 부드러운 애니메이션!
 */
static void animate_demo(int frames)
{
	uint32_t box_x = 0;
	uint32_t box_w = 80;
	uint32_t box_h = 60;
	int dx = 4;  /* x 방향 이동 속도 */

	printf("애니메이션 시작 (%d 프레임)...\n", frames);

	for (int f = 0; f < frames; f++) {
		struct drm_buf *buf = back_buf();

		/* 배경 그리기 */
		drm_gradient(buf, 10, 10, 40, 30, 30, 80);

		/* 타이틀 */
		drm_rect(buf, 0, 0, buf->width, 40, 30, 100, 200);
		drm_string(buf, 20, 10, "DRM Double Buffering Demo",
			   255, 255, 255, 2);

		/* 움직이는 사각형 */
		uint32_t box_y = buf->height / 2 - box_h / 2;
		drm_rect(buf, box_x, box_y, box_w, box_h, 255, 100, 50);

		/* 프레임 카운터 */
		char info[64];
		snprintf(info, sizeof(info), "Frame: %d/%d  Box X: %u",
			 f + 1, frames, box_x);
		drm_string(buf, 20, buf->height - 30, info,
			   150, 150, 150, 2);

		/* 버퍼 스왑! */
		drm_swap();

		/* 사각형 이동 */
		box_x += dx;
		if (box_x + box_w >= buf->width || box_x == 0)
			dx = -dx;

		/* 약간의 딜레이 (약 30fps) */
		usleep(33000);
	}

	printf("애니메이션 완료.\n");
}

/* ============================================================
 * 메인 함수
 * ============================================================ */

int main(void)
{
	printf("\n");
	printf("=== CITC OS DRM/KMS Graphics Demo ===\n");
	printf("\n");

	/* DRM 초기화 */
	if (drm_init() < 0) {
		printf("DRM 초기화 실패!\n");
		return 1;
	}

	/* 1. 정보 화면 표시 (버퍼 0에 그리기) */
	printf("정보 화면 그리기...\n");
	draw_info_scene(&drm.bufs[0]);

	/* SETCRTC로 표시 (이미 bufs[0]이 front) */
	{
		struct drm_mode_crtc crtc = {0};
		crtc.crtc_id = drm.crtc_id;
		crtc.fb_id = drm.bufs[0].fb_id;
		crtc.set_connectors_ptr =
			(uint64_t)(unsigned long)&drm.conn_id;
		crtc.count_connectors = 1;
		crtc.mode = drm.mode;
		crtc.mode_valid = 1;
		ioctl(drm.fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	}

	printf("QEMU 창에서 그래픽을 확인하세요.\n");
	printf("Enter를 누르면 애니메이션 데모로 진행합니다.\n");
	getchar();

	/* 2. 더블 버퍼링 애니메이션 데모 */
	animate_demo(150);  /* 약 5초 (150 frames @ 30fps) */

	printf("\nEnter를 누르면 종료합니다.\n");
	getchar();

	/* 정리 */
	drm_cleanup();
	printf("DRM 정리 완료.\n");

	return 0;
}
