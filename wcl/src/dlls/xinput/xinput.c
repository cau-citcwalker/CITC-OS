/*
 * xinput.c — XInput 게임패드 구현
 * ==================================
 *
 * xinput1_3.dll / xinput1_4.dll / xinput9_1_0.dll 의
 * XInputGetState, XInputSetState, XInputGetCapabilities 구현.
 *
 * 백엔드: Linux evdev (/dev/input/event*).
 *   - 장치 스캔: /dev/input/event0..15 순회
 *   - ioctl(EVIOCGBIT) 으로 EV_ABS 축 존재 확인 → 게임패드 판별
 *   - O_NONBLOCK read 로 이벤트 폴링
 *   - evdev 축/버튼 → XInput 구조체 매핑
 *
 * 최대 4개 컨트롤러 (XUSER_MAX_COUNT).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>

#include "../../../include/win32.h"
#include "../../../include/stub_entry.h"

/* ============================================================
 * evdev 상수 (linux/input.h 대신 직접 정의)
 * ============================================================ */

/* ioctl 매크로 */
#define EVIOCGBIT(ev, len) (0x80000000 | (((len) & 0x1FFF) << 16) | ('E' << 8) | (0x20 + (ev)))
#define EVIOCGABS(abs)     (0x80000000 | ((sizeof(struct input_absinfo) & 0x1FFF) << 16) | ('E' << 8) | (0x40 + (abs)))
#define EVIOCGNAME(len)    (0x80000000 | (((len) & 0x1FFF) << 16) | ('E' << 8) | 0x06)

/* 이벤트 타입 */
#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_ABS  0x03
#define EV_FF   0x15
#define EV_MAX  0x1f

/* 축 코드 */
#define ABS_X     0x00
#define ABS_Y     0x01
#define ABS_Z     0x02
#define ABS_RX    0x03
#define ABS_RY    0x04
#define ABS_RZ    0x05
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#define ABS_MAX   0x3f

/* 버튼 코드 (게임패드) */
#define BTN_SOUTH   0x130  /* A */
#define BTN_EAST    0x131  /* B */
#define BTN_NORTH   0x133  /* Y */
#define BTN_WEST    0x134  /* X */
#define BTN_TL      0x136  /* LB */
#define BTN_TR      0x137  /* RB */
#define BTN_SELECT  0x13a  /* Back */
#define BTN_START   0x13b  /* Start */
#define BTN_THUMBL  0x13d  /* L3 */
#define BTN_THUMBR  0x13e  /* R3 */
#define KEY_MAX     0x2ff

struct input_event {
	uint64_t time_sec;
	uint64_t time_usec;
	uint16_t type;
	uint16_t code;
	int32_t  value;
};

struct input_absinfo {
	int32_t value;
	int32_t minimum;
	int32_t maximum;
	int32_t fuzz;
	int32_t flat;
	int32_t resolution;
};

/* Force feedback */
#define FF_RUMBLE   0x50
#define FF_MAX      0x7f

struct ff_rumble_effect {
	uint16_t strong_magnitude;
	uint16_t weak_magnitude;
};

struct ff_effect {
	uint16_t type;
	int16_t  id;
	uint16_t direction;
	struct {
		uint16_t length;
		uint16_t delay;
	} replay;
	struct {
		uint16_t level;
		uint16_t envelope_length;
	} trigger;
	union {
		struct ff_rumble_effect rumble;
	} u;
};

#define EVIOCSFF (0x40000000 | ((sizeof(struct ff_effect) & 0x1FFF) << 16) | ('E' << 8) | 0x80)

/* ============================================================
 * 컨트롤러 상태
 * ============================================================ */

struct evdev_pad {
	int fd;                     /* evdev fd, -1 = 미연결 */
	int ff_id;                  /* force feedback effect id, -1 = 없음 */
	uint32_t packet;            /* 패킷 번호 (변경 시 증가) */

	/* 축 범위 */
	int32_t ax_min[6];          /* ABS_X..ABS_RZ */
	int32_t ax_max[6];
	int32_t ax_val[6];          /* 현재 값 */

	/* HAT (D-Pad) */
	int32_t hat_x, hat_y;

	/* 버튼 상태 */
	int btn[16];                /* btn[i]=1 pressed */
};

static struct evdev_pad g_pads[XUSER_MAX_COUNT];
static int g_pads_scanned;

/* 축 범위를 -32768..32767로 정규화 */
static int16_t normalize_axis(int32_t val, int32_t mn, int32_t mx)
{
	if (mx == mn) return 0;
	/* 0..range → -32768..32767 */
	int64_t range = (int64_t)(mx - mn);
	int64_t centered = (int64_t)(val - mn) * 65535 / range - 32768;
	if (centered < -32768) centered = -32768;
	if (centered > 32767)  centered = 32767;
	return (int16_t)centered;
}

/* 축 범위를 0..255로 정규화 (트리거) */
static uint8_t normalize_trigger(int32_t val, int32_t mn, int32_t mx)
{
	if (mx == mn) return 0;
	int64_t range = (int64_t)(mx - mn);
	int64_t norm = (int64_t)(val - mn) * 255 / range;
	if (norm < 0)   norm = 0;
	if (norm > 255) norm = 255;
	return (uint8_t)norm;
}

/* 비트가 설정되었는지 확인 */
static int test_bit(int bit, const uint8_t *array)
{
	return (array[bit / 8] >> (bit % 8)) & 1;
}

/* ============================================================
 * 장치 스캔
 * ============================================================ */

static void scan_gamepads(void)
{
	if (g_pads_scanned) return;
	g_pads_scanned = 1;

	for (int i = 0; i < XUSER_MAX_COUNT; i++) {
		g_pads[i].fd = -1;
		g_pads[i].ff_id = -1;
	}

	int found = 0;
	char path[64];

	for (int ev = 0; ev < 16 && found < XUSER_MAX_COUNT; ev++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", ev);
		int fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0) {
			fd = open(path, O_RDONLY | O_NONBLOCK);
			if (fd < 0) continue;
		}

		/* EV_ABS 비트 확인 */
		uint8_t ev_bits[(EV_MAX + 7) / 8 + 1];
		memset(ev_bits, 0, sizeof(ev_bits));
		if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
			close(fd);
			continue;
		}

		if (!test_bit(EV_ABS, ev_bits)) {
			close(fd);
			continue;
		}

		/* ABS 축 비트 확인 — 최소 ABS_X, ABS_Y 필요 */
		uint8_t abs_bits[(ABS_MAX + 7) / 8 + 1];
		memset(abs_bits, 0, sizeof(abs_bits));
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);

		if (!test_bit(ABS_X, abs_bits) || !test_bit(ABS_Y, abs_bits)) {
			close(fd);
			continue;
		}

		/* 게임패드로 판정 — 이름 가져오기 */
		char name[128] = "Unknown";
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);

		struct evdev_pad *p = &g_pads[found];
		p->fd = fd;

		/* 축 범위 가져오기 */
		int axes[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ };
		for (int a = 0; a < 6; a++) {
			struct input_absinfo ai = {0};
			if (test_bit(axes[a], abs_bits) &&
			    ioctl(fd, EVIOCGABS(axes[a]), &ai) >= 0) {
				p->ax_min[a] = ai.minimum;
				p->ax_max[a] = ai.maximum;
				p->ax_val[a] = ai.value;
			} else {
				p->ax_min[a] = 0;
				p->ax_max[a] = 0;
			}
		}

		/* FF 확인 */
		uint8_t ff_bits[(FF_MAX + 7) / 8 + 1];
		memset(ff_bits, 0, sizeof(ff_bits));
		if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits) >= 0 &&
		    test_bit(FF_RUMBLE, ff_bits)) {
			struct ff_effect eff;
			memset(&eff, 0, sizeof(eff));
			eff.type = FF_RUMBLE;
			eff.id = -1;
			eff.replay.length = 1000;
			if (ioctl(fd, EVIOCSFF, &eff) >= 0)
				p->ff_id = eff.id;
		}

		printf("xinput: pad[%d] = %s (%s)\n", found, path, name);
		found++;
	}

	if (found == 0)
		printf("xinput: no gamepad found\n");
}

/* ============================================================
 * evdev 이벤트 폴링
 * ============================================================ */

static void poll_events(struct evdev_pad *p)
{
	struct input_event ev;
	int changed = 0;

	while (read(p->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_ABS) {
			switch (ev.code) {
			case ABS_X:    p->ax_val[0] = ev.value; changed = 1; break;
			case ABS_Y:    p->ax_val[1] = ev.value; changed = 1; break;
			case ABS_Z:    p->ax_val[2] = ev.value; changed = 1; break;
			case ABS_RX:   p->ax_val[3] = ev.value; changed = 1; break;
			case ABS_RY:   p->ax_val[4] = ev.value; changed = 1; break;
			case ABS_RZ:   p->ax_val[5] = ev.value; changed = 1; break;
			case ABS_HAT0X: p->hat_x = ev.value; changed = 1; break;
			case ABS_HAT0Y: p->hat_y = ev.value; changed = 1; break;
			}
		} else if (ev.type == EV_KEY) {
			int pressed = (ev.value != 0) ? 1 : 0;
			switch (ev.code) {
			case BTN_SOUTH:  p->btn[0] = pressed; changed = 1; break;
			case BTN_EAST:   p->btn[1] = pressed; changed = 1; break;
			case BTN_NORTH:  p->btn[2] = pressed; changed = 1; break;
			case BTN_WEST:   p->btn[3] = pressed; changed = 1; break;
			case BTN_TL:     p->btn[4] = pressed; changed = 1; break;
			case BTN_TR:     p->btn[5] = pressed; changed = 1; break;
			case BTN_SELECT: p->btn[6] = pressed; changed = 1; break;
			case BTN_START:  p->btn[7] = pressed; changed = 1; break;
			case BTN_THUMBL: p->btn[8] = pressed; changed = 1; break;
			case BTN_THUMBR: p->btn[9] = pressed; changed = 1; break;
			}
		}
	}

	if (changed)
		p->packet++;
}

/* ============================================================
 * XInput API
 * ============================================================ */

static DWORD __attribute__((ms_abi))
xinput_XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState)
{
	scan_gamepads();

	if (dwUserIndex >= XUSER_MAX_COUNT || !pState)
		return ERROR_DEVICE_NOT_CONNECTED;

	struct evdev_pad *p = &g_pads[dwUserIndex];
	if (p->fd < 0)
		return ERROR_DEVICE_NOT_CONNECTED;

	poll_events(p);

	memset(pState, 0, sizeof(*pState));
	pState->dwPacketNumber = p->packet;

	XINPUT_GAMEPAD *g = &pState->Gamepad;

	/* 축 */
	g->sThumbLX = normalize_axis(p->ax_val[0], p->ax_min[0], p->ax_max[0]);
	g->sThumbLY = (int16_t)-normalize_axis(p->ax_val[1], p->ax_min[1], p->ax_max[1]); /* Y 반전 */
	g->bLeftTrigger  = normalize_trigger(p->ax_val[2], p->ax_min[2], p->ax_max[2]);
	g->sThumbRX = normalize_axis(p->ax_val[3], p->ax_min[3], p->ax_max[3]);
	g->sThumbRY = (int16_t)-normalize_axis(p->ax_val[4], p->ax_min[4], p->ax_max[4]); /* Y 반전 */
	g->bRightTrigger = normalize_trigger(p->ax_val[5], p->ax_min[5], p->ax_max[5]);

	/* 버튼 */
	if (p->btn[0]) g->wButtons |= XINPUT_GAMEPAD_A;
	if (p->btn[1]) g->wButtons |= XINPUT_GAMEPAD_B;
	if (p->btn[2]) g->wButtons |= XINPUT_GAMEPAD_Y;
	if (p->btn[3]) g->wButtons |= XINPUT_GAMEPAD_X;
	if (p->btn[4]) g->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (p->btn[5]) g->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
	if (p->btn[6]) g->wButtons |= XINPUT_GAMEPAD_BACK;
	if (p->btn[7]) g->wButtons |= XINPUT_GAMEPAD_START;
	if (p->btn[8]) g->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (p->btn[9]) g->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

	/* D-Pad (HAT) */
	if (p->hat_x < 0) g->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (p->hat_x > 0) g->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (p->hat_y < 0) g->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
	if (p->hat_y > 0) g->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;

	return ERROR_SUCCESS;
}

static DWORD __attribute__((ms_abi))
xinput_XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration)
{
	scan_gamepads();

	if (dwUserIndex >= XUSER_MAX_COUNT)
		return ERROR_DEVICE_NOT_CONNECTED;

	struct evdev_pad *p = &g_pads[dwUserIndex];
	if (p->fd < 0)
		return ERROR_DEVICE_NOT_CONNECTED;

	if (p->ff_id >= 0 && pVibration) {
		struct input_event play;
		memset(&play, 0, sizeof(play));
		play.type = EV_FF;
		play.code = (uint16_t)p->ff_id;
		play.value = (pVibration->wLeftMotorSpeed > 0 ||
		              pVibration->wRightMotorSpeed > 0) ? 1 : 0;
		ssize_t r = write(p->fd, &play, sizeof(play));
		(void)r;
	}

	return ERROR_SUCCESS;
}

static DWORD __attribute__((ms_abi))
xinput_XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
                             XINPUT_CAPABILITIES *pCaps)
{
	(void)dwFlags;
	scan_gamepads();

	if (dwUserIndex >= XUSER_MAX_COUNT || !pCaps)
		return ERROR_DEVICE_NOT_CONNECTED;

	struct evdev_pad *p = &g_pads[dwUserIndex];
	if (p->fd < 0)
		return ERROR_DEVICE_NOT_CONNECTED;

	memset(pCaps, 0, sizeof(*pCaps));
	pCaps->Type = XINPUT_DEVTYPE_GAMEPAD;
	pCaps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;

	/* 모든 버튼/축 지원 표시 */
	pCaps->Gamepad.wButtons = 0xFFFF;
	pCaps->Gamepad.bLeftTrigger = 255;
	pCaps->Gamepad.bRightTrigger = 255;
	pCaps->Gamepad.sThumbLX = 32767;
	pCaps->Gamepad.sThumbLY = 32767;
	pCaps->Gamepad.sThumbRX = 32767;
	pCaps->Gamepad.sThumbRY = 32767;

	if (p->ff_id >= 0) {
		pCaps->Vibration.wLeftMotorSpeed = 65535;
		pCaps->Vibration.wRightMotorSpeed = 65535;
	}

	return ERROR_SUCCESS;
}

/* XInputEnable — 글로벌 활성화/비활성화 (스텁) */
static void __attribute__((ms_abi))
xinput_XInputEnable(BOOL enable)
{
	(void)enable;
}

/* ============================================================
 * DLL 스텁 테이블
 * ============================================================ */

struct stub_entry xinput_stub_table[] = {
	{ "xinput1_3.dll", "XInputGetState",
	  (void *)xinput_XInputGetState },
	{ "xinput1_3.dll", "XInputSetState",
	  (void *)xinput_XInputSetState },
	{ "xinput1_3.dll", "XInputGetCapabilities",
	  (void *)xinput_XInputGetCapabilities },
	{ "xinput1_3.dll", "XInputEnable",
	  (void *)xinput_XInputEnable },

	{ "xinput1_4.dll", "XInputGetState",
	  (void *)xinput_XInputGetState },
	{ "xinput1_4.dll", "XInputSetState",
	  (void *)xinput_XInputSetState },
	{ "xinput1_4.dll", "XInputGetCapabilities",
	  (void *)xinput_XInputGetCapabilities },
	{ "xinput1_4.dll", "XInputEnable",
	  (void *)xinput_XInputEnable },

	{ "xinput9_1_0.dll", "XInputGetState",
	  (void *)xinput_XInputGetState },
	{ "xinput9_1_0.dll", "XInputSetState",
	  (void *)xinput_XInputSetState },
	{ "xinput9_1_0.dll", "XInputGetCapabilities",
	  (void *)xinput_XInputGetCapabilities },

	{ NULL, NULL, NULL }
};
