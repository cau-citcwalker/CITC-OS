/*
 * repo.h - CITC OS 패키지 저장소 (원격)
 * =======================================
 *
 * 패키지 저장소란?
 *   패키지 파일(.cpkg)을 모아놓은 HTTP 서버.
 *   apt의 저장소, pacman의 미러, npm의 registry와 같은 개념.
 *
 *   저장소 구조 (HTTP 서버):
 *     http://10.0.2.2:8080/
 *       PKGINDEX              ← 사용 가능한 패키지 목록
 *       hello-1.0.cpkg        ← 패키지 파일들
 *       greeting-1.0.cpkg
 *
 *   PKGINDEX 형식 (패키지 인덱스):
 *     name=hello
 *     version=1.0
 *     description=Hello World 프로그램
 *     depends=
 *     filename=hello-1.0.cpkg
 *                              ← 빈 줄로 패키지 구분
 *     name=greeting
 *     version=1.0
 *     ...
 *
 * 작동 흐름:
 *   1. citcpkg update
 *      → wget으로 PKGINDEX 다운로드 → /var/lib/citcpkg/PKGINDEX에 저장
 *
 *   2. citcpkg search hello
 *      → 로컬 PKGINDEX에서 키워드 검색
 *
 *   3. citcpkg install greeting
 *      → PKGINDEX에서 greeting 찾기
 *      → depends=hello 발견
 *      → hello 먼저 다운로드 & 설치 (재귀)
 *      → greeting 다운로드 & 설치
 *
 * 의존성 해결:
 *   DFS (Depth-First Search, 깊이 우선 탐색)를 사용.
 *   트리의 가장 깊은 곳(의존성 없는 패키지)부터 설치.
 *   visited 배열로 중복/순환 방지.
 */

#ifndef CITCPKG_REPO_H
#define CITCPKG_REPO_H

#include "package.h"

/* 저장소 관련 경로 */
#define REPO_CONF_PATH   "/etc/citcpkg/repo.conf"
#define REPO_INDEX_PATH  "/var/lib/citcpkg/PKGINDEX"
#define REPO_CACHE_DIR   "/var/lib/citcpkg/cache"

/* 인덱스에 저장 가능한 패키지 최대 수 */
#define REPO_MAX_PACKAGES 64

/*
 * 저장소 패키지 정보
 *
 * PKGINDEX에서 파싱한 패키지 정보.
 * pkg_info_t와 비슷하지만 filename 필드가 추가됨.
 * filename은 서버에서 다운로드할 파일명. (예: hello-1.0.cpkg)
 */
typedef struct {
	char name[CPKG_NAME_MAX];
	char version[CPKG_VER_MAX];
	char description[CPKG_DESC_MAX];
	char depends[CPKG_MAX_DEPS][CPKG_NAME_MAX];
	int  num_depends;
	char filename[128];   /* 서버의 파일명 (짧으므로 128이면 충분) */
} repo_pkg_t;

/*
 * repo_update - 저장소에서 패키지 인덱스 다운로드
 *
 * /etc/citcpkg/repo.conf에서 URL을 읽고,
 * wget으로 PKGINDEX를 다운로드하여 로컬에 저장.
 *
 * 반환: 0=성공, -1=실패
 */
int repo_update(void);

/*
 * repo_search - 패키지 검색
 *
 * 로컬 PKGINDEX에서 키워드가 포함된 패키지 검색.
 * keyword가 NULL이면 전체 목록 표시.
 *
 * 반환: 0=성공, -1=실패
 */
int repo_search(const char *keyword);

/*
 * repo_install - 원격 패키지 설치 (의존성 자동 해결)
 *
 * 1. PKGINDEX에서 패키지 정보 조회
 * 2. 의존성 트리를 DFS로 순회
 * 3. 필요한 패키지를 다운로드하고 pkg_install()로 설치
 *
 * 반환: 0=성공, -1=실패
 */
int repo_install(const char *pkg_name);

#endif /* CITCPKG_REPO_H */
