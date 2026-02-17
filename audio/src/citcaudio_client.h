/*
 * CITC Audio Client — 클라이언트 라이브러리
 * ==========================================
 *
 * CDP 클라이언트(cdp_client.h)와 동일한 패턴의 header-only 라이브러리.
 *
 * 사용법:
 *   #include "citcaudio_client.h"
 *
 *   int fd = citcaudio_connect();
 *   uint32_t sid = citcaudio_open_stream(fd, 44100, 2, 16);
 *   citcaudio_write(fd, sid, pcm_data, pcm_size);
 *   citcaudio_close_stream(fd, sid);
 *   close(fd);
 */

#ifndef CITCAUDIO_CLIENT_H
#define CITCAUDIO_CLIENT_H

#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>

#include "citcaudio_proto.h"

/*
 * 오디오 서버에 연결
 *
 * 반환: 소켓 fd (>= 0), 실패 시 -1
 */
static inline int citcaudio_connect(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CITCAUDIO_SOCKET_PATH,
		sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

/*
 * 오디오 스트림 열기
 *
 * 반환: stream_id (> 0), 실패 시 0
 */
static inline uint32_t citcaudio_open_stream(int fd, uint32_t sample_rate,
					     uint32_t channels, uint32_t bits)
{
	struct audio_open_stream req;
	req.sample_rate = sample_rate;
	req.channels = channels;
	req.bits = bits;

	if (audio_send_msg(fd, AUDIO_REQ_OPEN_STREAM,
			   &req, sizeof(req)) < 0)
		return 0;

	/* 응답 대기: AUDIO_EVT_STREAM_ID */
	uint32_t type;
	struct audio_stream_id resp;
	uint32_t resp_size;

	if (audio_recv_msg(fd, &type, &resp, sizeof(resp), &resp_size) < 0)
		return 0;
	if (type != AUDIO_EVT_STREAM_ID)
		return 0;

	return resp.stream_id;
}

/*
 * PCM 데이터 전송
 *
 * pcm: 16-bit signed LE PCM 데이터
 * pcm_size: 바이트 수
 *
 * 반환: 0 성공, -1 실패
 */
static inline int citcaudio_write(int fd, uint32_t stream_id,
				  const void *pcm, uint32_t pcm_size)
{
	/*
	 * audio_write_header + PCM 데이터를 하나의 메시지로 전송.
	 * payload = write_header(8 bytes) + pcm_data(pcm_size bytes)
	 */
	struct audio_write_header wh;
	wh.stream_id = stream_id;
	wh.pcm_size = pcm_size;

	uint32_t total_payload = sizeof(wh) + pcm_size;
	struct audio_msg_header hdr;
	hdr.type = AUDIO_REQ_WRITE;
	hdr.size = total_payload;

	if (audio_write_all(fd, &hdr, sizeof(hdr)) < 0)
		return -1;
	if (audio_write_all(fd, &wh, sizeof(wh)) < 0)
		return -1;
	if (pcm_size > 0) {
		if (audio_write_all(fd, pcm, pcm_size) < 0)
			return -1;
	}
	return 0;
}

/*
 * 스트림 닫기
 */
static inline int citcaudio_close_stream(int fd, uint32_t stream_id)
{
	struct audio_close_stream req;
	req.stream_id = stream_id;
	return audio_send_msg(fd, AUDIO_REQ_CLOSE_STREAM,
			      &req, sizeof(req));
}

#endif /* CITCAUDIO_CLIENT_H */
