/*
 * beep — 네이티브 비프 도구
 * =========================
 *
 * citcaudio 서버를 사용하여 사인파 톤을 재생.
 * 네이티브 Linux 앱이 citcaudio를 사용하는 데모.
 *
 * 사용법:
 *   beep                     — 440Hz 500ms (기본)
 *   beep <frequency>         — 지정 주파수 500ms
 *   beep <frequency> <ms>    — 지정 주파수 지정 시간
 *
 * 예시:
 *   beep 440 1000    — A4 (라) 1초
 *   beep 261 500     — C4 (도) 0.5초
 *   beep 880 200     — A5 (라↑) 0.2초
 *
 * 빌드:
 *   gcc -static -Wall -o beep beep.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "citcaudio_client.h"

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BITS          16
#define AMPLITUDE     16000

int main(int argc, char *argv[])
{
	int frequency = 440;
	int duration_ms = 500;

	if (argc >= 2)
		frequency = atoi(argv[1]);
	if (argc >= 3)
		duration_ms = atoi(argv[2]);

	if (frequency <= 0 || frequency > 20000) {
		fprintf(stderr, "beep: frequency must be 1-20000 Hz\n");
		return 1;
	}
	if (duration_ms <= 0 || duration_ms > 10000) {
		fprintf(stderr, "beep: duration must be 1-10000 ms\n");
		return 1;
	}

	/* citcaudio 서버 연결 */
	int fd = citcaudio_connect();
	if (fd < 0) {
		fprintf(stderr, "beep: cannot connect to citcaudio\n");
		return 1;
	}

	uint32_t sid = citcaudio_open_stream(fd, SAMPLE_RATE, CHANNELS, BITS);
	if (sid == 0) {
		fprintf(stderr, "beep: cannot open stream\n");
		close(fd);
		return 1;
	}

	/* 사인파 생성 + 전송 */
	int total_frames = SAMPLE_RATE * duration_ms / 1000;
	int chunk_frames = SAMPLE_RATE / 20; /* 50ms 청크 */
	int16_t buf[chunk_frames * CHANNELS];

	for (int f = 0; f < total_frames; f += chunk_frames) {
		int frames = chunk_frames;
		if (f + frames > total_frames)
			frames = total_frames - f;

		for (int i = 0; i < frames; i++) {
			double t = (double)(f + i) / SAMPLE_RATE;
			int16_t sample = (int16_t)(AMPLITUDE *
				sin(2.0 * M_PI * frequency * t));
			buf[i * 2] = sample;
			buf[i * 2 + 1] = sample;
		}

		uint32_t pcm_bytes = (uint32_t)(frames * CHANNELS *
						sizeof(int16_t));
		if (citcaudio_write(fd, sid, buf, pcm_bytes) < 0)
			break;

		usleep(45000); /* ~45ms (50ms 청크보다 약간 빠르게) */
	}

	citcaudio_close_stream(fd, sid);
	close(fd);

	return 0;
}
