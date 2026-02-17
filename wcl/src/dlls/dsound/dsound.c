/*
 * dsound.c — DirectSound 8 구현
 * ================================
 *
 * IDirectSound8 + IDirectSoundBuffer8 COM 인터페이스.
 * 백엔드: citcaudio 서버 (우선) 또는 OSS /dev/dsp (fallback).
 *
 * 구조:
 *   DirectSoundCreate8() → IDirectSound8 (COM vtable)
 *     → CreateSoundBuffer() → IDirectSoundBuffer8 (COM vtable)
 *       → Lock() / Unlock() — 링 버퍼 접근
 *       → Play() / Stop() — 백그라운드 스레드
 *       → SetFormat() — PCM 파라미터 설정
 *
 * 오디오 출력 (우선순위):
 *   1. citcaudio 서버 → 다중 앱 오디오 지원
 *   2. /dev/dsp (OSS) → citcaudio 없을 때 직접 접근
 *   3. /dev/null → 오디오 디바이스 없을 때
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "../../../include/win32.h"
#include "../../../include/stub_entry.h"
#include "../../../../audio/src/citcaudio_client.h"

/* ============================================================
 * OSS 오디오 백엔드 (citcaudio fallback)
 * ============================================================ */

/* OSS ioctl 상수 (sys/soundcard.h 대신 직접 정의) */
#define OSS_SNDCTL_DSP_RESET     0x5000
#define OSS_SNDCTL_DSP_SPEED     0xC0045002
#define OSS_SNDCTL_DSP_STEREO    0xC0045003
#define OSS_SNDCTL_DSP_SETFMT    0xC0045005
#define OSS_AFMT_S16_LE          0x00000010

#include <sys/ioctl.h>

static int oss_open(int sample_rate, int channels, int bits_per_sample)
{
	int fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		/* QEMU에서 OSS 없으면 /dev/null fallback */
		fd = open("/dev/null", O_WRONLY);
		if (fd >= 0)
			printf("dsound: /dev/dsp not available, audio → /dev/null\n");
		return fd;
	}

	/* 포맷 설정 */
	int fmt = OSS_AFMT_S16_LE;
	ioctl(fd, OSS_SNDCTL_DSP_SETFMT, &fmt);
	(void)bits_per_sample; /* 항상 16-bit */

	int stereo = (channels > 1) ? 1 : 0;
	ioctl(fd, OSS_SNDCTL_DSP_STEREO, &stereo);

	int rate = sample_rate;
	ioctl(fd, OSS_SNDCTL_DSP_SPEED, &rate);

	/* blocking 모드로 전환 */
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	printf("dsound: OSS opened (%dHz, %dch, %dbit)\n",
	       sample_rate, channels, bits_per_sample);
	return fd;
}

/* ============================================================
 * IDirectSoundBuffer8 구현
 * ============================================================ */

struct ds_buffer;

typedef struct IDirectSoundBuffer8Vtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	/* IDirectSoundBuffer */
	HRESULT (__attribute__((ms_abi)) *GetCaps)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *GetCurrentPosition)(void *, DWORD *, DWORD *);
	HRESULT (__attribute__((ms_abi)) *GetFormat)(void *, WAVEFORMATEX *, DWORD, DWORD *);
	HRESULT (__attribute__((ms_abi)) *GetVolume)(void *, long *);
	HRESULT (__attribute__((ms_abi)) *GetPan)(void *, long *);
	HRESULT (__attribute__((ms_abi)) *GetFrequency)(void *, DWORD *);
	HRESULT (__attribute__((ms_abi)) *GetStatus)(void *, DWORD *);
	HRESULT (__attribute__((ms_abi)) *Initialize)(void *, void *, void *);
	HRESULT (__attribute__((ms_abi)) *Lock)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
	HRESULT (__attribute__((ms_abi)) *Play)(void *, DWORD, DWORD, DWORD);
	HRESULT (__attribute__((ms_abi)) *SetCurrentPosition)(void *, DWORD);
	HRESULT (__attribute__((ms_abi)) *SetFormat)(void *, const WAVEFORMATEX *);
	HRESULT (__attribute__((ms_abi)) *SetVolume)(void *, long);
	HRESULT (__attribute__((ms_abi)) *SetPan)(void *, long);
	HRESULT (__attribute__((ms_abi)) *SetFrequency)(void *, DWORD);
	HRESULT (__attribute__((ms_abi)) *Stop)(void *);
	HRESULT (__attribute__((ms_abi)) *Unlock)(void *, void *, DWORD, void *, DWORD);
	HRESULT (__attribute__((ms_abi)) *Restore)(void *);
} IDirectSoundBuffer8Vtbl;

struct ds_buffer {
	IDirectSoundBuffer8Vtbl *lpVtbl;
	ULONG ref_count;

	/* PCM 설정 */
	int sample_rate;
	int channels;
	int bits_per_sample;

	/* 링 버퍼 */
	uint8_t *data;
	DWORD size;
	DWORD write_cursor;
	DWORD play_cursor;

	/* 오디오 백엔드 */
	int audio_fd;           /* OSS fd (fallback) 또는 -1 */
	int citcaudio_fd;       /* citcaudio 서버 소켓 또는 -1 */
	uint32_t stream_id;     /* citcaudio 스트림 ID */

	/* 재생 스레드 */
	pthread_t play_thread;
	int playing;
	int looping;
};

static HRESULT __attribute__((ms_abi))
buf_QueryInterface(void *This, REFIID riid, void **ppv)
{ (void)riid; if (ppv) *ppv = This; return S_OK; }

static ULONG __attribute__((ms_abi))
buf_AddRef(void *This)
{ struct ds_buffer *b = This; return ++b->ref_count; }

static ULONG __attribute__((ms_abi))
buf_Release(void *This)
{
	struct ds_buffer *b = This;
	ULONG r = --b->ref_count;
	if (r == 0) {
		b->playing = 0;
		if (b->play_thread) {
			pthread_join(b->play_thread, NULL);
			b->play_thread = 0;
		}
		if (b->citcaudio_fd >= 0) {
			if (b->stream_id > 0)
				citcaudio_close_stream(b->citcaudio_fd,
						       b->stream_id);
			close(b->citcaudio_fd);
		}
		if (b->audio_fd >= 0)
			close(b->audio_fd);
		free(b->data);
		free(b);
	}
	return r;
}

static HRESULT __attribute__((ms_abi))
buf_GetCaps(void *T, void *c) { (void)T; (void)c; return DS_OK; }

static HRESULT __attribute__((ms_abi))
buf_GetCurrentPosition(void *This, DWORD *pPlay, DWORD *pWrite)
{
	struct ds_buffer *b = This;
	if (pPlay) *pPlay = b->play_cursor;
	if (pWrite) *pWrite = b->write_cursor;
	return DS_OK;
}

static HRESULT __attribute__((ms_abi))
buf_GetFormat(void *T, WAVEFORMATEX *f, DWORD s, DWORD *w)
{ (void)T; (void)f; (void)s; (void)w; return DS_OK; }

static HRESULT __attribute__((ms_abi))
buf_stub_hr(void *T, ...) { (void)T; return DS_OK; }

static HRESULT __attribute__((ms_abi))
buf_Lock(void *This, DWORD dwOffset, DWORD dwBytes,
         void **ppAudio1, DWORD *pdwAudioBytes1,
         void **ppAudio2, DWORD *pdwAudioBytes2, DWORD dwFlags)
{
	struct ds_buffer *b = This;
	(void)dwFlags;

	if (!b->data || !ppAudio1 || !pdwAudioBytes1)
		return DSERR_GENERIC;

	/* 단순 구현: 한 세그먼트만 반환 (wrap-around 미지원) */
	DWORD off = dwOffset % b->size;
	DWORD avail = b->size - off;
	DWORD len1 = dwBytes < avail ? dwBytes : avail;

	*ppAudio1 = b->data + off;
	*pdwAudioBytes1 = len1;

	if (ppAudio2) {
		if (len1 < dwBytes) {
			*ppAudio2 = b->data;
			if (pdwAudioBytes2)
				*pdwAudioBytes2 = dwBytes - len1;
		} else {
			*ppAudio2 = NULL;
			if (pdwAudioBytes2) *pdwAudioBytes2 = 0;
		}
	}

	return DS_OK;
}

static void oss_write(int fd, const void *buf, size_t len)
{
	ssize_t r = write(fd, buf, len);
	(void)r; /* OSS write 실패는 무시 (오디오 끊김은 허용) */
}

static void audio_output(struct ds_buffer *b, const void *buf, size_t len)
{
	if (b->citcaudio_fd >= 0 && b->stream_id > 0) {
		citcaudio_write(b->citcaudio_fd, b->stream_id,
				buf, (uint32_t)len);
	} else if (b->audio_fd >= 0) {
		oss_write(b->audio_fd, buf, len);
	}
}

static void *play_thread_func(void *arg)
{
	struct ds_buffer *b = arg;
	int block = b->channels * (b->bits_per_sample / 8);
	int chunk = b->sample_rate * block / 50; /* ~20ms */
	if (chunk > (int)b->size) chunk = (int)b->size;

	while (b->playing) {
		DWORD pos = b->play_cursor;
		DWORD end = pos + (DWORD)chunk;

		if (end <= b->size) {
			audio_output(b, b->data + pos, (size_t)chunk);
			b->play_cursor = end;
		} else {
			/* wrap */
			DWORD first = b->size - pos;
			audio_output(b, b->data + pos, first);
			audio_output(b, b->data, (size_t)(chunk - first));
			b->play_cursor = (DWORD)(chunk - first);
		}

		if (b->play_cursor >= b->size) {
			if (b->looping)
				b->play_cursor = 0;
			else {
				b->playing = 0;
				break;
			}
		}

		usleep(20000); /* 20ms */
	}
	return NULL;
}

static HRESULT __attribute__((ms_abi))
buf_Play(void *This, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags)
{
	struct ds_buffer *b = This;
	(void)dwReserved1; (void)dwPriority;

	if (b->playing) return DS_OK;

	/* 오디오 백엔드 연결 (아직 안 열었으면) */
	if (b->citcaudio_fd < 0 && b->audio_fd < 0) {
		/* citcaudio 서버 시도 (다중 앱 지원) */
		b->citcaudio_fd = citcaudio_connect();
		if (b->citcaudio_fd >= 0) {
			b->stream_id = citcaudio_open_stream(
				b->citcaudio_fd,
				(uint32_t)b->sample_rate,
				(uint32_t)b->channels,
				(uint32_t)b->bits_per_sample);
			if (b->stream_id > 0) {
				printf("dsound: citcaudio connected (stream %u)\n",
				       b->stream_id);
			} else {
				/* 스트림 열기 실패 → fallback */
				close(b->citcaudio_fd);
				b->citcaudio_fd = -1;
			}
		}

		/* citcaudio 실패 → OSS 직접 접근 */
		if (b->citcaudio_fd < 0) {
			b->audio_fd = oss_open(b->sample_rate, b->channels,
					       b->bits_per_sample);
		}
	}

	b->looping = (dwFlags & DSBPLAY_LOOPING) ? 1 : 0;
	b->playing = 1;
	b->play_cursor = 0;

	pthread_create(&b->play_thread, NULL, play_thread_func, b);
	return DS_OK;
}

static HRESULT __attribute__((ms_abi))
buf_Stop(void *This)
{
	struct ds_buffer *b = This;
	b->playing = 0;
	if (b->play_thread) {
		pthread_join(b->play_thread, NULL);
		b->play_thread = 0;
	}
	return DS_OK;
}

static HRESULT __attribute__((ms_abi))
buf_Unlock(void *This, void *p1, DWORD n1, void *p2, DWORD n2)
{
	struct ds_buffer *b = This;
	(void)p1; (void)p2;
	b->write_cursor = (b->write_cursor + n1 + n2) % b->size;
	return DS_OK;
}

static HRESULT __attribute__((ms_abi))
buf_SetFormat(void *This, const WAVEFORMATEX *wfx)
{
	struct ds_buffer *b = This;
	if (!wfx) return DSERR_GENERIC;
	b->sample_rate = (int)wfx->nSamplesPerSec;
	b->channels = (int)wfx->nChannels;
	b->bits_per_sample = (int)wfx->wBitsPerSample;
	return DS_OK;
}

static HRESULT __attribute__((ms_abi))
buf_SetCurrentPosition(void *This, DWORD p)
{ struct ds_buffer *b = This; b->play_cursor = p % b->size; return DS_OK; }

static HRESULT __attribute__((ms_abi))
buf_Restore(void *T) { (void)T; return DS_OK; }

static IDirectSoundBuffer8Vtbl g_dsbuf_vtbl = {
	.QueryInterface    = buf_QueryInterface,
	.AddRef            = buf_AddRef,
	.Release           = buf_Release,
	.GetCaps           = buf_GetCaps,
	.GetCurrentPosition = buf_GetCurrentPosition,
	.GetFormat          = buf_GetFormat,
	.GetVolume         = (void *)buf_stub_hr,
	.GetPan            = (void *)buf_stub_hr,
	.GetFrequency      = (void *)buf_stub_hr,
	.GetStatus         = (void *)buf_stub_hr,
	.Initialize        = (void *)buf_stub_hr,
	.Lock              = buf_Lock,
	.Play              = buf_Play,
	.SetCurrentPosition = buf_SetCurrentPosition,
	.SetFormat         = buf_SetFormat,
	.SetVolume         = (void *)buf_stub_hr,
	.SetPan            = (void *)buf_stub_hr,
	.SetFrequency      = (void *)buf_stub_hr,
	.Stop              = buf_Stop,
	.Unlock            = buf_Unlock,
	.Restore           = buf_Restore,
};

/* ============================================================
 * IDirectSound8 구현
 * ============================================================ */

struct ds_device;

typedef struct IDirectSound8Vtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	/* IDirectSound */
	HRESULT (__attribute__((ms_abi)) *CreateSoundBuffer)(void *, const DSBUFFERDESC *, void **, void *);
	HRESULT (__attribute__((ms_abi)) *GetCaps)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *DuplicateSoundBuffer)(void *, void *, void **);
	HRESULT (__attribute__((ms_abi)) *SetCooperativeLevel)(void *, void *, DWORD);
	HRESULT (__attribute__((ms_abi)) *Compact)(void *);
	HRESULT (__attribute__((ms_abi)) *GetSpeakerConfig)(void *, DWORD *);
	HRESULT (__attribute__((ms_abi)) *SetSpeakerConfig)(void *, DWORD);
	HRESULT (__attribute__((ms_abi)) *Initialize)(void *, void *);
} IDirectSound8Vtbl;

struct ds_device {
	IDirectSound8Vtbl *lpVtbl;
	ULONG ref_count;
};

static HRESULT __attribute__((ms_abi))
ds_QueryInterface(void *This, REFIID riid, void **ppv)
{ (void)riid; if (ppv) *ppv = This; return S_OK; }

static ULONG __attribute__((ms_abi))
ds_AddRef(void *This)
{ struct ds_device *d = This; return ++d->ref_count; }

static ULONG __attribute__((ms_abi))
ds_Release(void *This)
{
	struct ds_device *d = This;
	ULONG r = --d->ref_count;
	if (r == 0) free(d);
	return r;
}

static HRESULT __attribute__((ms_abi))
ds_CreateSoundBuffer(void *This, const DSBUFFERDESC *desc,
                     void **ppDSBuffer, void *pUnkOuter)
{
	(void)This; (void)pUnkOuter;
	if (!desc || !ppDSBuffer) return DSERR_GENERIC;

	struct ds_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) return E_OUTOFMEMORY;

	buf->lpVtbl = &g_dsbuf_vtbl;
	buf->ref_count = 1;
	buf->audio_fd = -1;
	buf->citcaudio_fd = -1;
	buf->stream_id = 0;

	/* 기본 포맷 */
	buf->sample_rate = 44100;
	buf->channels = 2;
	buf->bits_per_sample = 16;

	if (desc->lpwfxFormat) {
		buf->sample_rate = (int)desc->lpwfxFormat->nSamplesPerSec;
		buf->channels = (int)desc->lpwfxFormat->nChannels;
		buf->bits_per_sample = (int)desc->lpwfxFormat->wBitsPerSample;
	}

	/* 버퍼 할당 */
	buf->size = desc->dwBufferBytes;
	if (buf->size == 0) buf->size = 44100 * 4; /* 1초 기본 */
	buf->data = calloc(1, buf->size);
	if (!buf->data) { free(buf); return E_OUTOFMEMORY; }

	printf("dsound: buffer created (%u bytes, %dHz, %dch, %dbit)\n",
	       (unsigned)buf->size, buf->sample_rate,
	       buf->channels, buf->bits_per_sample);

	*ppDSBuffer = buf;
	return DS_OK;
}

static HRESULT __attribute__((ms_abi))
ds_SetCooperativeLevel(void *This, void *hwnd, DWORD dwLevel)
{ (void)This; (void)hwnd; (void)dwLevel; return DS_OK; }

static HRESULT __attribute__((ms_abi))
ds_stub_hr(void *T, ...) { (void)T; return DS_OK; }

static IDirectSound8Vtbl g_ds_vtbl = {
	.QueryInterface      = ds_QueryInterface,
	.AddRef              = ds_AddRef,
	.Release             = ds_Release,
	.CreateSoundBuffer   = ds_CreateSoundBuffer,
	.GetCaps             = (void *)ds_stub_hr,
	.DuplicateSoundBuffer = (void *)ds_stub_hr,
	.SetCooperativeLevel = ds_SetCooperativeLevel,
	.Compact             = (void *)ds_stub_hr,
	.GetSpeakerConfig    = (void *)ds_stub_hr,
	.SetSpeakerConfig    = (void *)ds_stub_hr,
	.Initialize          = (void *)ds_stub_hr,
};

/* ============================================================
 * DLL 엔트리: DirectSoundCreate8
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
dsound_DirectSoundCreate8(void *lpGuid, void **ppDS8, void *pUnkOuter)
{
	(void)lpGuid; (void)pUnkOuter;
	if (!ppDS8) return DSERR_GENERIC;

	struct ds_device *dev = calloc(1, sizeof(*dev));
	if (!dev) return E_OUTOFMEMORY;

	dev->lpVtbl = &g_ds_vtbl;
	dev->ref_count = 1;

	printf("dsound: DirectSoundCreate8 OK\n");
	*ppDS8 = dev;
	return DS_OK;
}

/* DirectSoundCreate (non-8) — 같은 구현 */
static HRESULT __attribute__((ms_abi))
dsound_DirectSoundCreate(void *lpGuid, void **ppDS, void *pUnkOuter)
{
	return dsound_DirectSoundCreate8(lpGuid, ppDS, pUnkOuter);
}

/* DirectSoundEnumerateA — 기본 장치만 */
static HRESULT __attribute__((ms_abi))
dsound_DirectSoundEnumerateA(void *pDSEnumCallback, void *pContext)
{
	(void)pDSEnumCallback; (void)pContext;
	/* 콜백 호출 생략 — 앱이 NULL 체크만 하는 경우가 대부분 */
	return DS_OK;
}

struct stub_entry dsound_stub_table[] = {
	{ "dsound.dll", "DirectSoundCreate8",
	  (void *)dsound_DirectSoundCreate8 },
	{ "dsound.dll", "DirectSoundCreate",
	  (void *)dsound_DirectSoundCreate },
	{ "dsound.dll", "DirectSoundEnumerateA",
	  (void *)dsound_DirectSoundEnumerateA },
	{ NULL, NULL, NULL }
};
