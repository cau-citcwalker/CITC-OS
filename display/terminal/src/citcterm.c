/*
 * citcterm — CITC OS Terminal Emulator
 * =====================================
 *
 * CDP 클라이언트 터미널 에뮬레이터.
 *
 * 이것이 gnome-terminal, xterm, Konsole의 원리입니다:
 *   1. 윈도우 시스템에 연결하여 그래픽 윈도우 생성
 *   2. PTY(의사 터미널) 쌍 생성
 *   3. 자식 프로세스에서 쉘 실행 (PTY 슬레이브 연결)
 *   4. 키보드 입력 → PTY 마스터에 쓰기 → 쉘이 읽음
 *   5. 쉘 출력 → PTY 마스터에서 읽기 → ANSI 파싱 → 화면 렌더링
 *
 * PTY(Pseudo-Terminal)란?
 *   하드웨어 터미널(VT100 등)을 소프트웨어로 에뮬레이션하는 장치.
 *   마스터/슬레이브 쌍으로 구성:
 *     마스터: 터미널 에뮬레이터(우리)가 읽기/쓰기
 *     슬레이브: 쉘의 stdin/stdout/stderr
 *
 *     마스터에 쓰면 → 슬레이브에서 읽을 수 있음 (키보드 입력 전달)
 *     슬레이브에 쓰면 → 마스터에서 읽을 수 있음 (쉘 출력 수신)
 *
 * 사용법:
 *   compositor &
 *   sleep 2
 *   citcterm
 */

/*
 * _GNU_SOURCE — POSIX PTY 함수 + Linux 확장 모두 활성화.
 *
 * posix_openpt(), grantpt(), unlockpt(), ptsname()은
 * _XOPEN_SOURCE >= 600이 필요하고, syscall()은 GNU 확장이다.
 * _GNU_SOURCE는 _XOPEN_SOURCE를 포함하므로 둘 다 해결된다.
 *
 * _XOPEN_SOURCE만 쓰면 syscall() 등 Linux 확장이 숨겨진다!
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/input-event-codes.h>

/* CDP 클라이언트 라이브러리 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../protocol/cdp_client.h"
#pragma GCC diagnostic pop

/* 8x8 비트맵 폰트 */
#include "../../fbdraw/src/font8x8.h"

/* ============================================================
 * 터미널 상수 & 데이터 구조
 * ============================================================ */

/*
 * 터미널 크기: 80열 x 25행
 *
 * VT100 표준 크기. 1978년 DEC VT100이 80x24를 사용했고,
 * 이후 80x25가 IBM PC의 표준이 되었다.
 * 8x8 폰트 → 640x200 픽셀.
 */
#define TERM_COLS	80
#define TERM_ROWS	25
#define TERM_WIDTH	(TERM_COLS * 8)		/* 640 */
#define TERM_HEIGHT	(TERM_ROWS * 8)		/* 200 */

/* 색상 */
#define COLOR_BG	0x00000000	/* 검정 배경 */
#define COLOR_FG	0x00C8C8C8	/* 밝은 회색 텍스트 */
#define COLOR_CURSOR	0x00FFCC00	/* 노란색 블록 커서 */

/*
 * 터미널 셀 — 화면의 한 글자 위치
 *
 * 실제 터미널(xterm 등)은 색상, 굵기, 밑줄 속성도 저장하지만,
 * v1에서는 문자만 저장.
 */
struct term_cell {
	char ch;
};

/*
 * ANSI 이스케이프 시퀀스 파서 상태
 *
 * 쉘이 출력하는 데이터에는 텍스트와 제어 시퀀스가 섞여 있다:
 *   "Hello\033[2J\033[H"  →  "Hello" + 화면지움 + 커서홈
 *
 * 상태 기계로 바이트 단위 처리:
 *   ESC_NORMAL: 일반 문자 / 제어 문자 처리
 *   ESC_ESC:    ESC(0x1B) 받음, 다음 바이트 대기
 *   ESC_CSI:    CSI(ESC+'[') 받음, 파라미터 수집 중
 */
enum esc_state {
	ESC_NORMAL,
	ESC_ESC,
	ESC_CSI,
};

/*
 * 터미널 전체 상태
 */
struct terminal {
	/* 셀 버퍼 (화면 내용) */
	struct term_cell cells[TERM_ROWS][TERM_COLS];
	int cursor_row;
	int cursor_col;

	/* ANSI 이스케이프 시퀀스 파서 */
	enum esc_state esc_state;
	char esc_buf[32];
	int esc_len;

	/* PTY */
	int pty_master;
	pid_t child_pid;

	/* CDP 연결 */
	struct cdp_conn *conn;
	struct cdp_window *win;

	/* 상태 플래그 */
	int running;
	int dirty;	/* 1이면 다시 렌더링 필요 */
};

/* ============================================================
 * 1. 터미널 버퍼 조작
 * ============================================================ */

/*
 * 화면 한 줄 스크롤 (위로)
 *
 * 커서가 마지막 행(24)을 넘어가면 호출.
 * 모든 줄을 한 칸 위로 이동하고, 마지막 줄을 비움.
 *
 * 이것이 터미널에서 텍스트가 위로 올라가는 "스크롤" 동작.
 * 실제 터미널은 스크롤백 버퍼(히스토리)도 있지만 v1에서는 생략.
 */
static void term_scroll_up(struct terminal *term)
{
	memmove(&term->cells[0], &term->cells[1],
		sizeof(term->cells[0]) * (TERM_ROWS - 1));
	memset(&term->cells[TERM_ROWS - 1], 0, sizeof(term->cells[0]));
}

/* ============================================================
 * 2. ANSI 이스케이프 시퀀스 파서
 * ============================================================
 *
 * ANSI 이스케이프 코드의 역사:
 *   1978년 DEC VT100 터미널이 도입한 제어 코드.
 *   ESC(0x1B) + '[' + 파라미터 + 명령문자 형식.
 *   ANSI X3.64 표준으로 채택되어 모든 터미널이 지원.
 *
 * 예시:
 *   \033[H       커서를 (0,0)으로 이동
 *   \033[2J      화면 전체 지우기
 *   \033[K       현재 줄의 커서 이후 지우기
 *   \033[10;20H  커서를 10행 20열로 이동
 *   \033[31m     빨간색 (여기서는 무시)
 */

/*
 * CSI 파라미터 파싱
 *
 * "12;34" 같은 문자열을 params[0]=12, params[1]=34로 변환.
 * ';'는 파라미터 구분자.
 */
static int parse_csi_params(const char *buf, int len,
			    int *params, int max_params)
{
	int count = 0;
	int val = 0;
	int has_digit = 0;

	for (int i = 0; i < len && count < max_params; i++) {
		if (buf[i] >= '0' && buf[i] <= '9') {
			val = val * 10 + (buf[i] - '0');
			has_digit = 1;
		} else if (buf[i] == ';') {
			params[count++] = has_digit ? val : 0;
			val = 0;
			has_digit = 0;
		}
	}

	if ((has_digit || count > 0) && count < max_params)
		params[count++] = has_digit ? val : 0;

	return count;
}

/*
 * CSI 명령 실행
 *
 * CSI = Control Sequence Introducer = ESC + '['
 * 형식: ESC [ <파라미터들> <명령문자>
 */
static void term_execute_csi(struct terminal *term, char cmd)
{
	int params[8] = {0};
	int np = parse_csi_params(term->esc_buf, term->esc_len,
				  params, 8);
	int n = (np > 0 && params[0] > 0) ? params[0] : 1;

	switch (cmd) {
	case 'A': /* CUU — 커서 위로 n칸 */
		term->cursor_row -= n;
		if (term->cursor_row < 0)
			term->cursor_row = 0;
		break;

	case 'B': /* CUD — 커서 아래로 n칸 */
		term->cursor_row += n;
		if (term->cursor_row >= TERM_ROWS)
			term->cursor_row = TERM_ROWS - 1;
		break;

	case 'C': /* CUF — 커서 오른쪽 n칸 */
		term->cursor_col += n;
		if (term->cursor_col >= TERM_COLS)
			term->cursor_col = TERM_COLS - 1;
		break;

	case 'D': /* CUB — 커서 왼쪽 n칸 */
		term->cursor_col -= n;
		if (term->cursor_col < 0)
			term->cursor_col = 0;
		break;

	case 'H': /* CUP — 커서 위치 지정 */
	case 'f': /* HVP — 같은 동작 */
	{
		/*
		 * ESC[row;colH
		 * ANSI에서 행/열은 1부터 시작 (우리는 0부터)
		 */
		int row = (np > 0 && params[0] > 0) ? params[0] - 1 : 0;
		int col = (np > 1 && params[1] > 0) ? params[1] - 1 : 0;

		if (row >= TERM_ROWS) row = TERM_ROWS - 1;
		if (col >= TERM_COLS) col = TERM_COLS - 1;
		term->cursor_row = row;
		term->cursor_col = col;
		break;
	}

	case 'J': /* ED — 화면 지우기 */
	{
		int mode = (np > 0) ? params[0] : 0;

		if (mode == 0) {
			/* 커서부터 화면 끝까지 */
			for (int c = term->cursor_col; c < TERM_COLS; c++)
				term->cells[term->cursor_row][c].ch = 0;
			for (int r = term->cursor_row + 1; r < TERM_ROWS; r++)
				memset(&term->cells[r], 0,
				       sizeof(term->cells[r]));
		} else if (mode == 1) {
			/* 화면 시작부터 커서까지 */
			for (int r = 0; r < term->cursor_row; r++)
				memset(&term->cells[r], 0,
				       sizeof(term->cells[r]));
			for (int c = 0; c <= term->cursor_col; c++)
				term->cells[term->cursor_row][c].ch = 0;
		} else if (mode == 2) {
			/* 화면 전체 */
			memset(term->cells, 0, sizeof(term->cells));
		}
		break;
	}

	case 'K': /* EL — 줄 지우기 */
	{
		int mode = (np > 0) ? params[0] : 0;

		if (mode == 0) {
			/* 커서부터 줄 끝까지 */
			for (int c = term->cursor_col; c < TERM_COLS; c++)
				term->cells[term->cursor_row][c].ch = 0;
		} else if (mode == 1) {
			/* 줄 시작부터 커서까지 */
			for (int c = 0; c <= term->cursor_col; c++)
				term->cells[term->cursor_row][c].ch = 0;
		} else if (mode == 2) {
			/* 줄 전체 */
			memset(&term->cells[term->cursor_row], 0,
			       sizeof(term->cells[term->cursor_row]));
		}
		break;
	}

	case 'm': /* SGR — 색상/속성 (v1에서는 무시) */
		break;

	default:
		/* 미지원 CSI 명령 — 무시 */
		break;
	}
}

/*
 * 쉘 출력 한 바이트 처리
 *
 * ANSI 상태 기계:
 *   ESC_NORMAL → 일반 문자 처리 또는 제어 문자
 *   ESC_ESC    → ESC 다음 바이트 확인
 *   ESC_CSI    → 파라미터 수집 후 명령 실행
 */
static void term_putchar(struct terminal *term, unsigned char c)
{
	switch (term->esc_state) {
	case ESC_NORMAL:
		if (c == '\033') {
			/* ESC — 이스케이프 시퀀스 시작 */
			term->esc_state = ESC_ESC;
			term->esc_len = 0;
		} else if (c == '\r') {
			/* 캐리지 리턴 — 줄 시작으로 */
			term->cursor_col = 0;
		} else if (c == '\n') {
			/* 줄바꿈 — 다음 줄로 (필요시 스크롤) */
			term->cursor_row++;
			if (term->cursor_row >= TERM_ROWS) {
				term->cursor_row = TERM_ROWS - 1;
				term_scroll_up(term);
			}
		} else if (c == '\t') {
			/* 탭 — 다음 8칸 경계로 */
			term->cursor_col = (term->cursor_col + 8) & ~7;
			if (term->cursor_col >= TERM_COLS)
				term->cursor_col = TERM_COLS - 1;
		} else if (c == '\b') {
			/* 백스페이스 — 한 칸 왼쪽 */
			if (term->cursor_col > 0)
				term->cursor_col--;
		} else if (c == '\a') {
			/* 벨 (BEL) — 무시 */
		} else if (c >= 32 && c < 127) {
			/*
			 * 일반 출력 가능 문자 — 셀에 쓰기
			 *
			 * 셀에 문자를 쓰고 커서를 오른쪽으로.
			 * 줄 끝에 도달하면 다음 줄로 자동 줄바꿈.
			 */
			term->cells[term->cursor_row][term->cursor_col].ch = (char)c;
			term->cursor_col++;
			if (term->cursor_col >= TERM_COLS) {
				term->cursor_col = 0;
				term->cursor_row++;
				if (term->cursor_row >= TERM_ROWS) {
					term->cursor_row = TERM_ROWS - 1;
					term_scroll_up(term);
				}
			}
		}
		/* 기타 제어 문자(0-31) — 무시 */
		break;

	case ESC_ESC:
		if (c == '[') {
			/* CSI 시퀀스 시작 */
			term->esc_state = ESC_CSI;
			term->esc_len = 0;
		} else {
			/* 알 수 없는 ESC 시퀀스 — 무시하고 복귀 */
			term->esc_state = ESC_NORMAL;
		}
		break;

	case ESC_CSI:
		if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
			/* 파라미터 수집 */
			if (term->esc_len < (int)sizeof(term->esc_buf) - 1)
				term->esc_buf[term->esc_len++] = (char)c;
		} else if (c >= '@' && c <= '~') {
			/* 명령 문자 — 실행 후 복귀 */
			term->esc_buf[term->esc_len] = '\0';
			term_execute_csi(term, (char)c);
			term->esc_state = ESC_NORMAL;
		} else {
			/* 예상치 못한 문자 — 시퀀스 포기 */
			term->esc_state = ESC_NORMAL;
		}
		break;
	}
}

/* ============================================================
 * 3. 렌더링
 * ============================================================
 *
 * 터미널 셀 버퍼 → CDP 공유메모리 픽셀 버퍼
 *
 * 매 프레임마다:
 *   1. 전체 배경 채우기 (검정)
 *   2. 각 셀의 문자를 8x8 폰트로 그리기
 *   3. 커서 위치에 블록 커서 그리기
 */
static void term_render(struct terminal *term)
{
	uint32_t *px = term->win->pixels;
	int w = (int)term->win->width;
	int h = (int)term->win->height;

	/* 1. 배경 채우기 */
	for (int i = 0; i < w * h; i++)
		px[i] = COLOR_BG;

	/* 2. 문자 렌더링 */
	for (int row = 0; row < TERM_ROWS; row++) {
		for (int col = 0; col < TERM_COLS; col++) {
			char ch = term->cells[row][col].ch;

			if (ch < 32 || ch > 126)
				continue;

			int px_x = col * 8;
			int px_y = row * 8;
			const uint8_t *glyph = font8x8_basic[(int)ch];

			for (int gy = 0; gy < 8; gy++) {
				int y = px_y + gy;

				if (y >= h)
					break;
				for (int gx = 0; gx < 8; gx++) {
					int x = px_x + gx;

					if (x >= w)
						break;
					if (glyph[gy] & (1 << gx))
						px[y * w + x] = COLOR_FG;
				}
			}
		}
	}

	/* 3. 블록 커서 그리기 */
	int cx = term->cursor_col * 8;
	int cy = term->cursor_row * 8;

	for (int gy = 0; gy < 8; gy++) {
		int y = cy + gy;

		if (y >= h)
			break;
		for (int gx = 0; gx < 8; gx++) {
			int x = cx + gx;

			if (x >= w)
				break;
			px[y * w + x] = COLOR_CURSOR;
		}
	}

	/* 커서 위치의 문자를 반전 (커서 위에 글자 보이게) */
	if (term->cursor_row < TERM_ROWS &&
	    term->cursor_col < TERM_COLS) {
		char ch = term->cells[term->cursor_row]
				     [term->cursor_col].ch;

		if (ch >= 32 && ch <= 126) {
			const uint8_t *glyph = font8x8_basic[(int)ch];

			for (int gy = 0; gy < 8; gy++) {
				int y = cy + gy;

				if (y >= h)
					break;
				for (int gx = 0; gx < 8; gx++) {
					int x = cx + gx;

					if (x >= w)
						break;
					if (glyph[gy] & (1 << gx))
						px[y * w + x] = COLOR_BG;
				}
			}
		}
	}
}

/* ============================================================
 * 4. 키보드 입력 처리
 * ============================================================
 *
 * CDP 키 이벤트를 PTY 마스터에 쓰기
 *
 * 컴포지터가 보내는 CDP_EVT_KEY:
 *   keycode:   Linux 키코드 (KEY_A=30, KEY_ENTER=28 등)
 *   state:     1=pressed, 0=released, 2=repeat
 *   character: Shift/Ctrl 처리된 ASCII (0이면 매핑 불가)
 *
 * PTY에 쓰면 쉘이 읽을 수 있음:
 *   write(pty_master, "a", 1) → 쉘의 stdin에서 read()로 'a' 수신
 */
static void term_handle_key(struct terminal *term,
			    uint32_t keycode, uint32_t state,
			    char character)
{
	/* 키 릴리스(state=0)는 무시 */
	if (state == 0)
		return;

	char buf[8];
	int len = 0;

	switch (keycode) {
	case KEY_ENTER:
		/*
		 * Enter → '\r' (Carriage Return)
		 * 쉘은 \r을 줄 입력 완료로 처리.
		 * PTY의 line discipline이 \r → \r\n 변환을 자동 수행.
		 */
		buf[0] = '\r';
		len = 1;
		break;

	case KEY_BACKSPACE:
		/*
		 * Backspace → DEL(0x7F)
		 * VT100 관례: Backspace는 DEL을 전송.
		 * 쉘(readline 등)이 이 문자를 받으면 마지막 글자를 삭제.
		 */
		buf[0] = '\x7f';
		len = 1;
		break;

	case KEY_TAB:
		buf[0] = '\t';
		len = 1;
		break;

	case KEY_UP:
		/*
		 * 방향키 → VT100 이스케이프 시퀀스
		 * 쉘(readline)이 이 시퀀스를 히스토리 탐색, 커서 이동에 사용.
		 */
		memcpy(buf, "\033[A", 3);
		len = 3;
		break;

	case KEY_DOWN:
		memcpy(buf, "\033[B", 3);
		len = 3;
		break;

	case KEY_RIGHT:
		memcpy(buf, "\033[C", 3);
		len = 3;
		break;

	case KEY_LEFT:
		memcpy(buf, "\033[D", 3);
		len = 3;
		break;

	default:
		/*
		 * character 필드 사용:
		 *   일반 문자 (a-z, A-Z, 0-9, 특수문자)
		 *   Ctrl+문자 → 제어 문자 (1-26)
		 *     Ctrl+C = 0x03 → SIGINT (쉘이 프로세스 중단)
		 *     Ctrl+D = 0x04 → EOF (쉘 종료)
		 *     Ctrl+L = 0x0C → 화면 지우기
		 *   0이면 매핑 불가 (무시)
		 */
		if (character) {
			buf[0] = character;
			len = 1;
		}
		break;
	}

	if (len > 0) {
		ssize_t ret = write(term->pty_master, buf, (size_t)len);
		(void)ret;
	}
}

/* ============================================================
 * 5. PTY 생성 & 쉘 실행
 * ============================================================
 *
 * POSIX PTY API 사용:
 *   posix_openpt() → PTY 마스터 fd
 *   grantpt()      → 슬레이브 권한 설정
 *   unlockpt()     → 슬레이브 잠금 해제
 *   ptsname()      → 슬레이브 경로 (예: /dev/pts/0)
 *
 * 그 후 fork()로 자식 프로세스 생성:
 *   자식: setsid() + 슬레이브 열기 + exec("/bin/sh")
 *   부모: 마스터 fd로 읽기/쓰기
 */
static int term_spawn_shell(struct terminal *term)
{
	/*
	 * PTY 마스터 열기
	 *
	 * O_RDWR: 읽기/쓰기 모두 필요
	 * O_NOCTTY: 이 PTY가 현재 프로세스의 제어 터미널이 되지 않게
	 *           (자식 프로세스에서 setsid 후 별도로 설정)
	 */
	int master = posix_openpt(O_RDWR | O_NOCTTY);

	if (master < 0) {
		perror("posix_openpt");
		return -1;
	}

	if (grantpt(master) < 0 || unlockpt(master) < 0) {
		perror("grantpt/unlockpt");
		close(master);
		return -1;
	}

	char *slave_name = ptsname(master);

	if (!slave_name) {
		perror("ptsname");
		close(master);
		return -1;
	}

	/*
	 * 윈도우 크기 설정 (TIOCSWINSZ)
	 *
	 * 쉘과 프로그램(vi, less 등)이 터미널 크기를 알아야 한다.
	 * stty size, tput cols 등이 이 ioctl을 사용.
	 */
	struct winsize ws;

	ws.ws_row = TERM_ROWS;
	ws.ws_col = TERM_COLS;
	ws.ws_xpixel = TERM_WIDTH;
	ws.ws_ypixel = TERM_HEIGHT;
	ioctl(master, TIOCSWINSZ, &ws);

	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		close(master);
		return -1;
	}

	if (pid == 0) {
		/* === 자식 프로세스 === */
		close(master);

		/*
		 * setsid() — 새 세션 리더가 됨
		 *
		 * fork()된 자식은 부모의 세션에 속한다.
		 * setsid()로 새 세션을 만들면:
		 *   1. 새 세션 + 새 프로세스 그룹의 리더가 됨
		 *   2. 제어 터미널이 없는 상태
		 *   3. 이후 PTY 슬레이브를 열면 그것이 제어 터미널이 됨
		 *
		 * 이것이 Ctrl+C(SIGINT)가 작동하는 핵심:
		 *   제어 터미널에서 인터럽트 → 포그라운드 프로세스 그룹에 SIGINT
		 */
		setsid();

		int slave = open(slave_name, O_RDWR);

		if (slave < 0) {
			perror("open(slave)");
			_exit(1);
		}

		/* 슬레이브를 stdin/stdout/stderr로 설정 */
		dup2(slave, STDIN_FILENO);
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		if (slave > STDERR_FILENO)
			close(slave);

		/* 환경 변수 설정 */
		setenv("TERM", "vt100", 1);
		setenv("HOME", "/root", 1);
		setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
		setenv("PS1", "\\w # ", 1);

		/* 쉘 실행 — citcsh 우선, 없으면 busybox sh 폴백 */
		execl("/bin/citcsh", "citcsh", (char *)NULL);
		execl("/bin/sh", "sh", (char *)NULL);
		perror("execl");
		_exit(1);
	}

	/* === 부모 프로세스 === */
	term->pty_master = master;
	term->child_pid = pid;

	return 0;
}

/* ============================================================
 * 6. 메인 이벤트 루프
 * ============================================================
 *
 * poll()로 두 fd를 동시에 감시:
 *   1. CDP 소켓: 키보드 이벤트, 프레임 콜백
 *   2. PTY 마스터: 쉘 출력
 *
 * cdp_dispatch()를 사용하지 않는 이유:
 *   cdp_dispatch() 내부의 cdp_recv_msg()가 블로킹.
 *   PTY에서 데이터가 와도 처리할 수 없음!
 *   → poll()로 직접 멀티플렉싱.
 */
static void term_event_loop(struct terminal *term)
{
	struct pollfd fds[2];

	fds[0].fd = term->conn->sock_fd;
	fds[0].events = POLLIN;
	fds[1].fd = term->pty_master;
	fds[1].events = POLLIN;

	while (term->running) {
		int ret = poll(fds, 2, 100);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* CDP 소켓에서 이벤트 수신 */
		if (fds[0].revents & POLLIN) {
			uint32_t type, size;
			uint8_t payload[256];

			if (cdp_recv_msg(term->conn->sock_fd, &type,
					 payload, sizeof(payload),
					 &size) < 0) {
				term->running = 0;
				break;
			}

			switch (type) {
			case CDP_EVT_KEY:
			{
				struct cdp_key *key =
					(struct cdp_key *)payload;
				term_handle_key(term, key->keycode,
						key->state,
						(char)key->character);
				break;
			}
			case CDP_EVT_FRAME_DONE:
				if (term->dirty) {
					term_render(term);
					cdp_commit_to(term->conn,
						      term->win);
					term->dirty = 0;
				}
				cdp_request_frame(term->conn, term->win);
				break;
			default:
				break;
			}
		}

		/* PTY에서 쉘 출력 수신 */
		if (fds[1].revents & POLLIN) {
			char buf[512];
			ssize_t n = read(term->pty_master, buf,
					 sizeof(buf));

			if (n > 0) {
				for (ssize_t i = 0; i < n; i++)
					term_putchar(term,
						     (unsigned char)buf[i]);
				term->dirty = 1;
			} else if (n == 0) {
				/* 쉘 종료 */
				term->running = 0;
			}
		}

		/* PTY 에러 (쉘 종료 감지) */
		if (fds[1].revents & (POLLHUP | POLLERR))
			term->running = 0;

		/*
		 * poll 타임아웃이나 PTY 데이터 후 즉시 렌더링
		 *
		 * 프레임 콜백을 기다리지 않고 바로 그리는 "간단한" 방식.
		 * 터미널은 텍스트 갱신이 빈번하므로 즉시 반응이 중요.
		 */
		if (term->dirty) {
			term_render(term);
			cdp_commit_to(term->conn, term->win);
			term->dirty = 0;
		}
	}
}

/* ============================================================
 * 7. 메인 함수
 * ============================================================ */

int main(void)
{
	struct terminal term;

	memset(&term, 0, sizeof(term));
	term.running = 1;
	term.dirty = 1;

	printf("\n=== CITC Terminal Emulator ===\n\n");

	/* 1. 컴포지터에 연결 */
	printf("[1/3] 컴포지터에 연결...\n");
	term.conn = cdp_connect();
	if (!term.conn) {
		fprintf(stderr, "  컴포지터 연결 실패!\n");
		fprintf(stderr, "  compositor가 실행 중인지 확인하세요.\n");
		return 1;
	}
	printf("  연결 성공 (화면: %ux%u)\n",
	       term.conn->screen_width, term.conn->screen_height);

	/* 2. 터미널 윈도우 생성 */
	printf("[2/3] 터미널 윈도우 생성 (%dx%d = %dx%d chars)...\n",
	       TERM_WIDTH, TERM_HEIGHT, TERM_COLS, TERM_ROWS);
	term.win = cdp_create_surface(term.conn,
				      TERM_WIDTH, TERM_HEIGHT,
				      "citcterm");
	if (!term.win) {
		fprintf(stderr, "  윈도우 생성 실패!\n");
		cdp_disconnect(term.conn);
		return 1;
	}
	printf("  윈도우 생성 완료 (surface_id=%u)\n",
	       term.win->surface_id);

	/* 3. 쉘 프로세스 시작 */
	printf("[3/3] 쉘 프로세스 시작 (/bin/sh)...\n");
	if (term_spawn_shell(&term) < 0) {
		fprintf(stderr, "  쉘 시작 실패!\n");
		cdp_destroy_surface(term.conn, term.win);
		cdp_disconnect(term.conn);
		return 1;
	}
	printf("  쉘 PID=%d\n", term.child_pid);

	/* 첫 프레임 렌더링 */
	term_render(&term);
	cdp_commit_to(term.conn, term.win);
	cdp_request_frame(term.conn, term.win);
	term.dirty = 0;

	printf("\ncitcterm 시작! 터미널 윈도우를 클릭하여 포커스를 설정하세요.\n\n");

	/* 이벤트 루프 */
	term_event_loop(&term);

	/* 정리 */
	printf("\ncitcterm 종료.\n");
	close(term.pty_master);

	/* 자식 프로세스 회수 */
	if (term.child_pid > 0) {
		kill(term.child_pid, SIGTERM);
		waitpid(term.child_pid, NULL, WNOHANG);
	}

	cdp_destroy_surface(term.conn, term.win);
	cdp_disconnect(term.conn);

	return 0;
}
