/*
 * xinput.h — XInput 게임패드 구현
 * ==================================
 *
 * XInputGetState, XInputSetState, XInputGetCapabilities.
 * 백엔드: Linux evdev (/dev/input/event*).
 */

#ifndef CITC_XINPUT_H
#define CITC_XINPUT_H

#include "../../../include/stub_entry.h"

extern struct stub_entry xinput_stub_table[];

#endif /* CITC_XINPUT_H */
