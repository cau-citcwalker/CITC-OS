/*
 * audio_test.c — citcaudio 서버 테스트
 * ======================================
 *
 * 440Hz 사인파를 1초간 재생하여 오디오 서버를 검증.
 *
 * 사인파 생성:
 *   sample[t] = amplitude * sin(2π * frequency * t / sample_rate)
 *
 *   440Hz = 음악에서 A4 (라) 음.
 *   피아노의 가운데 "라" 건반.
 *
 * 사용법:
 *   1. citcaudio 서버 시작: ./citcaudio &
 *   2. 테스트 실행: ./audio_test
 *
 * 빌드:
 *   gcc -static -Wall -o audio_test audio_test.c -lm
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "citcaudio_client.h"

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BITS          16
#define FREQUENCY     440    /* A4 = 440Hz */
#define DURATION_MS   1000   /* 1초 */
#define AMPLITUDE     16000  /* 최대 32767의 약 절반 */

/* 테스트에 사용할 프레임 수 */
#define TOTAL_FRAMES  (SAMPLE_RATE * DURATION_MS / 1000)

int main(void)
{
	int pass = 0, fail = 0;

	/* static 바이너리에서 stdout 버퍼링 방지 */
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("\n=== CITC Audio Test ===\n\n");

	/* [1] 서버 연결 */
	int fd = citcaudio_connect();
	if (fd >= 0) {
		printf("  [1] Connect to citcaudio ... PASS\n");
		pass++;
	} else {
		printf("  [1] Connect to citcaudio ... FAIL (server not running?)\n");
		fail++;
		printf("\n--- audio_test: %d/%d PASS ---\n\n", pass, pass + fail);
		return 1;
	}

	/* [2] 스트림 열기 */
	uint32_t sid = citcaudio_open_stream(fd, SAMPLE_RATE, CHANNELS, BITS);
	if (sid > 0) {
		printf("  [2] Open stream (44100Hz, stereo, 16bit) → id=%u ... PASS\n", sid);
		pass++;
	} else {
		printf("  [2] Open stream ... FAIL\n");
		fail++;
		close(fd);
		printf("\n--- audio_test: %d/%d PASS ---\n\n", pass, pass + fail);
		return 1;
	}

	/* [3] 사인파 생성 + 전송 */
	{
		/*
		 * PCM 데이터를 청크 단위로 전송.
		 * 한 번에 보내는 양: 4410 frames (100ms 분량)
		 */
		#define CHUNK_FRAMES  4410
		int16_t buf[CHUNK_FRAMES * CHANNELS];
		int total_sent = 0;

		for (int f = 0; f < TOTAL_FRAMES; f += CHUNK_FRAMES) {
			int frames = CHUNK_FRAMES;
			if (f + frames > TOTAL_FRAMES)
				frames = TOTAL_FRAMES - f;

			for (int i = 0; i < frames; i++) {
				double t = (double)(f + i) / SAMPLE_RATE;
				int16_t sample = (int16_t)(AMPLITUDE *
					sin(2.0 * M_PI * FREQUENCY * t));
				/* stereo: 양쪽 동일 */
				buf[i * 2] = sample;
				buf[i * 2 + 1] = sample;
			}

			uint32_t pcm_bytes = (uint32_t)(frames * CHANNELS * sizeof(int16_t));
			if (citcaudio_write(fd, sid, buf, pcm_bytes) < 0) {
				printf("  [3] Write PCM data ... FAIL (write error at frame %d)\n", f);
				fail++;
				goto done;
			}
			total_sent += frames;

			/* 서버가 처리할 시간을 줌 (100ms 분량 전송 후 짧은 대기) */
			usleep(90000);  /* 90ms */
		}

		if (total_sent >= TOTAL_FRAMES) {
			printf("  [3] Write 440Hz sine (%d frames) ... PASS\n", total_sent);
			pass++;
		} else {
			printf("  [3] Write PCM data ... FAIL (only %d/%d frames)\n",
			       total_sent, TOTAL_FRAMES);
			fail++;
		}
	}

	/* [4] 스트림 닫기 */
done:
	if (citcaudio_close_stream(fd, sid) == 0) {
		printf("  [4] Close stream ... PASS\n");
		pass++;
	} else {
		printf("  [4] Close stream ... FAIL\n");
		fail++;
	}

	/* [5] 연결 해제 */
	close(fd);
	printf("  [5] Disconnect ... PASS\n");
	pass++;

	printf("\n--- audio_test: %d/%d PASS ---\n\n", pass, pass + fail);
	return fail > 0 ? 1 : 0;
}
