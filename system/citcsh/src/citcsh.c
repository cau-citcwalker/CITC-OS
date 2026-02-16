/*
 * citcsh — CITC OS Custom Shell
 * ==============================
 *
 * 커스텀 UNIX 쉘 — bash, zsh, fish의 원리를 직접 구현.
 *
 * 쉘이란?
 *   사용자가 입력한 문자열을 받아서:
 *     1. 토큰으로 분리 (토크나이저/렉서)
 *     2. 파이프라인 구조로 파싱 (파서)
 *     3. fork() + exec()으로 실행 (실행 엔진)
 *
 * 지원 기능:
 *   - 명령어 파싱 (공백 분리, 따옴표 처리)
 *   - 파이프: ls | grep foo | wc -l
 *   - 리다이렉션: > >> < 2>
 *   - 환경변수: $HOME, export VAR=value
 *   - 백그라운드 실행: cmd &
 *   - 빌트인 명령: cd, exit, export, echo, pwd, history, help
 *   - 시그널 처리: Ctrl+C는 자식만 종료, 쉘은 유지
 *
 * 사용법:
 *   compositor &
 *   sleep 2
 *   citcterm        # citcsh가 자동 실행됨
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

/* ============================================================
 * 상수 & 데이터 구조
 * ============================================================
 *
 * 쉘의 핵심 자료구조는 3단계로 구성:
 *
 *   입력 문자열 → token 배열 → pipeline 구조체
 *   "ls -l | grep foo"
 *     → [WORD:"ls"] [WORD:"-l"] [PIPE] [WORD:"grep"] [WORD:"foo"]
 *       → Pipeline{ cmd[0]={"ls","-l"}, cmd[1]={"grep","foo"} }
 */

#define MAX_LINE     1024    /* 입력 줄 최대 길이 */
#define MAX_TOKENS   128     /* 한 줄의 최대 토큰 수 */
#define MAX_ARGS     64      /* 한 명령의 최대 인자 수 */
#define MAX_CMDS     16      /* 파이프라인 최대 명령 수 */
#define HISTORY_SIZE 64      /* 히스토리 저장 개수 */

/*
 * 토큰 타입 — 렉서가 인식하는 단위
 *
 * 모든 쉘(bash, zsh)은 입력을 먼저 토큰으로 분리한다.
 * 토큰은 "단어"와 "연산자" 두 종류:
 *   단어: 명령이름, 인자, 파일이름 (ls, -l, foo.txt)
 *   연산자: |, >, >>, <, 2>, &
 */
enum token_type {
	TOK_WORD,        /* 일반 단어 (명령, 인자, 파일이름) */
	TOK_PIPE,        /* | — 파이프 연산자 */
	TOK_REDIR_OUT,   /* > — stdout을 파일로 */
	TOK_REDIR_APP,   /* >> — stdout을 파일에 추가 */
	TOK_REDIR_IN,    /* < — stdin을 파일에서 */
	TOK_REDIR_ERR,   /* 2> — stderr를 파일로 */
	TOK_BACKGROUND,  /* & — 백그라운드 실행 */
	TOK_EOF,         /* 줄 끝 */
};

struct token {
	enum token_type type;
	char *value;     /* TOK_WORD일 때 문자열 포인터 */
};

/*
 * 명령 구조체 — 파이프라인의 한 단위
 *
 * "grep -i foo < input.txt > output.txt"는:
 *   argv = {"grep", "-i", "foo", NULL}
 *   redir_in = "input.txt"
 *   redir_out = "output.txt"
 */
struct command {
	char *argv[MAX_ARGS];  /* execvp에 전달할 인자 배열 (NULL 종료) */
	int argc;              /* 인자 개수 */
	char *redir_in;        /* < 파일 */
	char *redir_out;       /* > 파일 */
	char *redir_append;    /* >> 파일 */
	char *redir_err;       /* 2> 파일 */
};

/*
 * 파이프라인 — 파이프로 연결된 명령 체인
 *
 * "ls | grep foo | wc -l"은:
 *   cmds[0] = {"ls"}, cmds[1] = {"grep","foo"}, cmds[2] = {"wc","-l"}
 *   num_cmds = 3
 */
struct pipeline {
	struct command cmds[MAX_CMDS];
	int num_cmds;
	int background;  /* &가 붙었는지 */
};

/* 전역 상태 */
static int last_exit_code;                    /* $? — 마지막 종료 코드 */
static int shell_running = 1;                 /* 메인 루프 제어 */
static char *history[HISTORY_SIZE];           /* 히스토리 링 버퍼 */
static int history_count;                     /* 총 저장된 명령 수 */

/* ============================================================
 * 1. 환경변수 확장
 * ============================================================
 *
 * $HOME → /root, $PATH → /bin:/sbin:..., $? → 종료코드
 *
 * bash에서 "echo $HOME"을 치면 "/root"가 출력되는 원리:
 * 쉘이 exec 전에 $HOME을 실제 값으로 치환(expand)한다.
 * 프로그램(echo)은 이미 치환된 인자를 받을 뿐이다.
 */
static void expand_env_vars(char *dst, const char *src, size_t dst_size)
{
	size_t di = 0;

	while (*src && di < dst_size - 1) {
		if (*src == '$') {
			src++;

			/* $? — 마지막 종료 코드 (특수 변수) */
			if (*src == '?') {
				src++;
				di += (size_t)snprintf(dst + di,
						       dst_size - di,
						       "%d", last_exit_code);
				continue;
			}

			/* $VAR_NAME — 환경변수 이름 추출 */
			const char *start = src;

			while (*src && (*src == '_' ||
			       (*src >= 'a' && *src <= 'z') ||
			       (*src >= 'A' && *src <= 'Z') ||
			       (*src >= '0' && *src <= '9')))
				src++;

			if (src > start) {
				/* 변수 이름을 임시 버퍼에 복사 */
				size_t name_len = (size_t)(src - start);
				char name[256];

				if (name_len >= sizeof(name))
					name_len = sizeof(name) - 1;
				memcpy(name, start, name_len);
				name[name_len] = '\0';

				const char *val = getenv(name);

				if (val) {
					size_t vlen = strlen(val);

					if (di + vlen < dst_size) {
						memcpy(dst + di, val, vlen);
						di += vlen;
					}
				}
			} else {
				/* 단독 $ — 그대로 출력 */
				dst[di++] = '$';
			}
		} else {
			dst[di++] = *src++;
		}
	}

	dst[di] = '\0';
}

/* ============================================================
 * 2. 토크나이저 (Lexer)
 * ============================================================
 *
 * 입력 문자열을 토큰 배열로 분리한다.
 *
 * "ls -l | grep "hello world" > out.txt"
 * → [WORD:"ls"] [WORD:"-l"] [PIPE] [WORD:"grep"]
 *   [WORD:"hello world"] [REDIR_OUT] [WORD:"out.txt"]
 *
 * 규칙:
 *   - 공백은 토큰 구분자
 *   - 큰따옴표(") 안의 공백은 보존
 *   - 작은따옴표(') 안의 공백은 보존, $확장 없음
 *   - |, >, >>, <, 2>, & 는 특수 토큰
 *
 * 이 과정을 "렉싱(lexing)" 또는 "토큰화(tokenization)"라고 한다.
 * 컴파일러도 소스코드를 먼저 토큰으로 분리하는데, 같은 원리이다.
 */
static int tokenize(char *line, struct token *tokens, int max_tokens)
{
	int count = 0;
	char *p = line;

	while (*p && count < max_tokens - 1) {
		/* 공백 건너뛰기 */
		while (*p == ' ' || *p == '\t')
			p++;

		if (!*p || *p == '\n')
			break;

		/* 주석 (#) — 나머지 무시 */
		if (*p == '#')
			break;

		/* 2> — stderr 리다이렉션 (2글자 연산자) */
		if (*p == '2' && *(p + 1) == '>') {
			tokens[count].type = TOK_REDIR_ERR;
			tokens[count].value = NULL;
			count++;
			p += 2;
			continue;
		}

		/* >> — 추가 모드 리다이렉션 (2글자 연산자) */
		if (*p == '>' && *(p + 1) == '>') {
			tokens[count].type = TOK_REDIR_APP;
			tokens[count].value = NULL;
			count++;
			p += 2;
			continue;
		}

		/* 1글자 연산자: |, >, <, & */
		if (*p == '|') {
			tokens[count].type = TOK_PIPE;
			tokens[count].value = NULL;
			count++;
			p++;
			continue;
		}

		if (*p == '>') {
			tokens[count].type = TOK_REDIR_OUT;
			tokens[count].value = NULL;
			count++;
			p++;
			continue;
		}

		if (*p == '<') {
			tokens[count].type = TOK_REDIR_IN;
			tokens[count].value = NULL;
			count++;
			p++;
			continue;
		}

		if (*p == '&') {
			tokens[count].type = TOK_BACKGROUND;
			tokens[count].value = NULL;
			count++;
			p++;
			continue;
		}

		/*
		 * 단어 토큰 — 따옴표 처리 포함
		 *
		 * 따옴표 안에서는 공백이 구분자가 아니다:
		 *   echo "hello world" → 인자 1개: "hello world"
		 *   echo hello world   → 인자 2개: "hello", "world"
		 *
		 * 이것이 쉘에서 따옴표가 중요한 이유이다.
		 */
		char *start = p;

		if (*p == '"' || *p == '\'') {
			char quote = *p++;

			/* 여는 따옴표 위치에 직접 문자열 쓰기 (in-place) */
			start = p;
			while (*p && *p != quote)
				p++;
			if (*p)
				*p++ = '\0'; /* 닫는 따옴표를 null로 */
		} else {
			/* 일반 단어: 공백이나 특수문자까지 */
			while (*p && *p != ' ' && *p != '\t' &&
			       *p != '\n' && *p != '|' && *p != '>' &&
			       *p != '<' && *p != '&' && *p != '#')
				p++;
			if (*p && *p != '\n') {
				/* 특수문자 앞에서 멈춘 경우, null 삽입은 안 함 */
				if (*p == ' ' || *p == '\t')
					*p++ = '\0';
			} else if (*p == '\n') {
				*p = '\0';
			}
		}

		tokens[count].type = TOK_WORD;
		tokens[count].value = start;
		count++;
	}

	tokens[count].type = TOK_EOF;
	tokens[count].value = NULL;
	return count;
}

/* ============================================================
 * 3. 파서
 * ============================================================
 *
 * 토큰 배열을 파이프라인 구조체로 변환.
 *
 * [WORD:"ls"] [WORD:"-l"] [PIPE] [WORD:"grep"] [WORD:"foo"] [REDIR_OUT] [WORD:"out.txt"]
 *   → Pipeline {
 *       cmds[0] = {argv={"ls","-l"}, redir=없음}
 *       cmds[1] = {argv={"grep","foo"}, redir_out="out.txt"}
 *       num_cmds = 2
 *     }
 *
 * 파서의 핵심 규칙:
 *   - WORD → 현재 명령의 argv에 추가
 *   - PIPE → 새 명령 시작
 *   - >, >>, <, 2> → 다음 WORD를 리다이렉션 파일로
 *   - & → 백그라운드 플래그 설정
 */
static int parse_pipeline(struct token *tokens, int num_tokens,
			  struct pipeline *pl)
{
	memset(pl, 0, sizeof(*pl));

	struct command *cur = &pl->cmds[0];

	pl->num_cmds = 1;

	for (int i = 0; i < num_tokens; i++) {
		struct token *t = &tokens[i];

		switch (t->type) {
		case TOK_WORD:
			if (cur->argc < MAX_ARGS - 1) {
				cur->argv[cur->argc++] = t->value;
			}
			break;

		case TOK_PIPE:
			/*
			 * 파이프: 새 명령 시작
			 * "ls | grep" → cmds[0]=ls, cmds[1]=grep
			 */
			cur->argv[cur->argc] = NULL;
			if (pl->num_cmds >= MAX_CMDS) {
				fprintf(stderr, "citcsh: 파이프 체인 너무 김\n");
				return -1;
			}
			cur = &pl->cmds[pl->num_cmds++];
			break;

		case TOK_REDIR_OUT:
			if (i + 1 < num_tokens &&
			    tokens[i + 1].type == TOK_WORD) {
				cur->redir_out = tokens[++i].value;
			} else {
				fprintf(stderr, "citcsh: > 뒤에 파일명 필요\n");
				return -1;
			}
			break;

		case TOK_REDIR_APP:
			if (i + 1 < num_tokens &&
			    tokens[i + 1].type == TOK_WORD) {
				cur->redir_append = tokens[++i].value;
			} else {
				fprintf(stderr, "citcsh: >> 뒤에 파일명 필요\n");
				return -1;
			}
			break;

		case TOK_REDIR_IN:
			if (i + 1 < num_tokens &&
			    tokens[i + 1].type == TOK_WORD) {
				cur->redir_in = tokens[++i].value;
			} else {
				fprintf(stderr, "citcsh: < 뒤에 파일명 필요\n");
				return -1;
			}
			break;

		case TOK_REDIR_ERR:
			if (i + 1 < num_tokens &&
			    tokens[i + 1].type == TOK_WORD) {
				cur->redir_err = tokens[++i].value;
			} else {
				fprintf(stderr, "citcsh: 2> 뒤에 파일명 필요\n");
				return -1;
			}
			break;

		case TOK_BACKGROUND:
			pl->background = 1;
			break;

		case TOK_EOF:
			break;
		}
	}

	/* 마지막 명령의 argv null 종료 */
	cur->argv[cur->argc] = NULL;
	return 0;
}

/* ============================================================
 * 4. 빌트인 명령
 * ============================================================
 *
 * 빌트인(built-in)은 쉘 프로세스 내부에서 직접 실행되는 명령이다.
 *
 * 왜 빌트인이 필요한가?
 *   cd는 fork+exec으로 실행하면 자식 프로세스의 디렉토리만 바뀌고
 *   쉘(부모)의 디렉토리는 안 바뀐다! 따라서 cd는 반드시 쉘 자체에서
 *   chdir()을 호출해야 한다.
 *
 *   exit, export도 같은 이유 — 쉘 자체의 상태를 변경해야 하므로.
 */

/* cd — 디렉토리 변경 */
static int builtin_cd(char **argv)
{
	const char *dir = argv[1];

	if (!dir)
		dir = getenv("HOME");
	if (!dir)
		dir = "/";

	if (chdir(dir) < 0) {
		fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
		return 1;
	}

	/* PWD 환경변수 갱신 */
	char cwd[512];

	if (getcwd(cwd, sizeof(cwd)))
		setenv("PWD", cwd, 1);

	return 0;
}

/* exit — 쉘 종료 */
static int builtin_exit(char **argv)
{
	int code = 0;

	if (argv[1])
		code = atoi(argv[1]);

	shell_running = 0;
	return code;
}

/*
 * export — 환경변수 설정
 *
 * "export VAR=value" → setenv("VAR", "value", 1)
 *
 * 환경변수는 fork()시 자식에게 상속된다.
 * 그래서 export한 변수는 이후 실행하는 모든 명령에서 사용 가능.
 * (쉘 내부에서 setenv()로 설정하면 자동으로 자식에게 전달됨)
 */
static int builtin_export(char **argv)
{
	if (!argv[1]) {
		/* 인자 없이 export — 현재 환경변수 목록 출력 */
		extern char **environ;

		for (char **env = environ; *env; env++)
			printf("%s\n", *env);
		return 0;
	}

	for (int i = 1; argv[i]; i++) {
		char *eq = strchr(argv[i], '=');

		if (eq) {
			*eq = '\0';
			setenv(argv[i], eq + 1, 1);
			*eq = '=';
		} else {
			/* "export VAR" — 이미 존재하면 유지 */
			if (!getenv(argv[i]))
				setenv(argv[i], "", 1);
		}
	}

	return 0;
}

/*
 * echo — 문자열 출력
 *
 * echo는 외부 명령(/bin/echo)으로도 존재하지만,
 * 쉘 빌트인으로 구현하면 fork() 오버헤드를 줄일 수 있다.
 * bash에서도 echo는 빌트인이다.
 */
static int builtin_echo(char **argv)
{
	int no_newline = 0;
	int start = 1;

	if (argv[1] && strcmp(argv[1], "-n") == 0) {
		no_newline = 1;
		start = 2;
	}

	for (int i = start; argv[i]; i++) {
		if (i > start)
			putchar(' ');
		fputs(argv[i], stdout);
	}

	if (!no_newline)
		putchar('\n');

	fflush(stdout);
	return 0;
}

/* pwd — 현재 디렉토리 출력 */
static int builtin_pwd(char **argv)
{
	(void)argv;
	char cwd[512];

	if (getcwd(cwd, sizeof(cwd))) {
		printf("%s\n", cwd);
	} else {
		perror("pwd");
		return 1;
	}

	return 0;
}

/* history — 히스토리 출력 */
static int builtin_history(char **argv)
{
	(void)argv;

	int start = 0;

	if (history_count > HISTORY_SIZE)
		start = history_count - HISTORY_SIZE;

	for (int i = start; i < history_count; i++) {
		int idx = i % HISTORY_SIZE;

		printf("  %d  %s\n", i + 1, history[idx]);
	}

	return 0;
}

/* help — 빌트인 목록 */
static int builtin_help(char **argv)
{
	(void)argv;

	printf("citcsh — CITC OS Shell\n");
	printf("\n");
	printf("빌트인 명령:\n");
	printf("  cd [dir]          디렉토리 변경\n");
	printf("  pwd               현재 디렉토리 출력\n");
	printf("  echo [-n] ...     문자열 출력\n");
	printf("  export [VAR=val]  환경변수 설정\n");
	printf("  history           명령 히스토리\n");
	printf("  help              이 도움말\n");
	printf("  exit [code]       쉘 종료\n");
	printf("\n");
	printf("연산자:\n");
	printf("  cmd1 | cmd2       파이프\n");
	printf("  cmd > file        stdout → 파일\n");
	printf("  cmd >> file       stdout → 파일 (추가)\n");
	printf("  cmd < file        stdin ← 파일\n");
	printf("  cmd 2> file       stderr → 파일\n");
	printf("  cmd &             백그라운드 실행\n");
	printf("\n");
	printf("변수: $VAR, $?, $HOME, $PATH\n");

	return 0;
}

/* 빌트인 여부 확인 */
static int is_builtin(const char *cmd)
{
	static const char *builtins[] = {
		"cd", "exit", "export", "echo", "pwd", "history", "help",
		NULL,
	};

	for (int i = 0; builtins[i]; i++) {
		if (strcmp(cmd, builtins[i]) == 0)
			return 1;
	}

	return 0;
}

/* 빌트인 실행 분기 */
static int run_builtin(char **argv)
{
	if (strcmp(argv[0], "cd") == 0)
		return builtin_cd(argv);
	if (strcmp(argv[0], "exit") == 0)
		return builtin_exit(argv);
	if (strcmp(argv[0], "export") == 0)
		return builtin_export(argv);
	if (strcmp(argv[0], "echo") == 0)
		return builtin_echo(argv);
	if (strcmp(argv[0], "pwd") == 0)
		return builtin_pwd(argv);
	if (strcmp(argv[0], "history") == 0)
		return builtin_history(argv);
	if (strcmp(argv[0], "help") == 0)
		return builtin_help(argv);

	return 1;
}

/* ============================================================
 * 5. 리다이렉션 설정
 * ============================================================
 *
 * fork() 후 자식 프로세스에서 호출.
 * open()으로 파일을 열고, dup2()로 fd를 교체한다.
 *
 * dup2(fd, STDOUT_FILENO)의 의미:
 *   "stdout이 가리키는 곳을 fd가 가리키는 곳으로 바꿔라"
 *   → 이후 printf(), write(1,...) 등이 모두 파일로 간다.
 *
 * 이것이 쉘 리다이렉션의 원리이다:
 *   echo hello > out.txt
 *   → fork() → 자식에서 open("out.txt") → dup2(fd,1) → exec("echo","hello")
 *   → echo는 자기가 stdout에 쓰는 줄 알지만, 실제로는 파일에 쓴다!
 */
static int setup_redirections(struct command *cmd)
{
	if (cmd->redir_in) {
		int fd = open(cmd->redir_in, O_RDONLY);

		if (fd < 0) {
			fprintf(stderr, "citcsh: %s: %s\n",
				cmd->redir_in, strerror(errno));
			return -1;
		}
		dup2(fd, STDIN_FILENO);
		close(fd);
	}

	if (cmd->redir_out) {
		int fd = open(cmd->redir_out,
			      O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd < 0) {
			fprintf(stderr, "citcsh: %s: %s\n",
				cmd->redir_out, strerror(errno));
			return -1;
		}
		dup2(fd, STDOUT_FILENO);
		close(fd);
	}

	if (cmd->redir_append) {
		int fd = open(cmd->redir_append,
			      O_WRONLY | O_CREAT | O_APPEND, 0644);

		if (fd < 0) {
			fprintf(stderr, "citcsh: %s: %s\n",
				cmd->redir_append, strerror(errno));
			return -1;
		}
		dup2(fd, STDOUT_FILENO);
		close(fd);
	}

	if (cmd->redir_err) {
		int fd = open(cmd->redir_err,
			      O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd < 0) {
			fprintf(stderr, "citcsh: %s: %s\n",
				cmd->redir_err, strerror(errno));
			return -1;
		}
		dup2(fd, STDERR_FILENO);
		close(fd);
	}

	return 0;
}

/* ============================================================
 * 6. 파이프라인 실행 엔진
 * ============================================================
 *
 * 쉘의 핵심 중의 핵심 — 파이프라인 실행.
 *
 * "ls | grep foo | wc -l" 실행 과정:
 *
 *   1. pipe() 2번 호출 → pipe_fds[0], pipe_fds[1]
 *
 *   2. fork() 3번:
 *      자식 0 (ls):    stdout → pipe_fds[0][1]
 *      자식 1 (grep):  stdin  ← pipe_fds[0][0], stdout → pipe_fds[1][1]
 *      자식 2 (wc):    stdin  ← pipe_fds[1][0]
 *
 *   3. 부모: 모든 pipe fd 닫기 → 자식들 waitpid()
 *
 * 핵심 원리:
 *   pipe()가 만드는 건 커널 내부의 4KB 버퍼.
 *   한쪽(fd[1])에 write하면 다른쪽(fd[0])에서 read 가능.
 *   ls의 stdout을 grep의 stdin에 연결하는 것이 이 원리.
 *
 * fork() + exec()의 의미:
 *   fork()  = 현재 프로세스의 복제본 생성
 *   exec()  = 복제본의 코드를 새 프로그램으로 교체
 *   → 결과: 새 프로세스에서 새 프로그램 실행
 *
 *   왜 이렇게 2단계?
 *   fork()와 exec() 사이에 파이프/리다이렉션을 설정할 수 있기 때문!
 *   (Windows의 CreateProcess()는 이 유연성이 없다)
 */
static int execute_pipeline(struct pipeline *pl)
{
	/*
	 * 빌트인: 파이프 없는 단일 명령만 빌트인으로 실행.
	 *
	 * 주의: 빌트인도 리다이렉션이 가능하다!
	 *   echo hello > out.txt   ← echo는 빌트인이지만 리다이렉션 필요
	 *
	 * 리다이렉션이 있으면 원래 fd를 저장 → 리다이렉션 적용 →
	 * 빌트인 실행 → 원래 fd 복원하는 과정이 필요하다.
	 * (fork하면 자식에서만 fd가 바뀌지만, 빌트인은 부모에서 실행되므로)
	 */
	if (pl->num_cmds == 1 && !pl->background &&
	    pl->cmds[0].argc > 0 && is_builtin(pl->cmds[0].argv[0])) {
		struct command *cmd = &pl->cmds[0];
		int saved_in = -1, saved_out = -1, saved_err = -1;
		int has_redir = (cmd->redir_in || cmd->redir_out ||
				 cmd->redir_append || cmd->redir_err);

		if (has_redir) {
			saved_in = dup(STDIN_FILENO);
			saved_out = dup(STDOUT_FILENO);
			saved_err = dup(STDERR_FILENO);
			if (setup_redirections(cmd) < 0) {
				if (saved_in >= 0) close(saved_in);
				if (saved_out >= 0) close(saved_out);
				if (saved_err >= 0) close(saved_err);
				last_exit_code = 1;
				return 1;
			}
		}

		last_exit_code = run_builtin(cmd->argv);
		fflush(stdout);

		if (has_redir) {
			dup2(saved_in, STDIN_FILENO);
			dup2(saved_out, STDOUT_FILENO);
			dup2(saved_err, STDERR_FILENO);
			close(saved_in);
			close(saved_out);
			close(saved_err);
		}

		return last_exit_code;
	}

	int num = pl->num_cmds;

	/*
	 * N-1개의 파이프 생성
	 *
	 * pipe(fds)는 fds[0]=읽기 끝, fds[1]=쓰기 끝 반환.
	 * 명령 i의 stdout → pipe_fds[i][1]
	 * 명령 i+1의 stdin ← pipe_fds[i][0]
	 */
	int pipe_fds[MAX_CMDS][2];

	for (int i = 0; i < num - 1; i++) {
		if (pipe(pipe_fds[i]) < 0) {
			perror("pipe");
			return 1;
		}
	}

	pid_t pids[MAX_CMDS];

	for (int i = 0; i < num; i++) {
		pids[i] = fork();

		if (pids[i] < 0) {
			perror("fork");
			return 1;
		}

		if (pids[i] == 0) {
			/* === 자식 프로세스 === */

			/*
			 * SIGINT를 기본 동작으로 복원.
			 * 부모(쉘)는 SIGINT를 무시하지만,
			 * 자식(실행 중인 명령)은 Ctrl+C로 종료되어야 한다.
			 */
			signal(SIGINT, SIG_DFL);

			/*
			 * 파이프 연결
			 *
			 * 첫 번째가 아니면: stdin을 이전 파이프에서 읽기
			 * 마지막이 아니면: stdout을 다음 파이프에 쓰기
			 */
			if (i > 0)
				dup2(pipe_fds[i - 1][0], STDIN_FILENO);
			if (i < num - 1)
				dup2(pipe_fds[i][1], STDOUT_FILENO);

			/* 모든 파이프 fd 닫기 (중요!) */
			for (int j = 0; j < num - 1; j++) {
				close(pipe_fds[j][0]);
				close(pipe_fds[j][1]);
			}

			/* 파일 리다이렉션 적용 */
			if (setup_redirections(&pl->cmds[i]) < 0)
				_exit(1);

			/* 빌트인이면 자식에서 직접 실행 (파이프 내) */
			if (pl->cmds[i].argc > 0 &&
			    is_builtin(pl->cmds[i].argv[0])) {
				int rc = run_builtin(pl->cmds[i].argv);

				fflush(stdout);
				_exit(rc);
			}

			/*
			 * execvp() — PATH에서 프로그램을 찾아 실행
			 *
			 * execvp("grep", {"grep","foo",NULL})
			 *   1. $PATH의 각 디렉토리에서 "grep" 검색
			 *   2. /bin/grep 발견 → 현재 프로세스를 grep으로 교체
			 *   3. exec 성공시 여기에 절대 돌아오지 않음!
			 *
			 * 돌아왔다면 → 실패 (명령을 찾지 못함)
			 */
			execvp(pl->cmds[i].argv[0], pl->cmds[i].argv);
			fprintf(stderr, "citcsh: %s: %s\n",
				pl->cmds[i].argv[0], strerror(errno));
			_exit(127);
		}
	}

	/* === 부모 프로세스 === */

	/* 모든 파이프 fd 닫기 — 자식들이 EOF를 받을 수 있도록 */
	for (int i = 0; i < num - 1; i++) {
		close(pipe_fds[i][0]);
		close(pipe_fds[i][1]);
	}

	if (pl->background) {
		/*
		 * 백그라운드 실행: wait하지 않음
		 * 나중에 SIGCHLD 핸들러가 수거
		 */
		printf("[1] %d\n", pids[num - 1]);
		last_exit_code = 0;
	} else {
		/*
		 * 포그라운드: 모든 자식이 끝날 때까지 대기
		 * 마지막 명령의 종료 코드를 $?로 설정
		 */
		for (int i = 0; i < num; i++) {
			int status;

			waitpid(pids[i], &status, 0);
			if (i == num - 1) {
				if (WIFEXITED(status))
					last_exit_code = WEXITSTATUS(status);
				else
					last_exit_code = 128;
			}
		}
	}

	return last_exit_code;
}

/* ============================================================
 * 7. 시그널 처리
 * ============================================================
 *
 * 쉘의 시그널 처리 핵심 규칙:
 *
 *   SIGINT (Ctrl+C):
 *     - 쉘 자체는 무시 (사용자가 Ctrl+C를 눌러도 쉘은 죽지 않음)
 *     - 포그라운드 자식에게만 전달 (터미널 드라이버가 처리)
 *     - fork() 후 자식에서 SIG_DFL로 복원
 *
 *   SIGCHLD:
 *     - 자식 프로세스 종료 시 커널이 보내는 시그널
 *     - 백그라운드 프로세스 종료를 수거 (좀비 방지)
 *     - waitpid(-1, WNOHANG)으로 비동기 수거
 *
 * 좀비 프로세스란?
 *   자식이 종료됐지만 부모가 waitpid()로 수거하지 않은 상태.
 *   프로세스 테이블에 항목이 남아있다 (리소스 누수).
 *   SIGCHLD 핸들러에서 waitpid()로 방지.
 */
static void sigchld_handler(int sig)
{
	(void)sig;
	int saved_errno = errno;

	/*
	 * WNOHANG: 종료된 자식이 없으면 즉시 반환
	 * -1: 모든 자식 대상
	 * 루프: 동시에 여러 자식이 종료될 수 있음
	 */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	errno = saved_errno;
}

static void setup_signals(void)
{
	struct sigaction sa;

	/*
	 * SIGINT 무시 — Ctrl+C로 쉘이 죽지 않게.
	 * 자식은 fork() 후에 SIG_DFL로 복원한다.
	 */
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

	/* SIGCHLD — 백그라운드 프로세스 좀비 방지 */
	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);
}

/* ============================================================
 * 8. 프롬프트 & 입력 & 히스토리
 * ============================================================ */

/*
 * 프롬프트 출력
 *
 * 형식: "디렉토리 # " (root) 또는 "디렉토리 $ " (일반 사용자)
 * 예: /root #
 *
 * 쉘이 프롬프트를 출력하면 이렇게 동작한다:
 *   1. print_prompt() → "/ # " 출력
 *   2. fgets() → 사용자 입력 대기
 *   3. 입력 받으면 → 토큰화 → 파싱 → 실행
 *   4. 1로 돌아감 (REPL: Read-Eval-Print Loop)
 */
static void print_prompt(void)
{
	char cwd[512];

	if (!getcwd(cwd, sizeof(cwd)))
		strcpy(cwd, "?");

	/*
	 * 긴 경로 대신 마지막 디렉토리만 표시
	 * /usr/local/bin → bin
	 * / → /
	 */
	const char *display = strrchr(cwd, '/');

	if (display && display != cwd)
		display++;
	else
		display = cwd;

	char prompt = (getuid() == 0) ? '#' : '$';

	printf("%s %c ", display, prompt);
	fflush(stdout);
}

/* 히스토리에 추가 */
static void history_add(const char *line)
{
	int idx = history_count % HISTORY_SIZE;

	free(history[idx]);
	history[idx] = strdup(line);
	history_count++;
}

/* ============================================================
 * 9. 메인 함수 — REPL 루프
 * ============================================================
 *
 * REPL = Read-Eval-Print Loop
 *
 * 모든 대화형 쉘의 기본 구조:
 *   while (1) {
 *       프롬프트 출력 (Print)
 *       입력 읽기     (Read)
 *       실행          (Eval)
 *   }
 *
 * 이것이 bash를 실행하면 보이는 "$" 프롬프트와
 * 명령 입력 대기의 정체이다.
 */
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	/* 시그널 설정 */
	setup_signals();

	/* 기본 환경변수 설정 */
	if (!getenv("HOME"))
		setenv("HOME", "/root", 1);
	if (!getenv("PATH"))
		setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
	setenv("SHELL", "/bin/citcsh", 1);

	/* 시작 메시지 */
	printf("citcsh — CITC OS Shell\n");
	printf("'help'를 입력하면 사용법을 볼 수 있습니다.\n\n");

	/* === REPL 루프 === */
	char line[MAX_LINE];
	char expanded[MAX_LINE];
	struct token tokens[MAX_TOKENS];
	struct pipeline pl;

	while (shell_running) {
		print_prompt();

		/*
		 * 입력 읽기
		 * fgets는 \n 포함, EOF시 NULL 반환.
		 * Ctrl+D → EOF → fgets NULL → 쉘 종료.
		 */
		if (!fgets(line, sizeof(line), stdin)) {
			printf("\n");
			break;  /* EOF (Ctrl+D) */
		}

		/* 줄바꿈 제거 */
		size_t len = strlen(line);

		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';

		/* 빈 줄 무시 */
		if (line[0] == '\0')
			continue;

		/* 히스토리에 저장 */
		history_add(line);

		/* 환경변수 확장 ($HOME → /root 등) */
		expand_env_vars(expanded, line, sizeof(expanded));

		/* 토큰화 */
		int num_tokens = tokenize(expanded, tokens, MAX_TOKENS);

		if (num_tokens == 0)
			continue;

		/* 파싱 */
		if (parse_pipeline(tokens, num_tokens, &pl) < 0)
			continue;

		/* 유효한 명령이 있는지 확인 */
		if (pl.cmds[0].argc == 0)
			continue;

		/* 실행 */
		execute_pipeline(&pl);
	}

	/* 히스토리 메모리 해제 */
	for (int i = 0; i < HISTORY_SIZE; i++)
		free(history[i]);

	return last_exit_code;
}
