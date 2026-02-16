/*
 * repo.c - CITC OS 패키지 저장소 구현
 * ======================================
 *
 * 원격 패키지 저장소에서 패키지를 검색하고 설치하는 기능.
 *
 * 핵심 개념:
 *
 * 1. 패키지 인덱스 (PKGINDEX)
 *    저장소에 어떤 패키지가 있는지 목록을 담은 파일.
 *    apt의 Packages, pacman의 .db, npm의 registry와 같은 역할.
 *
 *    왜 인덱스가 필요한가?
 *    패키지를 설치할 때마다 서버에 "뭐가 있어?" 물어보면 느림.
 *    → 한 번에 목록을 다운로드하고 로컬에 캐시. (apt update와 같음)
 *
 * 2. 의존성 해결 (Dependency Resolution)
 *    패키지 A가 B에 의존 → B를 먼저 설치해야 함.
 *    B가 C에 의존하면? → C → B → A 순서로 설치.
 *
 *    이 순서를 정하는 알고리즘이 DFS (깊이 우선 탐색):
 *
 *    install(A)                    실행 순서
 *      ├─ 의존성 B 발견                │
 *      │  install(B)        ←── 재귀   │
 *      │    ├─ 의존성 C 발견            │
 *      │    │  install(C)   ←── 재귀   │
 *      │    │    └─ 의존성 없음         ① C 설치
 *      │    └─ C 설치됨                ② B 설치
 *      └─ B 설치됨                     ③ A 설치
 *
 *    visited 배열: "이미 처리한 패키지"를 기록하여
 *    순환 의존성(A→B→A)에서 무한 루프 방지.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "repo.h"

/* ============================================================
 * 내부 함수 선언
 * ============================================================ */
static int read_repo_url(char *url, size_t url_size);
static int load_index(repo_pkg_t *pkgs, int max_pkgs);
static repo_pkg_t *find_in_index(repo_pkg_t *pkgs, int count,
				 const char *name);
static int install_with_deps(repo_pkg_t *pkgs, int count,
			     const char *name, const char *repo_url,
			     int *visited, int depth);

/* ============================================================
 * 저장소 URL 읽기
 * ============================================================
 *
 * /etc/citcpkg/repo.conf 파일 형식:
 *   # 주석
 *   url=http://10.0.2.2:8080
 *
 * 왜 설정 파일을 쓰는가?
 *   URL을 코드에 하드코딩하면 변경할 때 다시 컴파일해야 함.
 *   설정 파일에 두면 사용자가 자유롭게 변경 가능.
 *   apt의 /etc/apt/sources.list, pacman의 /etc/pacman.conf와 같은 패턴.
 */
static int read_repo_url(char *url, size_t url_size)
{
	FILE *fp = fopen(REPO_CONF_PATH, "r");

	if (!fp) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 저장소 설정 파일 없음: %s\n", REPO_CONF_PATH);
		return -1;
	}

	char line[512];

	while (fgets(line, sizeof(line), fp)) {
		/* 주석, 빈 줄 건너뛰기 */
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;

		/* 줄 끝 개행 제거 (\n, \r\n 모두 처리) */
		line[strcspn(line, "\r\n")] = '\0';

		if (strncmp(line, "url=", 4) == 0) {
			snprintf(url, url_size, "%s", line + 4);
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);
	fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
		" repo.conf에 url= 항목 없음\n");
	return -1;
}

/* ============================================================
 * PKGINDEX 파싱
 * ============================================================
 *
 * 로컬에 캐시된 /var/lib/citcpkg/PKGINDEX를 읽어서
 * repo_pkg_t 배열로 변환.
 *
 * 파싱 전략:
 *   - 빈 줄로 패키지 구분 (ini 파일의 섹션과 비슷)
 *   - 각 줄은 key=value
 *   - depends가 여러 개면 콤마로 구분: depends=hello,libfoo
 *
 * 반환: 패키지 수 (>= 0), 실패 시 -1
 */
static int load_index(repo_pkg_t *pkgs, int max_pkgs)
{
	FILE *fp = fopen(REPO_INDEX_PATH, "r");

	if (!fp) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 패키지 인덱스 없음.\n"
			"  먼저 'citcpkg update'를 실행하세요.\n");
		return -1;
	}

	int count = 0;
	char line[512];
	repo_pkg_t *cur = &pkgs[0];

	memset(cur, 0, sizeof(*cur));

	while (fgets(line, sizeof(line), fp) && count < max_pkgs) {
		line[strcspn(line, "\r\n")] = '\0';

		/* 빈 줄 = 다음 패키지로 넘어감 */
		if (line[0] == '\0') {
			if (cur->name[0] != '\0') {
				count++;
				if (count < max_pkgs) {
					cur = &pkgs[count];
					memset(cur, 0, sizeof(*cur));
				}
			}
			continue;
		}

		/* 주석 건너뛰기 */
		if (line[0] == '#')
			continue;

		/*
		 * key=value 파싱
		 *
		 * strchr()로 첫 번째 '='을 찾고,
		 * '=' 왼쪽이 key, 오른쪽이 value.
		 */
		char *eq = strchr(line, '=');

		if (!eq)
			continue;

		*eq = '\0';
		char *key = line;
		char *val = eq + 1;

		if (strcmp(key, "name") == 0)
			snprintf(cur->name, sizeof(cur->name), "%s", val);
		else if (strcmp(key, "version") == 0)
			snprintf(cur->version, sizeof(cur->version),
				 "%s", val);
		else if (strcmp(key, "description") == 0)
			snprintf(cur->description, sizeof(cur->description),
				 "%s", val);
		else if (strcmp(key, "filename") == 0)
			snprintf(cur->filename, sizeof(cur->filename),
				 "%s", val);
		else if (strcmp(key, "depends") == 0 && val[0] != '\0') {
			/*
			 * 콤마 구분 의존성 파싱
			 * 예: depends=hello,libfoo
			 *
			 * strtok(): 문자열을 구분자로 쪼개는 함수.
			 *   strtok("hello,libfoo", ",")
			 *     → 1번째 호출: "hello"
			 *     → 2번째 호출: "libfoo"
			 *     → 3번째 호출: NULL (끝)
			 *
			 * 주의: strtok()은 원본 문자열을 수정함!
			 *       (','를 '\0'으로 바꿈)
			 */
			char deps_buf[256];

			snprintf(deps_buf, sizeof(deps_buf), "%s", val);

			char *tok = strtok(deps_buf, ",");

			while (tok && cur->num_depends < CPKG_MAX_DEPS) {
				/* 앞 공백 건너뛰기 */
				while (*tok == ' ')
					tok++;
				snprintf(cur->depends[cur->num_depends],
					 CPKG_NAME_MAX, "%s", tok);
				cur->num_depends++;
				tok = strtok(NULL, ",");
			}
		}
	}

	/* 마지막 패키지 (파일 끝에 빈 줄이 없을 수도 있음) */
	if (cur->name[0] != '\0')
		count++;

	fclose(fp);
	return count;
}

/* 이름으로 인덱스에서 패키지 찾기 */
static repo_pkg_t *find_in_index(repo_pkg_t *pkgs, int count,
				 const char *name)
{
	for (int i = 0; i < count; i++) {
		if (strcmp(pkgs[i].name, name) == 0)
			return &pkgs[i];
	}
	return NULL;
}

/* ============================================================
 * repo_update - 패키지 인덱스 갱신
 * ============================================================
 *
 * apt update와 같은 역할.
 * 서버에서 PKGINDEX 파일을 다운로드하여 로컬에 저장.
 *
 * 왜 update를 따로 하는가?
 *   매번 install할 때 인덱스를 받으면 느림.
 *   인덱스는 자주 바뀌지 않으므로 필요할 때만 갱신.
 *   "캐시(cache)" 개념 - 자주 쓰는 데이터를 가까이 보관.
 */
int repo_update(void)
{
	char url[512];
	char cmd[1024];

	if (read_repo_url(url, sizeof(url)) != 0)
		return -1;

	printf("저장소: %s\n", url);
	printf("인덱스 다운로드 중...\n");

	/* 캐시 디렉토리 생성 (다운로드한 .cpkg 임시 보관) */
	mkdir(REPO_CACHE_DIR, 0755);

	/*
	 * wget으로 PKGINDEX 다운로드
	 *
	 * wget -q    : quiet 모드 (진행률 표시 안 함)
	 * wget -O    : 출력 파일 지정
	 * 2>&1       : stderr도 stdout으로 합침 (에러 메시지 캡처)
	 *
	 * busybox wget은 전체 wget보다 기능이 적지만
	 * 기본 HTTP GET은 지원.
	 */
	snprintf(cmd, sizeof(cmd),
		 "wget -q -O %s %s/PKGINDEX 2>&1",
		 REPO_INDEX_PATH, url);

	if (system(cmd) != 0) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 인덱스 다운로드 실패\n");
		fprintf(stderr, "  URL: %s/PKGINDEX\n", url);
		return -1;
	}

	/* 다운로드된 인덱스를 파싱하여 패키지 수 표시 */
	static repo_pkg_t pkgs[REPO_MAX_PACKAGES];
	int count = load_index(pkgs, REPO_MAX_PACKAGES);

	if (count < 0)
		return -1;

	printf(COLOR_GREEN "완료:" COLOR_RESET
	       " %d개 패키지 사용 가능\n", count);
	return 0;
}

/* ============================================================
 * repo_search - 패키지 검색
 * ============================================================
 *
 * apt search, pacman -Ss와 같은 역할.
 * 로컬 PKGINDEX에서 키워드 검색.
 *
 * strstr():
 *   문자열 안에서 부분 문자열을 찾는 함수.
 *   strstr("Hello World", "World") → "World" 포인터 반환
 *   strstr("Hello World", "xyz")   → NULL
 */
int repo_search(const char *keyword)
{
	static repo_pkg_t pkgs[REPO_MAX_PACKAGES];
	int count = load_index(pkgs, REPO_MAX_PACKAGES);

	if (count < 0)
		return -1;

	printf("%-16s %-8s %s\n", "패키지", "버전", "설명");
	printf("──────   ──── ────\n");

	int found = 0;

	for (int i = 0; i < count; i++) {
		/* keyword가 NULL이면 전체 표시 */
		if (keyword == NULL ||
		    strstr(pkgs[i].name, keyword) ||
		    strstr(pkgs[i].description, keyword)) {
			/*
			 * 설치 여부도 함께 표시
			 * 이미 설치된 패키지면 [설치됨] 표시
			 */
			const char *status = "";

			if (pkg_is_installed(pkgs[i].name))
				status = COLOR_GREEN " [설치됨]" COLOR_RESET;

			printf("%-16s %-8s %s%s\n",
			       pkgs[i].name, pkgs[i].version,
			       pkgs[i].description, status);
			found++;
		}
	}

	if (found == 0 && keyword)
		printf("  '%s'에 해당하는 패키지 없음\n", keyword);
	else
		printf("\n%d개 패키지\n", found);

	return 0;
}

/* ============================================================
 * 의존성 포함 설치 (재귀 DFS)
 * ============================================================
 *
 * 재귀(recursion)란?
 *   함수가 자기 자신을 호출하는 것.
 *
 *   install_with_deps("greeting")
 *     → greeting은 hello에 의존
 *     → install_with_deps("hello")    ← 자기 자신 호출!
 *       → hello는 의존성 없음
 *       → hello 다운로드 & 설치
 *     → greeting 다운로드 & 설치
 *
 * 무한 루프 방지:
 *   visited[i] = 1 → "이 패키지는 이미 처리 중/완료"
 *   depth 제한 → 혹시 순환 의존성이 있어도 10단계에서 멈춤
 *
 * 매개변수:
 *   pkgs     - 전체 인덱스 배열
 *   count    - 인덱스 패키지 수
 *   name     - 설치할 패키지 이름
 *   repo_url - 저장소 URL (다운로드용)
 *   visited  - 방문 기록 배열 (인덱스별)
 *   depth    - 현재 재귀 깊이
 */
static int install_with_deps(repo_pkg_t *pkgs, int count,
			     const char *name, const char *repo_url,
			     int *visited, int depth)
{
	/* 재귀 깊이 제한: 순환 의존성 방지 */
	if (depth > 10) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 의존성 깊이 초과 (순환 의존성?)\n");
		return -1;
	}

	/* 이미 설치되어 있으면 건너뛰기 */
	if (pkg_is_installed(name)) {
		if (depth > 0) /* 의존성으로 불린 경우만 메시지 */
			printf("  %s: 이미 설치됨 (건너뜀)\n", name);
		return 0;
	}

	/* 인덱스에서 패키지 찾기 */
	repo_pkg_t *pkg = find_in_index(pkgs, count, name);

	if (!pkg) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 패키지 '%s'을(를) 찾을 수 없음\n", name);
		return -1;
	}

	/*
	 * visited 체크
	 *
	 * 포인터 연산으로 배열 인덱스 계산:
	 *   pkg는 pkgs[i]를 가리키는 포인터.
	 *   idx = pkg - pkgs → pkgs[0]부터의 거리 = i
	 *
	 * 이것은 C의 포인터 산술(pointer arithmetic):
	 *   &pkgs[3] - &pkgs[0] = 3
	 *   (주소 차이를 sizeof(repo_pkg_t)로 나눈 값)
	 */
	int idx = (int)(pkg - pkgs);

	if (visited[idx])
		return 0;
	visited[idx] = 1;

	/* 의존성 먼저 설치 (재귀!) */
	for (int i = 0; i < pkg->num_depends; i++) {
		printf("  의존성 해결: %s → %s\n",
		       name, pkg->depends[i]);

		int ret = install_with_deps(pkgs, count,
					    pkg->depends[i],
					    repo_url, visited,
					    depth + 1);
		if (ret != 0)
			return ret;
	}

	/* 패키지 다운로드 */
	char local_path[CPKG_PATH_MAX];
	char cmd[1024];

	snprintf(local_path, sizeof(local_path),
		 "%s/%s", REPO_CACHE_DIR, pkg->filename);

	printf("\n" COLOR_BLUE "다운로드:" COLOR_RESET " %s (%s %s)\n",
	       pkg->filename, pkg->name, pkg->version);

	snprintf(cmd, sizeof(cmd),
		 "wget -q -O %s %s/%s 2>&1",
		 local_path, repo_url, pkg->filename);

	if (system(cmd) != 0) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 다운로드 실패: %s/%s\n",
			repo_url, pkg->filename);
		return -1;
	}

	/* 로컬 파일로 설치 (기존 pkg_install 재사용!) */
	int ret = pkg_install(local_path);

	/* 다운로드한 캐시 파일 정리 */
	unlink(local_path);

	return ret;
}

/* ============================================================
 * repo_install - 원격 패키지 설치 (공개 API)
 * ============================================================
 *
 * apt install <name>과 같은 역할.
 * 패키지 이름만으로 다운로드 + 의존성 해결 + 설치.
 */
int repo_install(const char *pkg_name)
{
	char url[512];

	if (read_repo_url(url, sizeof(url)) != 0)
		return -1;

	/* 이미 설치 확인 */
	if (pkg_is_installed(pkg_name)) {
		printf("'%s'은(는) 이미 설치되어 있습니다.\n", pkg_name);
		return 0;
	}

	/* 인덱스 로드 */
	static repo_pkg_t pkgs[REPO_MAX_PACKAGES];
	int count = load_index(pkgs, REPO_MAX_PACKAGES);

	if (count < 0)
		return -1;

	/* 인덱스에서 패키지 존재 확인 */
	if (!find_in_index(pkgs, count, pkg_name)) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" '%s' 패키지를 인덱스에서 찾을 수 없음\n",
			pkg_name);
		fprintf(stderr,
			"  'citcpkg update'로 인덱스를 갱신해보세요.\n");
		return -1;
	}

	/* 캐시 디렉토리 생성 */
	mkdir(REPO_CACHE_DIR, 0755);

	/* 의존성 포함 설치 (DFS) */
	int visited[REPO_MAX_PACKAGES];

	memset(visited, 0, sizeof(visited));

	return install_with_deps(pkgs, count, pkg_name,
				 url, visited, 0);
}
