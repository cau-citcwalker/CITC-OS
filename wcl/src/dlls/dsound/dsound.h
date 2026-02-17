/*
 * dsound.h — DirectSound 8 구현
 * ================================
 *
 * IDirectSound8 + IDirectSoundBuffer8 COM 인터페이스.
 * 백엔드: Linux OSS (/dev/dsp) 또는 ALSA (/dev/snd/pcm*).
 */

#ifndef CITC_DSOUND_H
#define CITC_DSOUND_H

#include "../../../include/stub_entry.h"

extern struct stub_entry dsound_stub_table[];

#endif /* CITC_DSOUND_H */
