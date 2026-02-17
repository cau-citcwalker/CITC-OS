/*
 * CITC Audio Protocol — 프로토콜 정의
 * ====================================
 *
 * CDP(CITC Display Protocol)의 오디오 버전.
 * Unix 소켓 기반 메시지 프로토콜로,
 * 여러 클라이언트의 오디오 스트림을 서버가 믹싱.
 *
 * 왜 오디오 서버가 필요한가?
 *   리눅스에서 /dev/dsp (OSS)는 한 번에 하나의 프로세스만 열 수 있다.
 *   PulseAudio/PipeWire가 하는 일:
 *     앱A → PCM 데이터 전송
 *     앱B → PCM 데이터 전송
 *     서버: 믹싱 (합산 + 클램핑) → 하드웨어에 출력
 *
 *   이것이 citcaudio가 하는 일!
 *
 * 프로토콜 흐름:
 *   1. 클라이언트: 소켓 연결 (/tmp/citc-audio-0)
 *   2. 클라이언트: OPEN_STREAM → 서버: STREAM_ID 응답
 *   3. 클라이언트: WRITE (PCM 데이터) 반복
 *   4. 클라이언트: CLOSE_STREAM
 *
 * 서버 루프 (10ms 주기):
 *   모든 스트림의 링버퍼에서 샘플 읽기
 *   → 선형 믹싱 (int32_t 합산 + int16_t 클램핑)
 *   → /dev/dsp에 write
 */

#ifndef CITCAUDIO_PROTO_H
#define CITCAUDIO_PROTO_H

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================
 * 소켓 경로
 * ============================================================
 *
 * CDP: /tmp/citc-display-0
 * 오디오: /tmp/citc-audio-0
 */
#define CITCAUDIO_SOCKET_PATH  "/tmp/citc-audio-0"

/* 오디오 기본 설정 */
#define CITCAUDIO_SAMPLE_RATE   44100
#define CITCAUDIO_CHANNELS      2
#define CITCAUDIO_BITS          16
#define CITCAUDIO_FRAME_SIZE    (CITCAUDIO_CHANNELS * (CITCAUDIO_BITS / 8))  /* 4 bytes */

/* 메시지 최대 payload (PCM 데이터 포함)
 * 100ms of 44100Hz stereo 16bit = 17640 bytes + 헤더
 * → 32KB면 충분 */
#define CITCAUDIO_MAX_PAYLOAD   32768

/* ============================================================
 * 메시지 헤더
 * ============================================================
 *
 * CDP와 동일한 형태: type(4) + size(4) + payload.
 */
struct audio_msg_header {
	uint32_t type;    /* 메시지 타입 */
	uint32_t size;    /* payload 크기 (바이트) */
};

/* ============================================================
 * 클라이언트 → 서버: 요청
 * ============================================================ */
enum audio_request {
	/*
	 * 스트림 생성 요청
	 * payload: struct audio_open_stream
	 * 응답: AUDIO_EVT_STREAM_ID
	 */
	AUDIO_REQ_OPEN_STREAM  = 1,

	/*
	 * PCM 데이터 전송
	 * payload: struct audio_write_header + PCM 바이트
	 */
	AUDIO_REQ_WRITE        = 2,

	/*
	 * 스트림 닫기
	 * payload: struct audio_close_stream
	 */
	AUDIO_REQ_CLOSE_STREAM = 3,
};

/* ============================================================
 * 서버 → 클라이언트: 이벤트
 * ============================================================ */
enum audio_event {
	/*
	 * 스트림 ID 전달 (OPEN_STREAM 응답)
	 * payload: struct audio_stream_id
	 */
	AUDIO_EVT_STREAM_ID    = 100,

	/*
	 * 버퍼 여유 알림 — 더 보내도 됨
	 * payload: struct audio_ready
	 */
	AUDIO_EVT_READY        = 101,
};

/* ============================================================
 * Payload 구조체
 * ============================================================ */

/* AUDIO_REQ_OPEN_STREAM */
struct audio_open_stream {
	uint32_t sample_rate;   /* 예: 44100 */
	uint32_t channels;      /* 1=mono, 2=stereo */
	uint32_t bits;          /* 8 또는 16 */
};

/* AUDIO_REQ_WRITE — 가변 길이 PCM 데이터 */
struct audio_write_header {
	uint32_t stream_id;
	uint32_t pcm_size;      /* 뒤따르는 PCM 바이트 수 */
	/* PCM 데이터가 바로 뒤에 따라옴 */
};

/* AUDIO_REQ_CLOSE_STREAM */
struct audio_close_stream {
	uint32_t stream_id;
};

/* AUDIO_EVT_STREAM_ID */
struct audio_stream_id {
	uint32_t stream_id;     /* 0이면 실패 */
};

/* AUDIO_EVT_READY */
struct audio_ready {
	uint32_t stream_id;
	uint32_t available;     /* 버퍼에 남은 공간 (바이트) */
};

/* ============================================================
 * 메시지 송수신 헬퍼
 * ============================================================
 *
 * CDP의 cdp_write_all/cdp_read_all과 동일한 패턴.
 */

static inline int audio_write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return -1;
		p += n;
		remaining -= (size_t)n;
	}
	return 0;
}

static inline int audio_read_all(int fd, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t n = read(fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return -1;
		p += n;
		remaining -= (size_t)n;
	}
	return 0;
}

static inline int audio_send_msg(int sock, uint32_t type,
				 const void *payload, uint32_t payload_size)
{
	struct audio_msg_header hdr;

	hdr.type = type;
	hdr.size = payload_size;

	if (audio_write_all(sock, &hdr, sizeof(hdr)) < 0)
		return -1;
	if (payload_size > 0 && payload) {
		if (audio_write_all(sock, payload, payload_size) < 0)
			return -1;
	}
	return 0;
}

static inline int audio_recv_msg(int sock, uint32_t *type_out,
				 void *payload_out, uint32_t payload_max,
				 uint32_t *payload_size_out)
{
	struct audio_msg_header hdr;

	if (audio_read_all(sock, &hdr, sizeof(hdr)) < 0)
		return -1;

	*type_out = hdr.type;
	if (payload_size_out)
		*payload_size_out = hdr.size;

	if (hdr.size > 0) {
		if (hdr.size > CITCAUDIO_MAX_PAYLOAD)
			return -1; /* 프로토콜 오류 */

		if (hdr.size <= payload_max) {
			if (audio_read_all(sock, payload_out, hdr.size) < 0)
				return -1;
		} else {
			/* 버퍼 작음 → 읽을 수 있는 만큼 읽고 나머지 버림 */
			if (payload_max > 0) {
				if (audio_read_all(sock, payload_out, payload_max) < 0)
					return -1;
			}
			uint32_t skip = hdr.size - payload_max;
			uint8_t discard[256];
			while (skip > 0) {
				uint32_t chunk = skip > 256 ? 256 : skip;
				if (audio_read_all(sock, discard, chunk) < 0)
					return -1;
				skip -= chunk;
			}
		}
	}

	return (int)hdr.type;
}

#endif /* CITCAUDIO_PROTO_H */
