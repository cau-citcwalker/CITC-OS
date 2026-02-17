/*
 * citcaudio — CITC OS 오디오 믹싱 서버
 * ======================================
 *
 * PulseAudio/PipeWire의 교육적 재구현.
 *
 * 오디오 서버란?
 *   여러 앱의 소리를 하나로 섞어서 사운드카드에 출력하는 프로그램.
 *   없으면 한 번에 하나의 앱만 소리를 낼 수 있다.
 *
 * 구조:
 *   [앱1] → PCM 데이터 ──┐
 *   [앱2] → PCM 데이터 ──┤→ [citcaudio] → 믹싱 → /dev/dsp
 *   [앱3] → PCM 데이터 ──┘
 *
 * 믹싱 알고리즘:
 *   1. 각 스트림의 링버퍼에서 N 샘플 읽기
 *   2. 선형 합산: mixed[i] = stream1[i] + stream2[i] + ...
 *   3. 클램핑: if mixed > 32767 → 32767, if mixed < -32768 → -32768
 *   4. 결과를 16-bit signed LE로 /dev/dsp에 write
 *
 * 이벤트 루프:
 *   poll() on { listen_fd, client_fd[0..3], timer_fd }
 *   - 새 연결: accept → client 추가
 *   - 클라이언트 데이터: 프로토콜 메시지 처리
 *   - 타이머: 10ms 주기로 믹싱 + 출력
 *
 * 빌드:
 *   gcc -static -Wall -o citcaudio citcaudio.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>

#include "citcaudio_proto.h"

/* ============================================================
 * OSS (Open Sound System) 상수
 * ============================================================
 *
 * QEMU는 기본적으로 OSS 에뮬레이션을 제공.
 * /dev/dsp를 열어 ioctl로 포맷/채널/샘플레이트 설정 후 write.
 */
#define OSS_SNDCTL_DSP_SETFMT   0xC0045005
#define OSS_SNDCTL_DSP_STEREO   0xC0045003
#define OSS_SNDCTL_DSP_SPEED    0xC0045002
#define OSS_AFMT_S16_LE         0x00000010

/* ============================================================
 * 서버 상수
 * ============================================================ */
#define MAX_CLIENTS       4
#define MAX_STREAMS       8
#define RING_SIZE         (44100 * 4 * 2)  /* 2초 분량 (44100Hz, 16bit, stereo) */
#define MIX_PERIOD_MS     10               /* 믹싱 주기 (ms) */
#define MIX_FRAMES        (CITCAUDIO_SAMPLE_RATE * MIX_PERIOD_MS / 1000) /* 441 frames per period */

/* ============================================================
 * 링 버퍼
 * ============================================================
 *
 * 오디오에서 링(원형) 버퍼는 필수:
 *   생산자(앱)는 뒤에 데이터를 쓰고
 *   소비자(믹서)는 앞에서 데이터를 읽는다.
 *   버퍼 끝에 도달하면 처음으로 돌아감.
 *
 *   [==읽은 영역==|---쓴 데이터---|==빈 공간==]
 *                 ^read           ^write
 */
struct ring_buffer {
	uint8_t data[RING_SIZE];
	uint32_t read_pos;
	uint32_t write_pos;
	uint32_t count;       /* 읽을 수 있는 바이트 수 */
};

static void ring_init(struct ring_buffer *rb)
{
	memset(rb, 0, sizeof(*rb));
}

static uint32_t ring_available(const struct ring_buffer *rb)
{
	return rb->count;
}

static uint32_t ring_free(const struct ring_buffer *rb)
{
	return RING_SIZE - rb->count;
}

static int ring_write(struct ring_buffer *rb, const void *data, uint32_t size)
{
	if (size > ring_free(rb))
		return -1; /* 공간 부족 */

	const uint8_t *src = (const uint8_t *)data;
	uint32_t first = RING_SIZE - rb->write_pos;

	if (first >= size) {
		memcpy(rb->data + rb->write_pos, src, size);
	} else {
		memcpy(rb->data + rb->write_pos, src, first);
		memcpy(rb->data, src + first, size - first);
	}
	rb->write_pos = (rb->write_pos + size) % RING_SIZE;
	rb->count += size;
	return 0;
}

static int ring_read(struct ring_buffer *rb, void *out, uint32_t size)
{
	if (size > ring_available(rb))
		return -1; /* 데이터 부족 */

	uint8_t *dst = (uint8_t *)out;
	uint32_t first = RING_SIZE - rb->read_pos;

	if (first >= size) {
		memcpy(dst, rb->data + rb->read_pos, size);
	} else {
		memcpy(dst, rb->data + rb->read_pos, first);
		memcpy(dst + first, rb->data, size - first);
	}
	rb->read_pos = (rb->read_pos + size) % RING_SIZE;
	rb->count -= size;
	return 0;
}

/* ============================================================
 * 스트림 & 클라이언트
 * ============================================================ */

struct audio_stream {
	int active;
	int client_idx;         /* 소유 클라이언트 인덱스 */
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bits;
	struct ring_buffer ring;
};

struct audio_client {
	int fd;                 /* 소켓 fd, -1이면 비어있음 */
};

/* ============================================================
 * 서버 상태
 * ============================================================ */
static struct {
	int listen_fd;
	int dsp_fd;
	int timer_fd;
	int running;

	struct audio_client clients[MAX_CLIENTS];
	struct audio_stream streams[MAX_STREAMS];
	int next_stream_id;     /* 1부터 시작 (0은 무효) */
} server;

/* ============================================================
 * OSS 디바이스 열기
 * ============================================================ */
static int open_dsp(void)
{
	int fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);

	if (fd < 0) {
		/* /dev/dsp 없으면 /dev/null fallback */
		fd = open("/dev/null", O_WRONLY);
		if (fd >= 0)
			printf("citcaudio: /dev/dsp not available, output → /dev/null\n");
		return fd;
	}

	int fmt = OSS_AFMT_S16_LE;
	ioctl(fd, OSS_SNDCTL_DSP_SETFMT, &fmt);

	int stereo = 1;
	ioctl(fd, OSS_SNDCTL_DSP_STEREO, &stereo);

	int rate = CITCAUDIO_SAMPLE_RATE;
	ioctl(fd, OSS_SNDCTL_DSP_SPEED, &rate);

	/* blocking 모드로 전환 */
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	printf("citcaudio: OSS opened (%dHz, stereo, 16bit)\n",
	       CITCAUDIO_SAMPLE_RATE);
	return fd;
}

/* ============================================================
 * 리스닝 소켓 생성
 * ============================================================
 *
 * 소켓 활성화 지원:
 *   citcinit이 LISTEN_FDS=1 환경변수를 설정하면
 *   fd 3이 이미 listen 상태의 소켓이므로 그대로 사용.
 *   그렇지 않으면 직접 생성.
 */
static int create_listen_socket(void)
{
	/* 소켓 활성화 확인 (citcinit이 전달한 fd) */
	const char *fds = getenv("LISTEN_FDS");
	if (fds && atoi(fds) > 0) {
		printf("citcaudio: using socket-activated fd 3\n");
		return 3;
	}

	/* 이전 소켓 파일 제거 */
	unlink(CITCAUDIO_SOCKET_PATH);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("citcaudio: socket");
		return -1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CITCAUDIO_SOCKET_PATH,
		sizeof(addr.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("citcaudio: bind");
		close(fd);
		return -1;
	}

	if (listen(fd, 4) < 0) {
		perror("citcaudio: listen");
		close(fd);
		return -1;
	}

	printf("citcaudio: listening on %s\n", CITCAUDIO_SOCKET_PATH);
	return fd;
}

/* ============================================================
 * 타이머 fd 생성 (10ms 주기)
 * ============================================================
 *
 * timerfd: Linux 커널의 타이머를 파일로 사용하는 API.
 * poll()과 함께 사용하면 "10ms마다 무언가 하기" 가능.
 */
static int create_timer(void)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (fd < 0) {
		perror("citcaudio: timerfd_create");
		return -1;
	}

	struct itimerspec ts;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = MIX_PERIOD_MS * 1000000L;
	ts.it_value = ts.it_interval;

	if (timerfd_settime(fd, 0, &ts, NULL) < 0) {
		perror("citcaudio: timerfd_settime");
		close(fd);
		return -1;
	}

	return fd;
}

/* ============================================================
 * 클라이언트 관리
 * ============================================================ */
static int find_free_client(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (server.clients[i].fd < 0)
			return i;
	return -1;
}

static void remove_client(int idx)
{
	if (idx < 0 || idx >= MAX_CLIENTS)
		return;

	int fd = server.clients[idx].fd;
	if (fd >= 0) {
		close(fd);
		server.clients[idx].fd = -1;
		printf("citcaudio: client %d disconnected\n", idx);
	}

	/* 해당 클라이언트의 스트림 정리 */
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (server.streams[i].active &&
		    server.streams[i].client_idx == idx) {
			server.streams[i].active = 0;
			printf("citcaudio: stream %d closed (client gone)\n", i + 1);
		}
	}
}

/* ============================================================
 * 메시지 처리
 * ============================================================ */

static void handle_open_stream(int client_idx, const struct audio_open_stream *req)
{
	/* 빈 스트림 찾기 */
	int sid = -1;
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (!server.streams[i].active) {
			sid = i;
			break;
		}
	}

	struct audio_stream_id resp;

	if (sid < 0) {
		/* 스트림 풀 */
		resp.stream_id = 0;
	} else {
		struct audio_stream *s = &server.streams[sid];
		s->active = 1;
		s->client_idx = client_idx;
		s->sample_rate = req->sample_rate;
		s->channels = req->channels;
		s->bits = req->bits;
		ring_init(&s->ring);

		resp.stream_id = (uint32_t)(sid + 1); /* 1-based */
		printf("citcaudio: stream %u opened (%uHz, %uch, %ubit)\n",
		       resp.stream_id, req->sample_rate,
		       req->channels, req->bits);
	}

	audio_send_msg(server.clients[client_idx].fd,
		       AUDIO_EVT_STREAM_ID, &resp, sizeof(resp));
}

static void handle_write(int client_idx, const uint8_t *payload,
			 uint32_t payload_size)
{
	if (payload_size < sizeof(struct audio_write_header))
		return;

	struct audio_write_header wh;
	memcpy(&wh, payload, sizeof(wh));

	if (wh.stream_id == 0 || wh.stream_id > MAX_STREAMS)
		return;

	struct audio_stream *s = &server.streams[wh.stream_id - 1];
	if (!s->active || s->client_idx != client_idx)
		return;

	uint32_t pcm_offset = sizeof(wh);
	uint32_t pcm_avail = payload_size - pcm_offset;
	uint32_t pcm_size = wh.pcm_size < pcm_avail ? wh.pcm_size : pcm_avail;

	if (pcm_size > 0 && ring_free(&s->ring) >= pcm_size) {
		ring_write(&s->ring, payload + pcm_offset, pcm_size);
	}
}

static void handle_close_stream(int client_idx,
				const struct audio_close_stream *req)
{
	if (req->stream_id == 0 || req->stream_id > MAX_STREAMS)
		return;

	struct audio_stream *s = &server.streams[req->stream_id - 1];
	if (s->active && s->client_idx == client_idx) {
		s->active = 0;
		printf("citcaudio: stream %u closed\n", req->stream_id);
	}
}

static void process_client(int client_idx)
{
	int fd = server.clients[client_idx].fd;

	/*
	 * AUDIO_REQ_WRITE는 큰 payload를 가질 수 있으므로
	 * 헤더를 먼저 읽고 payload를 별도로 처리.
	 */
	struct audio_msg_header hdr;
	if (audio_read_all(fd, &hdr, sizeof(hdr)) < 0) {
		remove_client(client_idx);
		return;
	}

	if (hdr.size > CITCAUDIO_MAX_PAYLOAD) {
		remove_client(client_idx);
		return;
	}

	uint8_t payload[CITCAUDIO_MAX_PAYLOAD];
	if (hdr.size > 0) {
		if (audio_read_all(fd, payload, hdr.size) < 0) {
			remove_client(client_idx);
			return;
		}
	}

	switch (hdr.type) {
	case AUDIO_REQ_OPEN_STREAM:
		if (hdr.size >= sizeof(struct audio_open_stream))
			handle_open_stream(client_idx,
				(struct audio_open_stream *)payload);
		break;
	case AUDIO_REQ_WRITE:
		handle_write(client_idx, payload, hdr.size);
		break;
	case AUDIO_REQ_CLOSE_STREAM:
		if (hdr.size >= sizeof(struct audio_close_stream))
			handle_close_stream(client_idx,
				(struct audio_close_stream *)payload);
		break;
	}
}

/* ============================================================
 * 믹싱
 * ============================================================
 *
 * 오디오 믹싱의 핵심:
 *   1. 각 스트림에서 같은 수의 샘플을 읽기
 *   2. int32_t로 합산 (오버플로 방지)
 *   3. int16_t 범위로 클램핑 [-32768, 32767]
 *
 * 왜 int32_t?
 *   int16_t 최대값 = 32767
 *   두 스트림 합 = 최대 65534
 *   int16_t로는 오버플로 → int32_t로 합산 후 클램핑
 */
static void do_mix(void)
{
	/* 44100 * 2ch * 2bytes * 10ms / 1000 = 1764 bytes per period */
	uint32_t bytes_per_period = MIX_FRAMES * CITCAUDIO_FRAME_SIZE;

	int16_t mix_buf[MIX_FRAMES * CITCAUDIO_CHANNELS];
	memset(mix_buf, 0, sizeof(mix_buf));

	int any_active = 0;

	for (int i = 0; i < MAX_STREAMS; i++) {
		struct audio_stream *s = &server.streams[i];
		if (!s->active || ring_available(&s->ring) < bytes_per_period)
			continue;

		int16_t tmp[MIX_FRAMES * CITCAUDIO_CHANNELS];
		ring_read(&s->ring, tmp, bytes_per_period);

		/* 선형 합산 */
		int total_samples = MIX_FRAMES * CITCAUDIO_CHANNELS;
		for (int j = 0; j < total_samples; j++) {
			int32_t sum = (int32_t)mix_buf[j] + (int32_t)tmp[j];
			/* 클램핑 */
			if (sum > 32767) sum = 32767;
			if (sum < -32768) sum = -32768;
			mix_buf[j] = (int16_t)sum;
		}

		any_active = 1;
	}

	/* 하드웨어에 출력 */
	if (any_active && server.dsp_fd >= 0) {
		write(server.dsp_fd, mix_buf, bytes_per_period);
	}
}

/* ============================================================
 * 시그널 핸들러
 * ============================================================ */
static void signal_handler(int sig)
{
	(void)sig;
	server.running = 0;
}

/* ============================================================
 * 메인 이벤트 루프
 * ============================================================ */
int main(void)
{
	/* static 바이너리에서 stdout 버퍼링 방지 */
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("\n=== CITC Audio Server (citcaudio) ===\n\n");

	/* 시그널 설정 */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, SIG_IGN);  /* 끊어진 소켓에 write해도 크래시 방지 */

	/* 초기화 */
	server.running = 1;
	server.next_stream_id = 1;

	for (int i = 0; i < MAX_CLIENTS; i++)
		server.clients[i].fd = -1;
	for (int i = 0; i < MAX_STREAMS; i++)
		server.streams[i].active = 0;

	/* OSS 디바이스 열기 */
	server.dsp_fd = open_dsp();
	if (server.dsp_fd < 0) {
		fprintf(stderr, "citcaudio: cannot open audio device\n");
		return 1;
	}

	/* 리스닝 소켓 */
	server.listen_fd = create_listen_socket();
	if (server.listen_fd < 0) {
		close(server.dsp_fd);
		return 1;
	}

	/* 10ms 타이머 */
	server.timer_fd = create_timer();
	if (server.timer_fd < 0) {
		close(server.listen_fd);
		close(server.dsp_fd);
		return 1;
	}

	printf("citcaudio: ready (mix period = %dms, %d frames)\n",
	       MIX_PERIOD_MS, MIX_FRAMES);

	/*
	 * poll() 배열:
	 *   [0] = listen_fd (새 연결)
	 *   [1] = timer_fd (믹싱 트리거)
	 *   [2..5] = client_fd[0..3]
	 */
	while (server.running) {
		struct pollfd fds[2 + MAX_CLIENTS];
		int nfds = 0;

		/* listen */
		fds[nfds].fd = server.listen_fd;
		fds[nfds].events = POLLIN;
		fds[nfds].revents = 0;
		int listen_idx = nfds++;

		/* timer */
		fds[nfds].fd = server.timer_fd;
		fds[nfds].events = POLLIN;
		fds[nfds].revents = 0;
		int timer_idx = nfds++;

		/* clients */
		int client_poll_map[MAX_CLIENTS];
		for (int i = 0; i < MAX_CLIENTS; i++) {
			client_poll_map[i] = -1;
			if (server.clients[i].fd >= 0) {
				client_poll_map[i] = nfds;
				fds[nfds].fd = server.clients[i].fd;
				fds[nfds].events = POLLIN;
				fds[nfds].revents = 0;
				nfds++;
			}
		}

		int ret = poll(fds, (nfds_t)nfds, 100);
		if (ret < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (ret == 0) continue;

		/* 새 연결 */
		if (fds[listen_idx].revents & POLLIN) {
			int cli_fd = accept(server.listen_fd, NULL, NULL);
			if (cli_fd >= 0) {
				int idx = find_free_client();
				if (idx >= 0) {
					server.clients[idx].fd = cli_fd;
					printf("citcaudio: client %d connected\n", idx);
				} else {
					/* 슬롯 없음 */
					close(cli_fd);
					printf("citcaudio: rejected client (full)\n");
				}
			}
		}

		/* 타이머 만료 → 믹싱 */
		if (fds[timer_idx].revents & POLLIN) {
			uint64_t expirations;
			read(server.timer_fd, &expirations, sizeof(expirations));
			do_mix();
		}

		/* 클라이언트 데이터 */
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (client_poll_map[i] < 0) continue;
			if (fds[client_poll_map[i]].revents & (POLLIN | POLLHUP)) {
				process_client(i);
			}
		}
	}

	/* 정리 */
	printf("\ncitcaudio: shutting down\n");

	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (server.clients[i].fd >= 0)
			close(server.clients[i].fd);
	}

	close(server.timer_fd);
	close(server.listen_fd);
	close(server.dsp_fd);
	unlink(CITCAUDIO_SOCKET_PATH);

	printf("citcaudio: done\n");
	return 0;
}
