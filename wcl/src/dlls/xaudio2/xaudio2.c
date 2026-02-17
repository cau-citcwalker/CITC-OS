/*
 * xaudio2.c — XAudio2 구현
 * ==========================
 *
 * XAudio2Create → IXAudio2 COM 인터페이스.
 * 백엔드: citcaudio 서버 (우선) 또는 OSS /dev/dsp (fallback).
 *
 * 구현:
 *   XAudio2Create() → IXAudio2 (CreateMasteringVoice, CreateSourceVoice)
 *   IXAudio2MasteringVoice → 스텁
 *   IXAudio2SourceVoice → SubmitSourceBuffer → citcaudio PCM 전송
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../include/win32.h"
#include "../../../include/stub_entry.h"
#include "../../../../audio/src/citcaudio_client.h"

/* ============================================================
 * IXAudio2SourceVoice 스텁
 * ============================================================ */

typedef struct IXAudio2SourceVoiceVtbl {
	/* 최소 vtable 슬롯 (실제 XAudio2는 더 많음) */
	void (__attribute__((ms_abi)) *GetVoiceDetails)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *SetOutputVoices)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *SetEffectChain)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *EnableEffect)(void *, uint32_t, uint32_t);
	HRESULT (__attribute__((ms_abi)) *DisableEffect)(void *, uint32_t, uint32_t);
	void    (__attribute__((ms_abi)) *GetEffectState)(void *, uint32_t, int *);
	HRESULT (__attribute__((ms_abi)) *SetEffectParameters)(void *, uint32_t, const void *, uint32_t, uint32_t);
	HRESULT (__attribute__((ms_abi)) *GetEffectParameters)(void *, uint32_t, void *, uint32_t);
	HRESULT (__attribute__((ms_abi)) *SetFilterParameters)(void *, const void *, uint32_t);
	void    (__attribute__((ms_abi)) *GetFilterParameters)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *SetOutputFilterParameters)(void *, void *, const void *, uint32_t);
	void    (__attribute__((ms_abi)) *GetOutputFilterParameters)(void *, void *, void *);
	HRESULT (__attribute__((ms_abi)) *SetVolume)(void *, float, uint32_t);
	void    (__attribute__((ms_abi)) *GetVolume)(void *, float *);
	HRESULT (__attribute__((ms_abi)) *SetChannelVolumes)(void *, uint32_t, const float *, uint32_t);
	void    (__attribute__((ms_abi)) *GetChannelVolumes)(void *, uint32_t, float *);
	HRESULT (__attribute__((ms_abi)) *SetOutputMatrix)(void *, void *, uint32_t, uint32_t, const float *, uint32_t);
	void    (__attribute__((ms_abi)) *GetOutputMatrix)(void *, void *, uint32_t, uint32_t, float *);
	void    (__attribute__((ms_abi)) *DestroyVoice)(void *);
	/* IXAudio2SourceVoice */
	HRESULT (__attribute__((ms_abi)) *Start)(void *, uint32_t, uint32_t);
	HRESULT (__attribute__((ms_abi)) *Stop)(void *, uint32_t, uint32_t);
	HRESULT (__attribute__((ms_abi)) *SubmitSourceBuffer)(void *, const void *, const void *);
	HRESULT (__attribute__((ms_abi)) *FlushSourceBuffers)(void *);
	HRESULT (__attribute__((ms_abi)) *Discontinuity)(void *);
	HRESULT (__attribute__((ms_abi)) *ExitLoop)(void *, uint32_t);
	void    (__attribute__((ms_abi)) *GetState)(void *, void *, uint32_t);
	HRESULT (__attribute__((ms_abi)) *SetFrequencyRatio)(void *, float, uint32_t);
	void    (__attribute__((ms_abi)) *GetFrequencyRatio)(void *, float *);
	HRESULT (__attribute__((ms_abi)) *SetSourceSampleRate)(void *, uint32_t);
} IXAudio2SourceVoiceVtbl;

/*
 * XAUDIO2_BUFFER 구조체 (Windows 정의)
 * SubmitSourceBuffer의 pBuffer 파라미터.
 */
struct xaudio2_buffer {
	uint32_t Flags;
	uint32_t AudioBytes;
	const uint8_t *pAudioData;
	uint32_t PlayBegin;
	uint32_t PlayLength;
	uint32_t LoopBegin;
	uint32_t LoopLength;
	uint32_t LoopCount;
	void    *pContext;
};

/*
 * SourceVoice 실제 구조체
 * citcaudio 서버에 연결하여 PCM 데이터를 전송.
 */
struct xa2_source_voice {
	IXAudio2SourceVoiceVtbl *lpVtbl;
	int citcaudio_fd;
	uint32_t stream_id;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bits;
};

static void __attribute__((ms_abi)) sv_stub(void *T, ...) { (void)T; }
static HRESULT __attribute__((ms_abi)) sv_stub_hr(void *T, ...) { (void)T; return S_OK; }

static HRESULT __attribute__((ms_abi))
sv_Start(void *This, uint32_t Flags, uint32_t OperationSet)
{
	(void)This; (void)Flags; (void)OperationSet;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
sv_Stop(void *This, uint32_t Flags, uint32_t OperationSet)
{
	(void)This; (void)Flags; (void)OperationSet;
	return S_OK;
}

static HRESULT __attribute__((ms_abi))
sv_SubmitSourceBuffer(void *This, const void *pBuf, const void *pBufferWMA)
{
	struct xa2_source_voice *sv = This;
	const struct xaudio2_buffer *pBuffer = pBuf;
	(void)pBufferWMA;

	if (!pBuffer || !pBuffer->pAudioData || pBuffer->AudioBytes == 0)
		return S_OK;

	/* citcaudio로 PCM 전송 */
	if (sv->citcaudio_fd >= 0 && sv->stream_id > 0) {
		citcaudio_write(sv->citcaudio_fd, sv->stream_id,
				pBuffer->pAudioData, pBuffer->AudioBytes);
	}

	return S_OK;
}

static void __attribute__((ms_abi))
sv_DestroyVoice(void *This)
{
	struct xa2_source_voice *sv = This;
	if (sv->citcaudio_fd >= 0) {
		if (sv->stream_id > 0)
			citcaudio_close_stream(sv->citcaudio_fd,
					       sv->stream_id);
		close(sv->citcaudio_fd);
	}
	free(sv);
}

static IXAudio2SourceVoiceVtbl g_sv_vtbl = {
	.GetVoiceDetails         = (void *)sv_stub,
	.SetOutputVoices         = (void *)sv_stub_hr,
	.SetEffectChain          = (void *)sv_stub_hr,
	.EnableEffect            = (void *)sv_stub_hr,
	.DisableEffect           = (void *)sv_stub_hr,
	.GetEffectState          = (void *)sv_stub,
	.SetEffectParameters     = (void *)sv_stub_hr,
	.GetEffectParameters     = (void *)sv_stub_hr,
	.SetFilterParameters     = (void *)sv_stub_hr,
	.GetFilterParameters     = (void *)sv_stub,
	.SetOutputFilterParameters = (void *)sv_stub_hr,
	.GetOutputFilterParameters = (void *)sv_stub,
	.SetVolume               = (void *)sv_stub_hr,
	.GetVolume               = (void *)sv_stub,
	.SetChannelVolumes       = (void *)sv_stub_hr,
	.GetChannelVolumes       = (void *)sv_stub,
	.SetOutputMatrix         = (void *)sv_stub_hr,
	.GetOutputMatrix         = (void *)sv_stub,
	.DestroyVoice            = sv_DestroyVoice,
	.Start                   = sv_Start,
	.Stop                    = sv_Stop,
	.SubmitSourceBuffer      = sv_SubmitSourceBuffer,
	.FlushSourceBuffers      = (void *)sv_stub_hr,
	.Discontinuity           = (void *)sv_stub_hr,
	.ExitLoop                = (void *)sv_stub_hr,
	.GetState                = (void *)sv_stub,
	.SetFrequencyRatio       = (void *)sv_stub_hr,
	.GetFrequencyRatio       = (void *)sv_stub,
	.SetSourceSampleRate     = (void *)sv_stub_hr,
};

/* ============================================================
 * IXAudio2 구현
 * ============================================================ */

typedef struct IXAudio2Vtbl {
	/* IUnknown */
	HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *, REFIID, void **);
	ULONG   (__attribute__((ms_abi)) *AddRef)(void *);
	ULONG   (__attribute__((ms_abi)) *Release)(void *);
	/* IXAudio2 */
	HRESULT (__attribute__((ms_abi)) *RegisterForCallbacks)(void *, void *);
	void    (__attribute__((ms_abi)) *UnregisterForCallbacks)(void *, void *);
	HRESULT (__attribute__((ms_abi)) *CreateSourceVoice)(void *, void **, const WAVEFORMATEX *, uint32_t, float, void *, void *, void *);
	HRESULT (__attribute__((ms_abi)) *CreateSubmixVoice)(void *, void **, uint32_t, uint32_t, uint32_t, uint32_t, void *, void *);
	HRESULT (__attribute__((ms_abi)) *CreateMasteringVoice)(void *, void **, uint32_t, uint32_t, uint32_t, uint32_t, void *);
	HRESULT (__attribute__((ms_abi)) *StartEngine)(void *);
	void    (__attribute__((ms_abi)) *StopEngine)(void *);
	HRESULT (__attribute__((ms_abi)) *CommitChanges)(void *, uint32_t);
	void    (__attribute__((ms_abi)) *GetPerformanceData)(void *, void *);
	void    (__attribute__((ms_abi)) *SetDebugConfiguration)(void *, void *, void *);
} IXAudio2Vtbl;

struct xa2_device {
	IXAudio2Vtbl *lpVtbl;
	ULONG ref_count;
};

static HRESULT __attribute__((ms_abi))
xa2_QueryInterface(void *This, REFIID riid, void **ppv)
{ (void)riid; if (ppv) *ppv = This; return S_OK; }

static ULONG __attribute__((ms_abi))
xa2_AddRef(void *This)
{ struct xa2_device *d = This; return ++d->ref_count; }

static ULONG __attribute__((ms_abi))
xa2_Release(void *This)
{
	struct xa2_device *d = This;
	ULONG r = --d->ref_count;
	if (r == 0) free(d);
	return r;
}

static HRESULT __attribute__((ms_abi))
xa2_CreateSourceVoice(void *This, void **ppSourceVoice,
                      const WAVEFORMATEX *pSourceFormat,
                      uint32_t Flags, float MaxFreqRatio,
                      void *pCallback, void *pSendList, void *pEffectChain)
{
	(void)This; (void)Flags;
	(void)MaxFreqRatio; (void)pCallback; (void)pSendList; (void)pEffectChain;
	if (!ppSourceVoice) return E_POINTER;

	struct xa2_source_voice *sv = calloc(1, sizeof(*sv));
	if (!sv) return E_OUTOFMEMORY;

	sv->lpVtbl = &g_sv_vtbl;
	sv->citcaudio_fd = -1;
	sv->stream_id = 0;

	/* 포맷 정보 저장 */
	sv->sample_rate = 44100;
	sv->channels = 2;
	sv->bits = 16;
	if (pSourceFormat) {
		sv->sample_rate = pSourceFormat->nSamplesPerSec;
		sv->channels = pSourceFormat->nChannels;
		sv->bits = pSourceFormat->wBitsPerSample;
	}

	/* citcaudio 서버에 연결 */
	sv->citcaudio_fd = citcaudio_connect();
	if (sv->citcaudio_fd >= 0) {
		sv->stream_id = citcaudio_open_stream(
			sv->citcaudio_fd,
			sv->sample_rate, sv->channels, sv->bits);
		if (sv->stream_id > 0) {
			printf("xaudio2: citcaudio stream %u opened "
			       "(%uHz, %uch, %ubit)\n",
			       sv->stream_id, sv->sample_rate,
			       sv->channels, sv->bits);
		} else {
			close(sv->citcaudio_fd);
			sv->citcaudio_fd = -1;
			printf("xaudio2: citcaudio stream open failed\n");
		}
	} else {
		printf("xaudio2: citcaudio not available (no-op mode)\n");
	}

	*ppSourceVoice = sv;
	return S_OK;
}

/* 정적 마스터링 보이스 스텁 (오디오 출력은 SourceVoice가 담당) */
static struct { IXAudio2SourceVoiceVtbl *lpVtbl; } g_mastering_voice = {
	.lpVtbl = &g_sv_vtbl,
};

static HRESULT __attribute__((ms_abi))
xa2_CreateMasteringVoice(void *This, void **ppMasteringVoice,
                         uint32_t InputChannels, uint32_t InputSampleRate,
                         uint32_t Flags, uint32_t DeviceIndex,
                         void *pEffectChain)
{
	(void)This; (void)InputChannels; (void)InputSampleRate;
	(void)Flags; (void)DeviceIndex; (void)pEffectChain;
	if (ppMasteringVoice)
		*ppMasteringVoice = &g_mastering_voice;
	printf("xaudio2: MasteringVoice created (stub)\n");
	return S_OK;
}

static void __attribute__((ms_abi)) xa2_stub(void *T, ...) { (void)T; }
static HRESULT __attribute__((ms_abi)) xa2_stub_hr(void *T, ...) { (void)T; return S_OK; }

static IXAudio2Vtbl g_xa2_vtbl = {
	.QueryInterface     = xa2_QueryInterface,
	.AddRef             = xa2_AddRef,
	.Release            = xa2_Release,
	.RegisterForCallbacks  = (void *)xa2_stub_hr,
	.UnregisterForCallbacks = (void *)xa2_stub,
	.CreateSourceVoice  = xa2_CreateSourceVoice,
	.CreateSubmixVoice  = (void *)xa2_stub_hr,
	.CreateMasteringVoice = xa2_CreateMasteringVoice,
	.StartEngine        = (void *)xa2_stub_hr,
	.StopEngine         = (void *)xa2_stub,
	.CommitChanges      = (void *)xa2_stub_hr,
	.GetPerformanceData = (void *)xa2_stub,
	.SetDebugConfiguration = (void *)xa2_stub,
};

/* ============================================================
 * DLL 엔트리: XAudio2Create
 * ============================================================ */

static HRESULT __attribute__((ms_abi))
xaudio2_XAudio2Create(void **ppXAudio2, uint32_t Flags, uint32_t XAudio2Processor)
{
	(void)Flags; (void)XAudio2Processor;
	if (!ppXAudio2) return E_POINTER;

	struct xa2_device *dev = calloc(1, sizeof(*dev));
	if (!dev) return E_OUTOFMEMORY;

	dev->lpVtbl = &g_xa2_vtbl;
	dev->ref_count = 1;

	printf("xaudio2: XAudio2Create OK\n");
	*ppXAudio2 = dev;
	return S_OK;
}

struct stub_entry xaudio2_stub_table[] = {
	{ "xaudio2_7.dll", "XAudio2Create",
	  (void *)xaudio2_XAudio2Create },
	{ "xaudio2_9.dll", "XAudio2Create",
	  (void *)xaudio2_XAudio2Create },
	{ NULL, NULL, NULL }
};
