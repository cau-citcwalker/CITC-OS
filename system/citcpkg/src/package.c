/*
 * package.c - CITC OS 패키지 관리자 구현
 * ========================================
 *
 * 패키지 설치/제거의 핵심 로직.
 *
 * 사용하는 외부 도구:
 *   tar    - .cpkg 압축 해제 (busybox tar)
 *   cp     - 파일 복사
 *   rm     - 파일 삭제
 *
 * 왜 C 코드에서 외부 도구를 호출하나?
 *   tar 포맷 파싱을 직접 구현하면 수천 줄이 됨.
 *   이미 있는 도구를 재사용하는 것이 Unix 철학!
 *   "한 가지를 잘 하는 작은 프로그램을 조합하라"
 *
 *   system() 함수: 쉘 명령어를 실행하고 결과를 기다림.
 *   popen() 함수:  쉘 명령어의 출력을 파이프로 읽음.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "package.h"

/* ============================================================
 * 헬퍼: 디렉토리를 재귀적으로 생성
 * ============================================================
 *
 * mkdir -p 와 동일한 동작.
 * "/var/lib/citcpkg/installed" → /var, /var/lib, ... 순서로 생성.
 *
 * 왜 직접 구현하나?
 *   mkdir()는 부모 디렉토리가 없으면 실패함.
 *   mkdir("/a/b/c") → /a가 없으면 ENOENT 에러.
 *   그래서 경로를 / 단위로 쪼개서 하나씩 만들어야 함.
 */
static void mkdir_p(const char *path)
{
	char tmp[CPKG_PATH_MAX];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

/* ============================================================
 * 헬퍼: 문자열 trim (앞뒤 공백 제거)
 * ============================================================ */
static char *trim(char *str)
{
	char *end;

	while (*str == ' ' || *str == '\t')
		str++;

	if (*str == '\0')
		return str;

	end = str + strlen(str) - 1;
	while (end > str && (*end == ' ' || *end == '\t' ||
			     *end == '\n' || *end == '\r'))
		end--;

	*(end + 1) = '\0';
	return str;
}

/* ============================================================
 * PKGINFO 파싱
 * ============================================================
 *
 * config.c의 key=value 파서와 같은 패턴!
 * 코드 재사용의 좋은 예시.
 */
static int parse_pkginfo(const char *filepath, pkg_info_t *pkg)
{
	FILE *fp;
	char line[512];

	fp = fopen(filepath, "r");
	if (!fp)
		return -1;

	memset(pkg, 0, sizeof(*pkg));

	while (fgets(line, sizeof(line), fp)) {
		char *trimmed = trim(line);
		char *eq, *key, *value;

		if (trimmed[0] == '\0' || trimmed[0] == '#')
			continue;

		eq = strchr(trimmed, '=');
		if (!eq)
			continue;

		*eq = '\0';
		key = trim(trimmed);
		value = trim(eq + 1);

		if (strcmp(key, "name") == 0) {
			snprintf(pkg->name, sizeof(pkg->name), "%s", value);
		} else if (strcmp(key, "version") == 0) {
			snprintf(pkg->version, sizeof(pkg->version),
				 "%s", value);
		} else if (strcmp(key, "description") == 0) {
			snprintf(pkg->description, sizeof(pkg->description),
				 "%s", value);
		} else if (strcmp(key, "depends") == 0) {
			if (strlen(value) > 0 &&
			    pkg->num_depends < CPKG_MAX_DEPS) {
				snprintf(pkg->depends[pkg->num_depends],
					 CPKG_NAME_MAX, "%s", value);
				pkg->num_depends++;
			}
		}
	}

	fclose(fp);
	return (pkg->name[0] != '\0') ? 0 : -1;
}

/* ============================================================
 * 패키지 설치 여부 확인
 * ============================================================ */
int pkg_is_installed(const char *name)
{
	char path[CPKG_PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s.pkg", CPKG_DB_DIR, name);
	return access(path, F_OK) == 0;
}

/* ============================================================
 * 패키지 설치
 * ============================================================
 *
 * 설치 과정 (7단계):
 *
 *   .cpkg 파일
 *       ↓ tar xzf
 *   임시 디렉토리 (/tmp/citcpkg-XXXXXX/)
 *   ├── PKGINFO     → 파싱 → 이름, 버전, 의존성
 *   └── data/       → 파일 목록 기록 + / 에 복사
 *       └── usr/
 *           └── bin/
 *               └── hello
 *       ↓
 *   / (루트 파일시스템에 설치됨)
 *   └── usr/
 *       └── bin/
 *           └── hello  ← 새로 설치된 파일
 *       ↓
 *   /var/lib/citcpkg/installed/hello.pkg  ← 설치 기록
 */
int pkg_install(const char *cpkg_path)
{
	/* tmp_dir은 /tmp/citcpkg-XXXXXX 정도이므로 256이면 충분 */
	char tmp_dir[256];
	char cmd[1024];
	char pkginfo_path[CPKG_PATH_MAX];
	char db_path[CPKG_PATH_MAX];
	char data_dir[CPKG_PATH_MAX];
	pkg_info_t pkg;
	FILE *db_fp;
	FILE *find_fp;
	char line[CPKG_PATH_MAX];
	char files[CPKG_MAX_FILES][CPKG_PATH_MAX];
	int num_files = 0;

	/* 1. .cpkg 파일 존재 확인 */
	if (access(cpkg_path, R_OK) != 0) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 패키지 파일을 찾을 수 없습니다: %s\n", cpkg_path);
		return -1;
	}

	/* 2. 임시 디렉토리 생성 */
	snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/citcpkg-XXXXXX");
	if (!mkdtemp(tmp_dir)) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 임시 디렉토리 생성 실패\n");
		return -1;
	}

	/* 3. .cpkg 압축 해제 */
	/*
	 * tar 옵션:
	 *   x = extract (추출)
	 *   z = gzip 압축 해제
	 *   f = 파일 지정
	 *   -C = 출력 디렉토리 지정
	 */
	snprintf(cmd, sizeof(cmd), "tar xzf %s -C %s 2>/dev/null",
		 cpkg_path, tmp_dir);

	if (system(cmd) != 0) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 패키지 압축 해제 실패: %s\n", cpkg_path);
		goto cleanup;
	}

	/* 4. PKGINFO 파싱 */
	snprintf(pkginfo_path, sizeof(pkginfo_path),
		 "%s/PKGINFO", tmp_dir);

	if (parse_pkginfo(pkginfo_path, &pkg) < 0) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" PKGINFO 파일이 없거나 잘못되었습니다\n");
		goto cleanup;
	}

	printf(COLOR_BLUE "패키지:" COLOR_RESET " %s %s\n",
	       pkg.name, pkg.version);

	if (pkg.description[0])
		printf(COLOR_BLUE "설명:  " COLOR_RESET " %s\n",
		       pkg.description);

	/* 5. 이미 설치되어 있는지 확인 */
	if (pkg_is_installed(pkg.name)) {
		fprintf(stderr, COLOR_YELLOW "경고:" COLOR_RESET
			" '%s'는 이미 설치되어 있습니다."
			" 먼저 제거하세요.\n", pkg.name);
		goto cleanup;
	}

	/* 6. 의존성 확인 */
	for (int i = 0; i < pkg.num_depends; i++) {
		if (!pkg_is_installed(pkg.depends[i])) {
			fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
				" 의존 패키지 '%s'가 설치되지 않았습니다\n",
				pkg.depends[i]);
			goto cleanup;
		}
	}

	/* 7. data/ 디렉토리에서 설치할 파일 목록 수집 */
	/*
	 * popen(): 명령어를 실행하고 그 출력을 파이프로 읽음.
	 *
	 * find 명령:
	 *   -type f = 일반 파일만
	 *   -type l = 심볼릭 링크도 포함
	 *
	 * 예: find /tmp/citcpkg-abc123/data -type f
	 *   → /tmp/citcpkg-abc123/data/usr/bin/hello
	 *
	 * 이 경로에서 "data" 부분을 제거하면 설치 경로가 됨:
	 *   /tmp/.../data/usr/bin/hello → /usr/bin/hello
	 */
	snprintf(data_dir, sizeof(data_dir), "%s/data", tmp_dir);
	snprintf(cmd, sizeof(cmd),
		 "find %s -type f -o -type l 2>/dev/null", data_dir);

	find_fp = popen(cmd, "r");
	if (!find_fp) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 파일 목록 수집 실패\n");
		goto cleanup;
	}

	while (fgets(line, sizeof(line), find_fp)) {
		char *trimmed = trim(line);
		const char *rel_path;

		if (trimmed[0] == '\0')
			continue;

		/*
		 * 상대 경로 추출:
		 * "/tmp/citcpkg-abc/data/usr/bin/hello"
		 *                   ^^^^ 이 위치부터가 설치 경로
		 * → "/usr/bin/hello"
		 */
		rel_path = trimmed + strlen(data_dir);
		if (rel_path[0] == '\0')
			continue;

		if (num_files < CPKG_MAX_FILES) {
			snprintf(files[num_files], CPKG_PATH_MAX,
				 "%s", rel_path);
			num_files++;
		}
	}
	pclose(find_fp);

	if (num_files == 0) {
		fprintf(stderr, COLOR_YELLOW "경고:" COLOR_RESET
			" 패키지에 설치할 파일이 없습니다\n");
		goto cleanup;
	}

	/* 8. 파일 복사 (data/ → /) */
	/*
	 * cp -a: 아카이브 모드 복사
	 *   - 권한, 소유자, 타임스탬프 보존
	 *   - 심볼릭 링크 보존
	 *   - 재귀적 복사
	 *
	 * data/. 의 마지막 점(.):
	 *   "이 디렉토리의 내용물"을 의미.
	 *   cp -a data/. / → data 안의 파일들을 / 에 복사
	 *   (data 디렉토리 자체가 아니라 내용물만)
	 */
	printf("파일 %d개 설치 중...\n", num_files);

	snprintf(cmd, sizeof(cmd), "cp -a %s/. / 2>/dev/null", data_dir);
	if (system(cmd) != 0) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 파일 복사 실패\n");
		goto cleanup;
	}

	/* 9. 설치 기록 저장 */
	/*
	 * /var/lib/citcpkg/installed/hello.pkg 파일에:
	 *   PKGINFO 내용 + 파일 목록을 기록.
	 *
	 * 나중에 pkg_remove()가 이 파일을 읽어서
	 * 어떤 파일을 삭제해야 하는지 알 수 있음.
	 */
	mkdir_p(CPKG_DB_DIR);

	snprintf(db_path, sizeof(db_path), "%s/%s.pkg",
		 CPKG_DB_DIR, pkg.name);

	db_fp = fopen(db_path, "w");
	if (!db_fp) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 설치 기록 저장 실패: %s\n", strerror(errno));
		goto cleanup;
	}

	fprintf(db_fp, "name=%s\n", pkg.name);
	fprintf(db_fp, "version=%s\n", pkg.version);
	fprintf(db_fp, "description=%s\n", pkg.description);
	for (int i = 0; i < pkg.num_depends; i++)
		fprintf(db_fp, "depends=%s\n", pkg.depends[i]);
	fprintf(db_fp, "---FILES---\n");
	for (int i = 0; i < num_files; i++)
		fprintf(db_fp, "%s\n", files[i]);

	fclose(db_fp);

	/* 10. 정리 */
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_dir);
	if (system(cmd) != 0) { /* 정리 실패는 무시 */ }

	printf(COLOR_GREEN "설치 완료:" COLOR_RESET " %s %s (%d개 파일)\n",
	       pkg.name, pkg.version, num_files);
	return 0;

cleanup:
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_dir);
	if (system(cmd) != 0) { /* 정리 실패는 무시 */ }
	return -1;
}

/* ============================================================
 * 패키지 제거
 * ============================================================
 *
 * 제거 과정:
 *   1. /var/lib/citcpkg/installed/<name>.pkg 읽기
 *   2. "---FILES---" 이후의 줄들이 설치된 파일 목록
 *   3. 각 파일을 unlink()로 삭제
 *   4. 설치 기록 파일 삭제
 *
 * unlink() vs remove():
 *   unlink() = 파일만 삭제 (디렉토리 불가)
 *   remove() = 파일과 빈 디렉토리 모두 삭제
 *   패키지가 만든 디렉토리는 다른 패키지도 쓸 수 있으므로
 *   파일만 삭제하고 디렉토리는 남겨두는 것이 안전.
 */
int pkg_remove(const char *name)
{
	char db_path[CPKG_PATH_MAX];
	FILE *fp;
	char line[CPKG_PATH_MAX];
	int in_files = 0;
	int removed = 0;
	pkg_info_t pkg;

	if (!pkg_is_installed(name)) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" '%s'는 설치되어 있지 않습니다\n", name);
		return -1;
	}

	/* 설치 기록 읽기 */
	snprintf(db_path, sizeof(db_path), "%s/%s.pkg", CPKG_DB_DIR, name);

	/* 먼저 패키지 정보 파싱 */
	parse_pkginfo(db_path, &pkg);
	printf(COLOR_BLUE "제거:" COLOR_RESET " %s %s\n",
	       pkg.name, pkg.version);

	/* 파일 삭제 */
	fp = fopen(db_path, "r");
	if (!fp) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" 설치 기록을 읽을 수 없습니다\n");
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *trimmed = trim(line);

		/* ---FILES--- 구분자를 만나면 이후부터 파일 경로 */
		if (strcmp(trimmed, "---FILES---") == 0) {
			in_files = 1;
			continue;
		}

		if (!in_files || trimmed[0] == '\0')
			continue;

		/* 파일 삭제 */
		if (unlink(trimmed) == 0) {
			removed++;
		} else if (errno != ENOENT) {
			/* ENOENT = 파일이 이미 없음 (무시) */
			fprintf(stderr, "  경고: %s 삭제 실패: %s\n",
				trimmed, strerror(errno));
		}
	}

	fclose(fp);

	/* 설치 기록 삭제 */
	unlink(db_path);

	printf(COLOR_GREEN "제거 완료:" COLOR_RESET " %s (%d개 파일 삭제)\n",
	       name, removed);
	return 0;
}

/* ============================================================
 * 설치된 패키지 목록
 * ============================================================
 *
 * /var/lib/citcpkg/installed/ 디렉토리를 순회.
 * .pkg 확장자를 가진 파일마다 PKGINFO를 파싱하여 출력.
 *
 * 이 패턴은 config.c의 디렉토리 순회와 동일!
 */
int pkg_list(void)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	dir = opendir(CPKG_DB_DIR);
	if (!dir) {
		printf("설치된 패키지가 없습니다.\n");
		return 0;
	}

	printf(COLOR_BOLD "%-20s %-10s %s" COLOR_RESET "\n",
	       "패키지", "버전", "설명");
	printf("%-20s %-10s %s\n", "──────", "────", "────");

	while ((entry = readdir(dir)) != NULL) {
		char path[CPKG_PATH_MAX];
		const char *dot;
		pkg_info_t pkg;

		/* .pkg 파일만 처리 */
		dot = strrchr(entry->d_name, '.');
		if (!dot || strcmp(dot, ".pkg") != 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s",
			 CPKG_DB_DIR, entry->d_name);

		if (parse_pkginfo(path, &pkg) == 0) {
			printf("%-20s %-10s %s\n",
			       pkg.name, pkg.version, pkg.description);
			count++;
		}
	}

	closedir(dir);

	if (count == 0)
		printf("  (없음)\n");

	printf("\n%d개 패키지 설치됨\n", count);
	return count;
}

/* ============================================================
 * 패키지 상세 정보
 * ============================================================ */
int pkg_info(const char *name)
{
	char db_path[CPKG_PATH_MAX];
	FILE *fp;
	char line[CPKG_PATH_MAX];
	int in_files = 0;
	int file_count = 0;
	pkg_info_t pkg;

	if (!pkg_is_installed(name)) {
		fprintf(stderr, COLOR_RED "오류:" COLOR_RESET
			" '%s'는 설치되어 있지 않습니다\n", name);
		return -1;
	}

	snprintf(db_path, sizeof(db_path), "%s/%s.pkg", CPKG_DB_DIR, name);

	if (parse_pkginfo(db_path, &pkg) < 0)
		return -1;

	printf(COLOR_BOLD "패키지 정보: %s" COLOR_RESET "\n", pkg.name);
	printf("  버전:    %s\n", pkg.version);
	printf("  설명:    %s\n", pkg.description);

	if (pkg.num_depends > 0) {
		printf("  의존성:  ");
		for (int i = 0; i < pkg.num_depends; i++) {
			printf("%s%s", pkg.depends[i],
			       i < pkg.num_depends - 1 ? ", " : "");
		}
		printf("\n");
	}

	/* 설치된 파일 목록 */
	printf("  파일:\n");

	fp = fopen(db_path, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		char *trimmed = trim(line);

		if (strcmp(trimmed, "---FILES---") == 0) {
			in_files = 1;
			continue;
		}

		if (in_files && trimmed[0] != '\0') {
			printf("    %s\n", trimmed);
			file_count++;
		}
	}

	fclose(fp);
	printf("  총 %d개 파일\n", file_count);

	return 0;
}
