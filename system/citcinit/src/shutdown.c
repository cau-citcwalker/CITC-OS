/*
 * shutdown - CITC OS 시스템 종료 명령어
 * ======================================
 *
 * 이 프로그램은 PID 1(citcinit)에 시그널을 보내서
 * 시스템을 안전하게 종료하거나 재부팅합니다.
 *
 * 동작 원리:
 *   1. 사용자가 쉘에서 "shutdown" 또는 "reboot" 입력
 *   2. 이 프로그램이 PID 1에 시그널 전송 (kill(1, signal))
 *   3. citcinit이 시그널을 받아서 do_shutdown() 실행
 *   4. 서비스 정지 → SIGTERM → SIGKILL → sync → unmount → reboot()
 *
 * 시그널 매핑:
 *   SIGTERM → 전원 끄기 (power off)
 *   SIGINT  → 재부팅 (restart)
 *   SIGUSR1 → 시스템 정지 (halt, 전원은 안 꺼짐)
 *
 * argv[0] 트릭:
 *   Unix에서 프로그램은 자신이 어떤 이름으로 실행되었는지 알 수 있습니다.
 *   argv[0]에 실행 시 사용된 이름이 들어있기 때문입니다.
 *
 *   예: /sbin/shutdown 실행파일에 심볼릭 링크를 만들면:
 *     /sbin/reboot → /sbin/shutdown
 *     /sbin/poweroff → /sbin/shutdown
 *     /sbin/halt → /sbin/shutdown
 *
 *   argv[0]이 "reboot"이면 재부팅, "poweroff"이면 전원끄기.
 *   busybox도 같은 원리로 동작합니다 (하나의 바이너리, 수백 개의 심링크).
 *
 * 사용법:
 *   shutdown           → 전원 끄기
 *   shutdown -r        → 재부팅
 *   shutdown -h        → 전원 끄기 (halt)
 *   reboot             → 재부팅 (심링크)
 *   poweroff           → 전원 끄기 (심링크)
 *   halt               → 시스템 정지 (심링크)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>     /* basename() — 경로에서 파일 이름만 추출 */

/*
 * ANSI 색상 코드
 * 터미널에서 중요한 메시지를 강조하기 위해 사용
 */
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BOLD   "\033[1m"
#define COLOR_RESET  "\033[0m"

/*
 * 종료 모드 열거형
 *
 * enum을 사용하면 매직 넘버 대신 의미 있는 이름을 쓸 수 있습니다.
 * 코드 가독성에 크게 도움됩니다.
 */
enum shutdown_mode {
	MODE_POWEROFF,  /* 전원 끄기 */
	MODE_REBOOT,    /* 재부팅 */
	MODE_HALT,      /* 시스템 정지 (전원은 유지) */
};

static void usage(const char *prog)
{
	printf("사용법: %s [옵션]\n", prog);
	printf("\n");
	printf("옵션:\n");
	printf("  -h, --halt      시스템 정지 (전원 유지)\n");
	printf("  -p, --poweroff  전원 끄기 (기본값)\n");
	printf("  -r, --reboot    재부팅\n");
	printf("  --help          이 도움말 표시\n");
	printf("\n");
	printf("심볼릭 링크:\n");
	printf("  reboot   → shutdown -r\n");
	printf("  poweroff → shutdown -p\n");
	printf("  halt     → shutdown -h\n");
}

int main(int argc, char *argv[])
{
	enum shutdown_mode mode = MODE_POWEROFF;
	int signal_to_send;
	const char *action_str;

	/*
	 * argv[0]에서 프로그램 이름 추출
	 *
	 * basename()은 경로에서 파일 이름 부분만 반환합니다.
	 *   "/sbin/reboot" → "reboot"
	 *   "./shutdown"   → "shutdown"
	 *
	 * strdup()으로 복사하는 이유:
	 *   일부 구현에서 basename()이 원본 문자열을 수정할 수 있음.
	 *   안전하게 복사본을 만들어 사용.
	 */
	char *prog_copy = strdup(argv[0]);
	const char *prog = basename(prog_copy);

	/*
	 * 프로그램 이름으로 모드 결정 (argv[0] 트릭)
	 *
	 * strcmp()는 두 문자열이 같으면 0을 반환합니다.
	 * C에서 0은 "false"이므로, !strcmp()는 "같을 때 true"가 됩니다.
	 */
	if (strcmp(prog, "reboot") == 0) {
		mode = MODE_REBOOT;
	} else if (strcmp(prog, "poweroff") == 0) {
		mode = MODE_POWEROFF;
	} else if (strcmp(prog, "halt") == 0) {
		mode = MODE_HALT;
	}

	/*
	 * 명령줄 인자 파싱
	 *
	 * argv[0]: 프로그램 이름
	 * argv[1], argv[2], ...: 인자들
	 *
	 * getopt()를 쓸 수도 있지만, 여기서는 간단하게 직접 파싱합니다.
	 * 옵션이 몇 개 안 될 때는 이 방식이 더 명확합니다.
	 */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0 ||
		    strcmp(argv[i], "--reboot") == 0) {
			mode = MODE_REBOOT;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--halt") == 0) {
			mode = MODE_HALT;
		} else if (strcmp(argv[i], "-p") == 0 ||
			   strcmp(argv[i], "--poweroff") == 0) {
			mode = MODE_POWEROFF;
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(prog);
			free(prog_copy);
			return 0;
		} else {
			fprintf(stderr, "알 수 없는 옵션: %s\n", argv[i]);
			usage(prog);
			free(prog_copy);
			return 1;
		}
	}

	/*
	 * 모드에 따라 시그널과 메시지 결정
	 *
	 * PID 1(citcinit)의 시그널 핸들러 매핑:
	 *   SIGTERM  → shutdown_cmd = LINUX_REBOOT_CMD_POWER_OFF
	 *   SIGINT   → shutdown_cmd = LINUX_REBOOT_CMD_RESTART
	 *   SIGUSR1  → shutdown_cmd = LINUX_REBOOT_CMD_HALT
	 */
	switch (mode) {
	case MODE_REBOOT:
		signal_to_send = SIGINT;
		action_str = "재부팅";
		break;
	case MODE_HALT:
		signal_to_send = SIGUSR1;
		action_str = "시스템 정지";
		break;
	case MODE_POWEROFF:
	default:
		signal_to_send = SIGTERM;
		action_str = "전원 끄기";
		break;
	}

	/*
	 * 종료 메시지 출력
	 *
	 * 사용자에게 무슨 일이 일어나는지 알려주는 것이 중요합니다.
	 * 특히 시스템 종료처럼 중대한 작업은 명확한 피드백이 필요.
	 */
	printf("\n");
	printf(COLOR_BOLD COLOR_YELLOW);
	printf("  *** 시스템 %s ***\n", action_str);
	printf(COLOR_RESET);
	printf("\n");

	/*
	 * PID 1에 시그널 전송
	 *
	 * kill(pid, signal):
	 *   pid = 1  → PID 1(init)에 시그널 전송
	 *   signal   → 전송할 시그널 번호
	 *
	 * 반환값:
	 *   0  → 성공
	 *   -1 → 실패 (errno에 원인)
	 *
	 * 실패할 수 있는 경우:
	 *   - EPERM: 권한 없음 (root만 PID 1에 시그널 전송 가능)
	 *   - ESRCH: PID 1이 존재하지 않음 (이러면 안 됨)
	 */
	if (kill(1, signal_to_send) < 0) {
		fprintf(stderr,
			COLOR_RED "오류: PID 1에 시그널 전송 실패\n" COLOR_RESET);
		perror("kill");
		fprintf(stderr, "\n");
		fprintf(stderr, "  가능한 원인:\n");
		fprintf(stderr, "  - root 권한이 필요합니다\n");
		fprintf(stderr, "  - init 시스템이 실행 중이지 않습니다\n");
		free(prog_copy);
		return 1;
	}

	/*
	 * 시그널을 보낸 후에는 citcinit이 처리합니다.
	 * 이 프로세스는 citcinit의 do_shutdown()에서
	 * kill(-1, SIGTERM)으로 종료됩니다.
	 *
	 * 혹시 종료가 안 되는 경우를 대비해 무한 대기.
	 * (정상적이면 여기에 도달하기 전에 시스템이 꺼짐)
	 */
	printf("  citcinit에 %s 요청 전송 완료\n", action_str);
	printf("  시스템이 곧 %s됩니다...\n", action_str);

	/* citcinit이 우리를 죽일 때까지 대기 */
	for (;;)
		sleep(1);

	/* UNREACHABLE */
	free(prog_copy);
	return 0;
}
