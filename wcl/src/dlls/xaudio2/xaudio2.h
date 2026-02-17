/*
 * xaudio2.h — XAudio2 최소 스텁
 * ===============================
 *
 * XAudio2Create, IXAudio2::CreateMasteringVoice/CreateSourceVoice 스텁.
 * 실제 오디오 출력은 DirectSound 경로를 재사용.
 */

#ifndef CITC_XAUDIO2_H
#define CITC_XAUDIO2_H

#include "../../../include/stub_entry.h"

extern struct stub_entry xaudio2_stub_table[];

#endif /* CITC_XAUDIO2_H */
