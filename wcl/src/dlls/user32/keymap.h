/*
 * keymap.h — Linux evdev keycode → Windows VK_* 변환
 * ====================================================
 *
 * Linux에서 키보드 이벤트는 evdev(Event Device) 서브시스템을 통해
 * /dev/input/eventN 파일로 전달됩니다.
 *
 * evdev keycode vs Windows VK:
 *   evdev:  하드웨어 스캔코드 기반 (linux/input-event-codes.h)
 *           KEY_A = 30, KEY_ESC = 1, KEY_ENTER = 28
 *   VK:    가상 키코드 (winuser.h)
 *           VK_A = 0x41 ('A'), VK_ESCAPE = 0x1B, VK_RETURN = 0x0D
 *
 * evdev 코드는 키보드의 물리적 위치에 매핑,
 * VK 코드는 키의 논리적 의미에 매핑.
 *
 * 이 변환 테이블은 CDP 컴포지터가 보내는 evdev keycode를
 * Win32 앱이 기대하는 VK 코드로 변환합니다.
 *
 * 참고: linux/input-event-codes.h (커널 소스)
 *       winuser.h (Windows SDK)
 */

#ifndef USER32_KEYMAP_H
#define USER32_KEYMAP_H

#include <stdint.h>

/* ============================================================
 * Windows 가상 키코드 (VK_*) — 필요한 것만 정의
 * ============================================================
 *
 * 전체 목록은 winuser.h에 200개+ 정의되어 있지만
 * 일반적인 키보드 입력에 필요한 것만 포함.
 */

/* 특수키 */
#define VK_BACK       0x08  /* Backspace */
#define VK_TAB        0x09  /* Tab */
#define VK_RETURN     0x0D  /* Enter */
#define VK_SHIFT      0x10  /* Shift (좌/우 통합) */
#define VK_CONTROL    0x11  /* Ctrl (좌/우 통합) */
#define VK_MENU       0x12  /* Alt (좌/우 통합) */
#define VK_PAUSE      0x13  /* Pause/Break */
#define VK_CAPITAL    0x14  /* Caps Lock */
#define VK_ESCAPE     0x1B  /* Escape */
#define VK_SPACE      0x20  /* Space */

/* 탐색키 */
#define VK_PRIOR      0x21  /* Page Up */
#define VK_NEXT       0x22  /* Page Down */
#define VK_END        0x23  /* End */
#define VK_HOME       0x24  /* Home */
#define VK_LEFT       0x25  /* 화살표 ← */
#define VK_UP         0x26  /* 화살표 ↑ */
#define VK_RIGHT      0x27  /* 화살표 → */
#define VK_DOWN       0x28  /* 화살표 ↓ */
#define VK_INSERT     0x2D  /* Insert */
#define VK_DELETE     0x2E  /* Delete */

/* 숫자 (ASCII와 동일: '0'-'9') */
/* VK_0 = 0x30 ... VK_9 = 0x39 */

/* 영문자 (ASCII와 동일: 'A'-'Z') */
/* VK_A = 0x41 ... VK_Z = 0x5A */

/* 왼쪽/오른쪽 Windows 키 */
#define VK_LWIN       0x5B
#define VK_RWIN       0x5C

/* 넘패드 */
#define VK_NUMPAD0    0x60
#define VK_NUMPAD1    0x61
#define VK_NUMPAD2    0x62
#define VK_NUMPAD3    0x63
#define VK_NUMPAD4    0x64
#define VK_NUMPAD5    0x65
#define VK_NUMPAD6    0x66
#define VK_NUMPAD7    0x67
#define VK_NUMPAD8    0x68
#define VK_NUMPAD9    0x69
#define VK_MULTIPLY   0x6A  /* Numpad * */
#define VK_ADD        0x6B  /* Numpad + */
#define VK_SUBTRACT   0x6D  /* Numpad - */
#define VK_DECIMAL    0x6E  /* Numpad . */
#define VK_DIVIDE     0x6F  /* Numpad / */

/* 펑션키 */
#define VK_F1         0x70
#define VK_F2         0x71
#define VK_F3         0x72
#define VK_F4         0x73
#define VK_F5         0x74
#define VK_F6         0x75
#define VK_F7         0x76
#define VK_F8         0x77
#define VK_F9         0x78
#define VK_F10        0x79
#define VK_F11        0x7A
#define VK_F12        0x7B

/* Lock 키 */
#define VK_NUMLOCK    0x90
#define VK_SCROLL     0x91  /* Scroll Lock */

/* 좌/우 구분 Shift/Ctrl/Alt */
#define VK_LSHIFT     0xA0
#define VK_RSHIFT     0xA1
#define VK_LCONTROL   0xA2
#define VK_RCONTROL   0xA3
#define VK_LMENU      0xA4  /* Left Alt */
#define VK_RMENU      0xA5  /* Right Alt */

/* 구두점/기호 (US 키보드 레이아웃) */
#define VK_OEM_1      0xBA  /* ;: */
#define VK_OEM_PLUS   0xBB  /* =+ */
#define VK_OEM_COMMA  0xBC  /* ,< */
#define VK_OEM_MINUS  0xBD  /* -_ */
#define VK_OEM_PERIOD 0xBE  /* .> */
#define VK_OEM_2      0xBF  /* /? */
#define VK_OEM_3      0xC0  /* `~ */
#define VK_OEM_4      0xDB  /* [{ */
#define VK_OEM_5      0xDC  /* \| */
#define VK_OEM_6      0xDD  /* ]} */
#define VK_OEM_7      0xDE  /* '" */

/* ============================================================
 * Linux evdev keycode 정의 (linux/input-event-codes.h)
 * ============================================================
 *
 * 전체 키 정의는 커널 소스의 include/uapi/linux/input-event-codes.h
 * 여기서는 변환 테이블에 필요한 것만 인라인 정의.
 */

/* evdev keycode 상수 — KEY_* */
#define KEY_ESC          1
#define KEY_1            2
#define KEY_2            3
#define KEY_3            4
#define KEY_4            5
#define KEY_5            6
#define KEY_6            7
#define KEY_7            8
#define KEY_8            9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_NUMLOCK     69
#define KEY_SCROLLLOCK  70
#define KEY_KP7         71
#define KEY_KP8         72
#define KEY_KP9         73
#define KEY_KPMINUS     74
#define KEY_KP4         75
#define KEY_KP5         76
#define KEY_KP6         77
#define KEY_KPPLUS      78
#define KEY_KP1         79
#define KEY_KP2         80
#define KEY_KP3         81
#define KEY_KP0         82
#define KEY_KPDOT       83
#define KEY_F11         87
#define KEY_F12         88
#define KEY_KPENTER     96
#define KEY_RIGHTCTRL   97
#define KEY_KPSLASH     98
#define KEY_RIGHTALT   100
#define KEY_HOME       102
#define KEY_UP         103
#define KEY_PAGEUP     104
#define KEY_LEFT       105
#define KEY_RIGHT      106
#define KEY_END        107
#define KEY_DOWN       108
#define KEY_PAGEDOWN   109
#define KEY_INSERT     110
#define KEY_DELETE     111
#define KEY_PAUSE      119
#define KEY_LEFTMETA   125
#define KEY_RIGHTMETA  126
#define KEY_Y           21  /* KEY_Y = 21 */

/* ============================================================
 * 변환 테이블
 * ============================================================
 *
 * 배열 인덱스 = Linux evdev keycode
 * 배열 값 = Windows VK_* 코드
 *
 * 0 = 매핑 없음 (미정의 키)
 *
 * 크기를 128로 제한 — 일반적인 키보드 키를 모두 포함.
 * 확장 키 (멀티미디어 등)는 별도 매핑 필요 시 확장.
 */

static const uint8_t evdev_to_vk[128] = {
	/* 0 */  0,
	/* 1  KEY_ESC */       VK_ESCAPE,
	/* 2  KEY_1 */         '1',
	/* 3  KEY_2 */         '2',
	/* 4  KEY_3 */         '3',
	/* 5  KEY_4 */         '4',
	/* 6  KEY_5 */         '5',
	/* 7  KEY_6 */         '6',
	/* 8  KEY_7 */         '7',
	/* 9  KEY_8 */         '8',
	/* 10 KEY_9 */         '9',
	/* 11 KEY_0 */         '0',
	/* 12 KEY_MINUS */     VK_OEM_MINUS,
	/* 13 KEY_EQUAL */     VK_OEM_PLUS,
	/* 14 KEY_BACKSPACE */ VK_BACK,
	/* 15 KEY_TAB */       VK_TAB,
	/* 16 KEY_Q */         'Q',
	/* 17 KEY_W */         'W',
	/* 18 KEY_E */         'E',
	/* 19 KEY_R */         'R',
	/* 20 KEY_T */         'T',
	/* 21 KEY_Y */         'Y',
	/* 22 KEY_U */         'U',
	/* 23 KEY_I */         'I',
	/* 24 KEY_O */         'O',
	/* 25 KEY_P */         'P',
	/* 26 KEY_LEFTBRACE */ VK_OEM_4,
	/* 27 KEY_RIGHTBRACE */VK_OEM_6,
	/* 28 KEY_ENTER */     VK_RETURN,
	/* 29 KEY_LEFTCTRL */  VK_LCONTROL,
	/* 30 KEY_A */         'A',
	/* 31 KEY_S */         'S',
	/* 32 KEY_D */         'D',
	/* 33 KEY_F */         'F',
	/* 34 KEY_G */         'G',
	/* 35 KEY_H */         'H',
	/* 36 KEY_J */         'J',
	/* 37 KEY_K */         'K',
	/* 38 KEY_L */         'L',
	/* 39 KEY_SEMICOLON */ VK_OEM_1,
	/* 40 KEY_APOSTROPHE */VK_OEM_7,
	/* 41 KEY_GRAVE */     VK_OEM_3,
	/* 42 KEY_LEFTSHIFT */ VK_LSHIFT,
	/* 43 KEY_BACKSLASH */ VK_OEM_5,
	/* 44 KEY_Z */         'Z',
	/* 45 KEY_X */         'X',
	/* 46 KEY_C */         'C',
	/* 47 KEY_V */         'V',
	/* 48 KEY_B */         'B',
	/* 49 KEY_N */         'N',
	/* 50 KEY_M */         'M',
	/* 51 KEY_COMMA */     VK_OEM_COMMA,
	/* 52 KEY_DOT */       VK_OEM_PERIOD,
	/* 53 KEY_SLASH */     VK_OEM_2,
	/* 54 KEY_RIGHTSHIFT */VK_RSHIFT,
	/* 55 KEY_KPASTERISK */VK_MULTIPLY,
	/* 56 KEY_LEFTALT */   VK_LMENU,
	/* 57 KEY_SPACE */     VK_SPACE,
	/* 58 KEY_CAPSLOCK */  VK_CAPITAL,
	/* 59 KEY_F1 */        VK_F1,
	/* 60 KEY_F2 */        VK_F2,
	/* 61 KEY_F3 */        VK_F3,
	/* 62 KEY_F4 */        VK_F4,
	/* 63 KEY_F5 */        VK_F5,
	/* 64 KEY_F6 */        VK_F6,
	/* 65 KEY_F7 */        VK_F7,
	/* 66 KEY_F8 */        VK_F8,
	/* 67 KEY_F9 */        VK_F9,
	/* 68 KEY_F10 */       VK_F10,
	/* 69 KEY_NUMLOCK */   VK_NUMLOCK,
	/* 70 KEY_SCROLLLOCK */VK_SCROLL,
	/* 71 KEY_KP7 */       VK_NUMPAD7,
	/* 72 KEY_KP8 */       VK_NUMPAD8,
	/* 73 KEY_KP9 */       VK_NUMPAD9,
	/* 74 KEY_KPMINUS */   VK_SUBTRACT,
	/* 75 KEY_KP4 */       VK_NUMPAD4,
	/* 76 KEY_KP5 */       VK_NUMPAD5,
	/* 77 KEY_KP6 */       VK_NUMPAD6,
	/* 78 KEY_KPPLUS */    VK_ADD,
	/* 79 KEY_KP1 */       VK_NUMPAD1,
	/* 80 KEY_KP2 */       VK_NUMPAD2,
	/* 81 KEY_KP3 */       VK_NUMPAD3,
	/* 82 KEY_KP0 */       VK_NUMPAD0,
	/* 83 KEY_KPDOT */     VK_DECIMAL,
	/* 84 */  0,
	/* 85 */  0,
	/* 86 */  0,
	/* 87 KEY_F11 */       VK_F11,
	/* 88 KEY_F12 */       VK_F12,
	/* 89 */  0,
	/* 90 */  0,
	/* 91 */  0,
	/* 92 */  0,
	/* 93 */  0,
	/* 94 */  0,
	/* 95 */  0,
	/* 96  KEY_KPENTER */  VK_RETURN,
	/* 97  KEY_RIGHTCTRL */VK_RCONTROL,
	/* 98  KEY_KPSLASH */  VK_DIVIDE,
	/* 99 */  0,
	/* 100 KEY_RIGHTALT */ VK_RMENU,
	/* 101 */ 0,
	/* 102 KEY_HOME */     VK_HOME,
	/* 103 KEY_UP */       VK_UP,
	/* 104 KEY_PAGEUP */   VK_PRIOR,
	/* 105 KEY_LEFT */     VK_LEFT,
	/* 106 KEY_RIGHT */    VK_RIGHT,
	/* 107 KEY_END */      VK_END,
	/* 108 KEY_DOWN */     VK_DOWN,
	/* 109 KEY_PAGEDOWN */ VK_NEXT,
	/* 110 KEY_INSERT */   VK_INSERT,
	/* 111 KEY_DELETE */   VK_DELETE,
	/* 112 */ 0,
	/* 113 */ 0,
	/* 114 */ 0,
	/* 115 */ 0,
	/* 116 */ 0,
	/* 117 */ 0,
	/* 118 */ 0,
	/* 119 KEY_PAUSE */    VK_PAUSE,
	/* 120 */ 0,
	/* 121 */ 0,
	/* 122 */ 0,
	/* 123 */ 0,
	/* 124 */ 0,
	/* 125 KEY_LEFTMETA */ VK_LWIN,
	/* 126 KEY_RIGHTMETA */VK_RWIN,
	/* 127 */ 0,
};

/*
 * evdev keycode → VK 변환 함수
 *
 * 범위 밖의 keycode(128+)는 0 반환 (매핑 없음).
 * 호출자가 0을 확인하여 미지원 키를 무시할 수 있음.
 */
static inline uint32_t linux_keycode_to_vk(uint32_t keycode)
{
	if (keycode >= 128)
		return 0;
	return evdev_to_vk[keycode];
}

#endif /* USER32_KEYMAP_H */
