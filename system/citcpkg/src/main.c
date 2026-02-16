/*
 * citcpkg - CITC OS 패키지 관리자
 * =================================
 *
 * CITC OS에서 소프트웨어를 설치하고 관리하는 도구.
 *
 * v0.1: 로컬 .cpkg 파일 설치/제거/조회
 * v0.2: 원격 저장소 + 의존성 자동 해결
 *
 * 명령어:
 *   citcpkg install <이름|경로>   패키지 설치
 *   citcpkg remove <name>         패키지 제거
 *   citcpkg list                  설치된 패키지 목록
 *   citcpkg info <name>           패키지 상세 정보
 *   citcpkg update                저장소 인덱스 갱신
 *   citcpkg search [keyword]      사용 가능한 패키지 검색
 *
 * install 스마트 감지:
 *   argv[2]가 '/' 또는 '.'으로 시작 → 로컬 파일 설치
 *   그 외 → 저장소에서 다운로드 설치
 *
 *   citcpkg install /packages/hello-1.0.cpkg  ← 로컬
 *   citcpkg install greeting                  ← 원격
 */

#include <stdio.h>
#include <string.h>

#include "package.h"
#include "repo.h"

#define VERSION "0.2"

static void print_usage(void)
{
	printf("citcpkg - CITC OS Package Manager v%s\n", VERSION);
	printf("\n");
	printf("사용법:\n");
	printf("  citcpkg install <이름|파일.cpkg>  패키지 설치\n");
	printf("  citcpkg remove <name>             패키지 제거\n");
	printf("  citcpkg list                      설치된 패키지 목록\n");
	printf("  citcpkg info <name>               패키지 상세 정보\n");
	printf("  citcpkg update                    저장소 인덱스 갱신\n");
	printf("  citcpkg search [keyword]          패키지 검색\n");
	printf("\n");
	printf("예시:\n");
	printf("  citcpkg update                    저장소에서 목록 받기\n");
	printf("  citcpkg search                    전체 패키지 목록\n");
	printf("  citcpkg install greeting          원격에서 설치\n");
	printf("  citcpkg install /pkg/hello.cpkg   로컬 파일 설치\n");
	printf("  citcpkg remove hello\n");
}

/*
 * main() - CLI 엔트리포인트
 *
 * argc (argument count):  인자 개수
 * argv (argument vector): 인자 문자열 배열
 *
 * "citcpkg install greeting" 실행 시:
 *   argc = 3
 *   argv[0] = "citcpkg"
 *   argv[1] = "install"
 *   argv[2] = "greeting"     ← '/'로 시작 안 함 → 원격 설치
 *
 * "citcpkg install /packages/hello-1.0.cpkg" 실행 시:
 *   argv[2] = "/packages/..."  ← '/'로 시작 → 로컬 설치
 */
int main(int argc, char *argv[])
{
	if (argc < 2) {
		print_usage();
		return 1;
	}

	const char *command = argv[1];

	if (strcmp(command, "install") == 0) {
		if (argc < 3) {
			fprintf(stderr,
				"사용법: citcpkg install <이름|파일.cpkg>\n");
			return 1;
		}

		/*
		 * 스마트 감지:
		 *   '/' 또는 '.'으로 시작하면 파일 경로 → 로컬 설치
		 *   그 외 → 패키지 이름 → 원격 저장소에서 설치
		 */
		if (argv[2][0] == '/' || argv[2][0] == '.')
			return pkg_install(argv[2]) == 0 ? 0 : 1;
		else
			return repo_install(argv[2]) == 0 ? 0 : 1;

	} else if (strcmp(command, "remove") == 0) {
		if (argc < 3) {
			fprintf(stderr,
				"사용법: citcpkg remove <name>\n");
			return 1;
		}
		return pkg_remove(argv[2]) == 0 ? 0 : 1;

	} else if (strcmp(command, "list") == 0) {
		pkg_list();
		return 0;

	} else if (strcmp(command, "info") == 0) {
		if (argc < 3) {
			fprintf(stderr,
				"사용법: citcpkg info <name>\n");
			return 1;
		}
		return pkg_info(argv[2]) == 0 ? 0 : 1;

	} else if (strcmp(command, "update") == 0) {
		return repo_update() == 0 ? 0 : 1;

	} else if (strcmp(command, "search") == 0) {
		/* keyword는 선택사항 - 없으면 전체 목록 */
		return repo_search(argc >= 3 ? argv[2] : NULL) == 0 ? 0 : 1;

	} else if (strcmp(command, "help") == 0 ||
		   strcmp(command, "--help") == 0 ||
		   strcmp(command, "-h") == 0) {
		print_usage();
		return 0;

	} else {
		fprintf(stderr, "알 수 없는 명령: %s\n", command);
		fprintf(stderr, "'citcpkg help'를 입력하세요\n");
		return 1;
	}
}
