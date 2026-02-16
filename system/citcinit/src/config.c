/*
 * config.c - CITC OS 서비스 설정 파일 파서 구현
 * ================================================
 *
 * C에서 텍스트 파일을 파싱하는 방법을 배웁니다.
 *
 * 핵심 함수들:
 *   fopen()  - 파일 열기
 *   fgets()  - 한 줄 읽기
 *   strchr() - 문자 찾기 (= 찾아서 key/value 분리)
 *   strcmp() - 문자열 비교
 *   strncpy()- 안전한 문자열 복사
 *   opendir()/readdir() - 디렉토리 순회
 *
 * 파싱 전략:
 *   1단계: 파일을 한 줄씩 읽는다 (fgets)
 *   2단계: 빈 줄, 주석(#) 건너뛰기
 *   3단계: '=' 문자를 찾아서 key와 value 분리
 *   4단계: 앞뒤 공백 제거 (trim)
 *   5단계: key에 따라 적절한 동작 수행
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>     /* opendir(), readdir(), closedir() */
#include <errno.h>

#include "config.h"
#include "service.h"

/* 로그 매크로 */
#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_RESET  "\033[0m"

#define LOG_OK(fmt, ...) \
	printf(COLOR_GREEN "[  OK  ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_FAIL(fmt, ...) \
	printf(COLOR_RED   "[ FAIL ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
	printf(COLOR_BLUE  "[ INFO ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
	printf(COLOR_YELLOW "[ WARN ]" COLOR_RESET " " fmt "\n", ##__VA_ARGS__)

/* 한 줄의 최대 길이 */
#define LINE_MAX_LEN 512

/* ============================================================
 * 문자열 유틸리티: 앞뒤 공백 제거 (trim)
 * ============================================================
 *
 * "  hello world  " → "hello world"
 *
 * 왜 trim이 필요한가?
 *   설정 파일에서 사용자가 = 앞뒤에 공백을 넣을 수 있음:
 *   "name = syslog"  → key="name ", value=" syslog"
 *   trim하지 않으면 비교가 실패함.
 *
 * 구현:
 *   1. 앞에서부터 공백이 아닌 첫 문자 찾기
 *   2. 뒤에서부터 공백이 아닌 마지막 문자 찾기
 *   3. 그 범위를 반환
 *
 * 주의: 이 함수는 원본 문자열을 수정합니다 (뒤쪽에 '\0' 삽입).
 *       C에서는 흔한 패턴이지만, 다른 언어에서는 보통 새 문자열을 반환.
 */
static char *trim(char *str)
{
	char *end;

	/* 앞쪽 공백 건너뛰기 */
	while (*str == ' ' || *str == '\t')
		str++;

	/* 빈 문자열 */
	if (*str == '\0')
		return str;

	/* 뒤쪽 공백 제거 */
	end = str + strlen(str) - 1;
	while (end > str && (*end == ' ' || *end == '\t' ||
			     *end == '\n' || *end == '\r'))
		end--;

	/* NULL 종료 */
	*(end + 1) = '\0';

	return str;
}

/* ============================================================
 * 파일 확장자 확인
 * ============================================================
 *
 * 파일 이름이 ".conf"로 끝나는지 확인.
 *
 * strrchr(): 문자열에서 마지막으로 나타나는 문자를 찾음.
 *   "syslog.conf" → strrchr('.') → ".conf"
 */
static int has_conf_extension(const char *filename)
{
	const char *dot = strrchr(filename, '.');

	if (!dot)
		return 0;

	return strcmp(dot, ".conf") == 0;
}

/* ============================================================
 * 단일 설정 파일 파싱
 * ============================================================
 *
 * 파싱 과정 (한 줄씩):
 *
 *   "name=syslog"
 *        ↓
 *   strchr(line, '=') → '=' 위치 찾기
 *        ↓
 *   '='을 '\0'으로 바꿔서 문자열 분리
 *        ↓
 *   key = "name", value = "syslog"
 *        ↓
 *   strcmp(key, "name") → 매치! → name에 저장
 *
 * 2-패스(two-pass) 전략:
 *   1st pass: name과 exec를 먼저 읽어서 svc_register() 호출
 *             (이게 있어야 다른 필드를 설정할 수 있음)
 *   ... 하지만 이건 복잡하므로:
 *
 *   대신 임시 구조체에 모든 값을 저장한 후,
 *   파일을 다 읽으면 한 번에 등록.
 */

/* 파싱 중 임시 저장용 구조체 */
struct parsed_service {
	char name[SVC_NAME_MAX];
	char exec[SVC_PATH_MAX];
	char type_str[32];
	int auto_restart;
	char args[SVC_MAX_ARGS][256];
	int num_args;
	char depends[SVC_MAX_DEPS][SVC_NAME_MAX];
	int num_depends;
	char socket_path[SVC_PATH_MAX];  /* 소켓 활성화 경로 (Class 19) */
};

int config_load_file(const char *filepath)
{
	FILE *fp;
	char line[LINE_MAX_LEN];
	struct parsed_service ps;
	service_type_t type;

	/*
	 * fopen(): 파일을 열고 FILE 포인터를 반환.
	 * "r" = 읽기 모드.
	 * 실패하면 NULL 반환.
	 */
	fp = fopen(filepath, "r");
	if (!fp) {
		LOG_WARN("Config file open failed: %s (%s)",
			 filepath, strerror(errno));
		return -1;
	}

	/* 임시 구조체 초기화 */
	memset(&ps, 0, sizeof(ps));

	/*
	 * fgets(): 파일에서 한 줄을 읽음.
	 * - 최대 LINE_MAX_LEN-1 바이트 읽음
	 * - 줄 끝의 '\n'도 포함
	 * - EOF에 도달하면 NULL 반환
	 *
	 * 왜 fgets인가? (vs fscanf, fread)
	 *   - fgets는 버퍼 오버플로 방지 (최대 크기 지정)
	 *   - 한 줄 단위로 처리하기 좋음
	 *   - fscanf는 포맷이 복잡하면 오류 나기 쉬움
	 *   - fread는 바이너리 파일용
	 */
	while (fgets(line, sizeof(line), fp)) {
		char *trimmed = trim(line);
		char *eq;
		char *key;
		char *value;

		/* 빈 줄이나 주석 건너뛰기 */
		if (trimmed[0] == '\0' || trimmed[0] == '#')
			continue;

		/*
		 * '=' 문자 찾기.
		 * strchr(str, ch): 문자열에서 ch가 처음 나타나는 위치.
		 * 없으면 NULL.
		 */
		eq = strchr(trimmed, '=');
		if (!eq) {
			LOG_WARN("Invalid format (no =): %s", trimmed);
			continue;
		}

		/* '='를 '\0'으로 바꿔서 key와 value 분리 */
		*eq = '\0';
		key = trim(trimmed);
		value = trim(eq + 1);

		/* key에 따라 값 저장 */
		if (strcmp(key, "name") == 0) {
			snprintf(ps.name, sizeof(ps.name), "%s", value);
		} else if (strcmp(key, "exec") == 0) {
			snprintf(ps.exec, sizeof(ps.exec), "%s", value);
		} else if (strcmp(key, "type") == 0) {
			snprintf(ps.type_str, sizeof(ps.type_str),
				 "%s", value);
		} else if (strcmp(key, "restart") == 0) {
			ps.auto_restart = (strcmp(value, "yes") == 0 ||
					   strcmp(value, "1") == 0);
		} else if (strcmp(key, "args") == 0) {
			if (ps.num_args < SVC_MAX_ARGS) {
				snprintf(ps.args[ps.num_args],
					 sizeof(ps.args[0]), "%s", value);
				ps.num_args++;
			}
		} else if (strcmp(key, "depends") == 0) {
			if (ps.num_depends < SVC_MAX_DEPS) {
				snprintf(ps.depends[ps.num_depends],
					 sizeof(ps.depends[0]), "%s", value);
				ps.num_depends++;
			}
		} else if (strcmp(key, "socket") == 0) {
			/*
			 * socket= : 소켓 활성화 경로 (Class 19)
			 *
			 * 이 경로에 Unix domain socket을 생성하여
			 * 클라이언트 연결을 감시. 연결이 오면 서비스 시작.
			 *
			 * 예: socket=/tmp/citc-display-0
			 *     → init이 이 소켓을 미리 만들어 listen
			 *     → 클라이언트가 연결하면 compositor 시작
			 */
			snprintf(ps.socket_path, sizeof(ps.socket_path),
				 "%s", value);
		} else {
			LOG_WARN("Unknown key: %s (file: %s)",
				 key, filepath);
		}
	}

	fclose(fp);

	/* 필수 필드 확인 */
	if (ps.name[0] == '\0') {
		LOG_FAIL("No 'name' in config: %s", filepath);
		return -1;
	}
	if (ps.exec[0] == '\0') {
		LOG_FAIL("No 'exec' in config: %s", filepath);
		return -1;
	}

	/* 타입 문자열 → enum 변환 */
	if (strcmp(ps.type_str, "oneshot") == 0)
		type = SVC_TYPE_ONESHOT;
	else if (strcmp(ps.type_str, "notify") == 0)
		type = SVC_TYPE_NOTIFY;
	else
		type = SVC_TYPE_SIMPLE;  /* 기본값 */

	/* 서비스 등록 */
	if (svc_register(ps.name, ps.exec, type, ps.auto_restart) < 0) {
		LOG_FAIL("Service registration failed: %s", ps.name);
		return -1;
	}

	/* 인자 추가 */
	for (int i = 0; i < ps.num_args; i++)
		svc_add_arg(ps.name, ps.args[i]);

	/* 의존성 추가 */
	for (int i = 0; i < ps.num_depends; i++)
		svc_add_dependency(ps.name, ps.depends[i]);

	/* 소켓 활성화 설정 (Class 19) */
	if (ps.socket_path[0] != '\0')
		svc_set_socket(ps.name, ps.socket_path);

	LOG_OK("Service loaded: %s (%s)", ps.name, filepath);
	return 0;
}

/* ============================================================
 * 디렉토리에서 모든 .conf 파일 로드
 * ============================================================
 *
 * 디렉토리 순회 API:
 *   opendir(path)  → DIR* 핸들 반환
 *   readdir(dir)   → struct dirent* (파일 이름 포함)
 *   closedir(dir)  → 디렉토리 닫기
 *
 * struct dirent:
 *   d_name[] - 파일 이름 (경로 없이 이름만)
 *   d_type   - 파일 타입 (DT_REG=일반파일, DT_DIR=디렉토리)
 *
 * 주의: readdir()은 "." 과 ".." 도 반환하므로 건너뛰어야 함.
 */
int config_load_services(const char *config_dir)
{
	DIR *dir;
	struct dirent *entry;
	int loaded = 0;
	char filepath[512];

	LOG_INFO("Loading service configs: %s", config_dir);

	dir = opendir(config_dir);
	if (!dir) {
		LOG_WARN("Config dir open failed: %s (%s)",
			 config_dir, strerror(errno));
		LOG_WARN("Using hardcoded services.");
		return 0;
	}

	/*
	 * readdir() 루프: 디렉토리의 모든 항목을 순회.
	 * NULL이 반환되면 더 이상 항목이 없음.
	 */
	while ((entry = readdir(dir)) != NULL) {
		/* "."과 ".." 건너뛰기 */
		if (entry->d_name[0] == '.')
			continue;

		/* .conf 파일만 처리 */
		if (!has_conf_extension(entry->d_name))
			continue;

		/* 전체 경로 구성 */
		snprintf(filepath, sizeof(filepath), "%s/%s",
			 config_dir, entry->d_name);

		/* 파일 로드 */
		if (config_load_file(filepath) == 0)
			loaded++;
	}

	closedir(dir);

	LOG_OK("%d service configs loaded", loaded);
	return loaded;
}
