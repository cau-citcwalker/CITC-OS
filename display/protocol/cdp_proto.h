/*
 * CITC Display Protocol (CDP) — 프로토콜 정의
 * ============================================
 *
 * CDP는 Wayland 프로토콜의 핵심 개념을 가르치기 위한
 * 간소화된 디스플레이 프로토콜입니다.
 *
 * Wayland란?
 *   Linux 데스크탑의 디스플레이 서버 프로토콜.
 *   X11(1987년)을 대체하기 위해 2008년 시작.
 *
 *   핵심 아이디어:
 *     1. 앱(클라이언트)이 자기 메모리에 직접 그림
 *     2. 컴포지터(서버)가 여러 앱의 버퍼를 합성
 *     3. 소켓으로 제어 메시지를 주고받음
 *     4. 공유메모리(또는 GPU 버퍼)로 픽셀 데이터 공유
 *
 *   X11과의 차이:
 *     X11: 앱 → X서버에 "사각형 그려줘" 요청 → 서버가 그림
 *     Wayland: 앱이 직접 버퍼에 그림 → 서버에 "완성!" 알림 → 서버가 합성
 *
 * CDP ↔ Wayland 대응표:
 *   /tmp/citc-display-0      ↔  $XDG_RUNTIME_DIR/wayland-0
 *   struct cdp_msg_header    ↔  Wayland 와이어 프로토콜 헤더
 *   CDP_REQ_CREATE_SURFACE   ↔  wl_compositor.create_surface + xdg_toplevel
 *   CDP_REQ_ATTACH_BUFFER    ↔  wl_surface.attach(wl_buffer)
 *   CDP_REQ_COMMIT           ↔  wl_surface.commit
 *   CDP_EVT_FRAME_DONE       ↔  wl_callback.done
 *   memfd + SCM_RIGHTS       ↔  wl_shm_pool
 *
 * 이 헤더는 서버(compositor)와 클라이언트(cdp_demo) 모두가 include합니다.
 * 프로토콜 정의는 반드시 양쪽이 동일해야 하므로 하나의 파일에!
 */

#ifndef CDP_PROTO_H
#define CDP_PROTO_H

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ============================================================
 * 소켓 경로
 * ============================================================
 *
 * Wayland: $XDG_RUNTIME_DIR/wayland-0  (보통 /run/user/1000/wayland-0)
 * CDP:     /tmp/citc-display-0
 *
 * /tmp을 사용하는 이유:
 *   우리 initramfs에서 /tmp은 tmpfs (RAM 기반 파일시스템).
 *   XDG_RUNTIME_DIR 같은 복잡한 설정 없이 바로 사용 가능.
 */
#define CDP_SOCKET_PATH  "/tmp/citc-display-0"

/* ============================================================
 * 프로토콜 상수
 * ============================================================ */
#define CDP_VERSION        1
#define CDP_MSG_MAX_PAYLOAD  256   /* 최대 payload 크기 */

/* ============================================================
 * 메시지 헤더
 * ============================================================
 *
 * 모든 CDP 메시지는 이 헤더로 시작합니다.
 *
 * Wayland 와이어 프로토콜과의 비교:
 *   Wayland: object_id(4) + opcode(2) + size(2) + payload
 *     → 오브젝트 ID로 여러 오브젝트를 구분 (수백~수천 개)
 *
 *   CDP: type(4) + size(4) + payload
 *     → 단순화! 타입만으로 구분. 오브젝트 ID는 payload에 포함.
 *     → 교육 목적: 메시지 기반 프로토콜의 핵심만 이해하면 됨.
 */
struct cdp_msg_header {
	uint32_t type;    /* 메시지 타입 (enum cdp_request 또는 enum cdp_event) */
	uint32_t size;    /* payload 크기 (바이트, 헤더 미포함) */
};

/* ============================================================
 * 클라이언트 → 서버: 요청 (Request)
 * ============================================================
 *
 * Wayland에서는 이것을 "request"라고 부릅니다.
 * 클라이언트가 서버에게 무언가를 요청하는 메시지.
 */
enum cdp_request {
	/*
	 * Surface 생성 요청
	 * Wayland: wl_compositor.create_surface() + xdg_surface + xdg_toplevel
	 *
	 * Wayland에서는 surface와 toplevel이 분리되어 있지만
	 * CDP에서는 하나로 합침 (교육적 단순화).
	 */
	CDP_REQ_CREATE_SURFACE  = 1,

	/*
	 * Surface 삭제
	 * Wayland: wl_surface.destroy()
	 */
	CDP_REQ_DESTROY_SURFACE = 2,

	/*
	 * 공유메모리 버퍼 연결
	 * Wayland: wl_shm_pool_create_buffer() + wl_surface.attach()
	 *
	 * 이 메시지와 함께 memfd의 fd를 SCM_RIGHTS로 전달!
	 * → cdp_send_fd() 사용
	 */
	CDP_REQ_ATTACH_BUFFER   = 3,

	/*
	 * 화면 갱신 요청 ("그리기 완료!")
	 * Wayland: wl_surface.commit()
	 *
	 * commit의 핵심 개념:
	 *   클라이언트가 공유메모리에 픽셀을 그리는 동안
	 *   서버는 이전 상태를 표시합니다.
	 *   commit을 보내면 "새 내용으로 교체해줘"라는 뜻.
	 *   → 찢어짐(tearing) 방지!
	 */
	CDP_REQ_COMMIT          = 4,

	/*
	 * 프레임 콜백 요청
	 * Wayland: wl_surface.frame() → wl_callback
	 *
	 * "다음에 화면이 실제로 갱신될 때 알려줘"
	 * → 서버가 CDP_EVT_FRAME_DONE을 보내면 그때 다시 그리기
	 * → 불필요한 렌더링 방지, 전력 절약
	 */
	CDP_REQ_FRAME           = 5,

	/*
	 * 윈도우 제목 설정
	 * Wayland: xdg_toplevel.set_title()
	 */
	CDP_REQ_SET_TITLE       = 6,

	/*
	 * 패널(Panel) surface 설정
	 * Wayland: wlr-layer-shell (zwlr_layer_surface_v1)
	 *
	 * 일반 surface를 "패널"로 전환합니다.
	 * 패널이란?
	 *   - 화면 가장자리에 고정된 특수 윈도우
	 *   - Windows의 태스크바, macOS의 Dock과 같은 역할
	 *   - 항상 최상위에 표시 (다른 윈도우 위)
	 *   - 타이틀바/테두리 없음, 드래그 불가
	 *
	 * Wayland에서는 wlr-layer-shell 프로토콜이 이 역할:
	 *   layer: BOTTOM(월페이퍼) → BOTTOM → TOP → OVERLAY(잠금화면)
	 *   anchor: TOP/BOTTOM/LEFT/RIGHT
	 *   exclusive_zone: 다른 윈도우가 침범하지 않을 영역
	 *
	 * CDP에서는 단순화하여 bottom 패널만 지원.
	 */
	CDP_REQ_SET_PANEL       = 7,
};

/* ============================================================
 * 서버 → 클라이언트: 이벤트 (Event)
 * ============================================================
 *
 * Wayland에서는 이것을 "event"라고 부릅니다.
 * 서버가 클라이언트에게 알리는 메시지.
 */
enum cdp_event {
	/*
	 * 연결 환영 메시지
	 * Wayland: wl_display.global (글로벌 오브젝트 광고)
	 *
	 * 연결 직후 서버가 보내는 첫 메시지.
	 * 화면 크기 등 기본 정보를 알려줌.
	 */
	CDP_EVT_WELCOME         = 100,

	/*
	 * Surface 생성 완료 + ID 전달
	 * Wayland: wl_callback.done (for create request)
	 */
	CDP_EVT_SURFACE_ID      = 101,

	/*
	 * 프레임 완료 — 다음 프레임 그려도 됨
	 * Wayland: wl_callback.done (frame callback)
	 *
	 * 이 이벤트를 받으면 클라이언트가 다시 그리기 시작.
	 * 모니터 주사율에 맞춰 렌더링하는 핵심 메커니즘!
	 */
	CDP_EVT_FRAME_DONE      = 102,

	/* === 포인터(마우스) 이벤트 === */
	/*
	 * Wayland: wl_pointer.motion
	 * 좌표는 surface-local (윈도우 클라이언트 영역 기준)
	 */
	CDP_EVT_POINTER_MOTION  = 110,
	/* Wayland: wl_pointer.button */
	CDP_EVT_POINTER_BUTTON  = 111,
	/* Wayland: wl_pointer.enter — 마우스가 surface 위로 진입 */
	CDP_EVT_POINTER_ENTER   = 112,
	/* Wayland: wl_pointer.leave — 마우스가 surface 밖으로 나감 */
	CDP_EVT_POINTER_LEAVE   = 113,

	/* === 키보드 이벤트 === */
	/*
	 * Wayland: wl_keyboard.key
	 *
	 * 보안 관련 핵심 개념:
	 *   X11에서는 모든 앱이 다른 앱의 키입력을 볼 수 있었음!
	 *   (XGrabKeyboard 없이도 XSelectInput으로 가능)
	 *   → 키로거 공격에 취약
	 *
	 *   Wayland에서는 컴포지터가 포커스된 앱에만 키 이벤트를 보냄.
	 *   다른 앱은 절대 볼 수 없음. → 훨씬 안전!
	 */
	CDP_EVT_KEY             = 120,
	/* Wayland: wl_keyboard.enter — 이 surface가 포커스 받음 */
	CDP_EVT_FOCUS_IN        = 121,
	/* Wayland: wl_keyboard.leave — 이 surface가 포커스 잃음 */
	CDP_EVT_FOCUS_OUT       = 122,
};

/* ============================================================
 * Payload 구조체들
 * ============================================================
 *
 * 각 메시지 타입에 대응하는 데이터 구조.
 * 헤더 뒤에 이 구조체가 바로 따라옴.
 */

/* CDP_REQ_CREATE_SURFACE의 payload */
struct cdp_create_surface {
	int32_t x, y;            /* 초기 위치 (서버가 무시할 수 있음) */
	int32_t width, height;   /* 원하는 클라이언트 영역 크기 */
};

/* CDP_REQ_ATTACH_BUFFER의 payload (fd는 SCM_RIGHTS로 별도 전달!) */
struct cdp_attach_buffer {
	uint32_t surface_id;
	uint32_t width, height;  /* 버퍼 크기 (픽셀) */
	uint32_t stride;         /* 한 줄의 바이트 수 (= width * 4 for XRGB8888) */
	uint32_t format;         /* 0 = XRGB8888 (DRM과 동일 형식) */
};

/* CDP_REQ_COMMIT의 payload */
struct cdp_commit {
	uint32_t surface_id;
};

/* CDP_REQ_FRAME의 payload */
struct cdp_frame_req {
	uint32_t surface_id;
};

/* CDP_REQ_SET_TITLE의 payload */
struct cdp_set_title {
	uint32_t surface_id;
	char title[60];          /* null-terminated, 최대 59자 */
};

/* CDP_REQ_SET_PANEL의 payload */
struct cdp_set_panel {
	uint32_t surface_id;
	uint32_t edge;           /* 0=bottom, 1=top (현재는 bottom만) */
	uint32_t height;         /* 패널 높이 (픽셀) */
};

/* CDP_REQ_DESTROY_SURFACE의 payload */
struct cdp_destroy_surface {
	uint32_t surface_id;
};

/* CDP_EVT_WELCOME의 payload */
struct cdp_welcome {
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t version;        /* CDP_VERSION */
};

/* CDP_EVT_SURFACE_ID의 payload */
struct cdp_surface_id {
	uint32_t surface_id;
};

/* CDP_EVT_FRAME_DONE의 payload */
struct cdp_frame_done {
	uint32_t surface_id;
};

/* CDP_EVT_POINTER_MOTION의 payload */
struct cdp_pointer_motion {
	uint32_t surface_id;
	int32_t x, y;            /* surface-local 좌표 */
};

/* CDP_EVT_POINTER_BUTTON의 payload */
struct cdp_pointer_button {
	uint32_t surface_id;
	uint32_t button;         /* BTN_LEFT 등 (linux/input.h 값) */
	uint32_t state;          /* 1=pressed, 0=released */
};

/* CDP_EVT_POINTER_ENTER의 payload */
struct cdp_pointer_enter {
	uint32_t surface_id;
	int32_t x, y;            /* 진입 시 좌표 */
};

/* CDP_EVT_POINTER_LEAVE의 payload */
struct cdp_pointer_leave {
	uint32_t surface_id;
};

/* CDP_EVT_KEY의 payload */
struct cdp_key {
	uint32_t keycode;        /* Linux keycode (KEY_A 등) */
	uint32_t state;          /* 1=pressed, 0=released */
	uint32_t character;      /* ASCII 변환 결과 (0이면 변환 불가) */
};

/* CDP_EVT_FOCUS_IN의 payload */
struct cdp_focus_in {
	uint32_t surface_id;
};

/* CDP_EVT_FOCUS_OUT의 payload */
struct cdp_focus_out {
	uint32_t surface_id;
};

/* ============================================================
 * 메시지 송수신 헬퍼 함수
 * ============================================================
 *
 * 소켓에서 메시지를 보내고 받는 함수.
 *
 * 왜 단순 write()/read()가 아닌가?
 *   1. Partial write/read 문제:
 *      write(sock, buf, 100) → 50바이트만 보내질 수 있음!
 *      나머지 50바이트를 다시 보내야 함.
 *      TCP/Unix 소켓은 스트림이므로 경계(boundary)가 없음.
 *
 *   2. 헤더 + payload 분리:
 *      먼저 헤더(8바이트)를 보내고, 그 다음 payload를 보냄.
 *      받는 쪽도 헤더를 먼저 읽어 크기를 알고, 그만큼 payload를 읽음.
 */

/*
 * 전체 바이트 보내기 (partial write 처리)
 *
 * write()가 요청한 크기보다 적게 보낼 수 있으므로
 * 전체가 전송될 때까지 반복.
 */
static inline int cdp_write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;   /* 시그널에 의한 중단 → 재시도 */
			return -1;
		}
		if (n == 0)
			return -1;     /* 연결 끊김 */
		p += n;
		remaining -= (size_t)n;
	}
	return 0;
}

/*
 * 전체 바이트 읽기 (partial read 처리)
 */
static inline int cdp_read_all(int fd, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t n = read(fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;     /* 연결 끊김 (EOF) */
		p += n;
		remaining -= (size_t)n;
	}
	return 0;
}

/*
 * CDP 메시지 보내기: 헤더 + payload
 */
static inline int cdp_send_msg(int sock, uint32_t type,
			       const void *payload, uint32_t payload_size)
{
	struct cdp_msg_header hdr;

	hdr.type = type;
	hdr.size = payload_size;

	if (cdp_write_all(sock, &hdr, sizeof(hdr)) < 0)
		return -1;

	if (payload_size > 0 && payload) {
		if (cdp_write_all(sock, payload, payload_size) < 0)
			return -1;
	}

	return 0;
}

/*
 * CDP 메시지 받기: 헤더 + payload
 *
 * 반환: 메시지 타입 (>0), 에러 시 -1
 * payload_out에 데이터가 복사됨 (payload_max 이하)
 */
static inline int cdp_recv_msg(int sock, uint32_t *type_out,
			       void *payload_out, uint32_t payload_max,
			       uint32_t *payload_size_out)
{
	struct cdp_msg_header hdr;

	if (cdp_read_all(sock, &hdr, sizeof(hdr)) < 0)
		return -1;

	*type_out = hdr.type;
	if (payload_size_out)
		*payload_size_out = hdr.size;

	if (hdr.size > 0) {
		if (hdr.size > CDP_MSG_MAX_PAYLOAD) {
			/* 너무 큰 메시지 — 프로토콜 오류 */
			return -1;
		}
		if (hdr.size > payload_max) {
			/* payload 버퍼가 작음 — 읽고 버리기 */
			uint8_t discard[CDP_MSG_MAX_PAYLOAD];

			if (cdp_read_all(sock, discard, hdr.size) < 0)
				return -1;
			if (payload_max > 0)
				memcpy(payload_out, discard, payload_max);
		} else {
			if (cdp_read_all(sock, payload_out, hdr.size) < 0)
				return -1;
		}
	}

	return (int)hdr.type;
}

/* ============================================================
 * SCM_RIGHTS — 파일 디스크립터 전달
 * ============================================================
 *
 * Unix 도메인 소켓의 특별한 기능: 프로세스 간 fd 전달!
 *
 * 왜 필요한가?
 *   프로세스 A가 memfd_create()로 공유메모리를 만들면 fd=5를 받음.
 *   프로세스 B에게 숫자 "5"를 전달해봤자 B에서는 의미 없음.
 *   (B의 fd 테이블에서 5번은 전혀 다른 파일)
 *
 *   SCM_RIGHTS를 사용하면:
 *     커널이 A의 fd 5가 가리키는 실제 파일 객체(struct file)를
 *     B의 fd 테이블에 새 번호로 복사해줌.
 *     → 두 프로세스가 같은 메모리 영역을 공유!
 *
 * 이것이 Wayland에서 공유메모리가 작동하는 원리:
 *   1. 클라이언트: memfd_create → mmap → 픽셀 그리기
 *   2. 클라이언트: sendmsg(SCM_RIGHTS, fd) → 서버에 fd 전달
 *   3. 서버: recvmsg → fd 수신 → mmap → 같은 픽셀 데이터 접근!
 *
 * sendmsg/recvmsg:
 *   일반 send/recv와 달리 "보조 데이터(ancillary data)"를 보낼 수 있음.
 *   struct msghdr → msg_control에 보조 데이터를 담음.
 *   struct cmsghdr → cmsg_type = SCM_RIGHTS로 fd 전달.
 */

/*
 * fd를 소켓으로 전달 (SCM_RIGHTS)
 *
 * 데이터는 최소 1바이트 이상 함께 보내야 함 (프로토콜 규칙).
 * 실제 Wayland도 메시지와 함께 fd를 보냅니다.
 */
static inline int cdp_send_fd(int socket, int fd)
{
	/*
	 * 보조 데이터 버퍼
	 *
	 * CMSG_SPACE(sizeof(int)): fd 1개를 담기 위한 정렬된 크기
	 * 이 버퍼가 커널에게 "fd를 전달해줘"라고 알려주는 컨테이너
	 */
	char ctrl_buf[CMSG_SPACE(sizeof(int))];
	char dummy = 'F';    /* 최소 1바이트 데이터 (의미 없음) */
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;

	memset(&msg, 0, sizeof(msg));
	memset(ctrl_buf, 0, sizeof(ctrl_buf));

	/* 실제 데이터 (1바이트 더미) */
	iov.iov_base = &dummy;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	/* 보조 데이터 (fd 전달) */
	msg.msg_control = ctrl_buf;
	msg.msg_controllen = sizeof(ctrl_buf);

	/*
	 * cmsghdr 구조:
	 *   cmsg_level = SOL_SOCKET  → 소켓 레벨 제어
	 *   cmsg_type  = SCM_RIGHTS  → 파일 디스크립터 전달
	 *   cmsg_data  = fd값        → 전달할 fd
	 */
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

	if (sendmsg(socket, &msg, 0) < 0)
		return -1;

	return 0;
}

/*
 * 소켓에서 fd 수신 (SCM_RIGHTS)
 *
 * 반환: 수신된 fd (>= 0), 에러 시 -1
 */
static inline int cdp_recv_fd(int socket)
{
	char ctrl_buf[CMSG_SPACE(sizeof(int))];
	char dummy;
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;

	memset(&msg, 0, sizeof(msg));
	memset(ctrl_buf, 0, sizeof(ctrl_buf));

	iov.iov_base = &dummy;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	msg.msg_control = ctrl_buf;
	msg.msg_controllen = sizeof(ctrl_buf);

	if (recvmsg(socket, &msg, 0) <= 0)
		return -1;

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg ||
	    cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS) {
		return -1;
	}

	int received_fd;

	memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
	return received_fd;
}

#endif /* CDP_PROTO_H */
