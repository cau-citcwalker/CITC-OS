/*
 * package.h - CITC OS 패키지 관리자 (citcpkg)
 * =============================================
 *
 * 패키지 매니저의 핵심 기능:
 *   - install: .cpkg 파일에서 소프트웨어 설치
 *   - remove:  설치된 패키지 제거
 *   - list:    설치된 패키지 목록 조회
 *   - info:    특정 패키지의 상세 정보
 *
 * 패키지 포맷 (.cpkg = tar.gz 아카이브):
 *   PKGINFO     - 메타데이터 (name, version, description, depends)
 *   data/       - 설치할 파일들 (루트 기준 경로)
 *
 * 설치 데이터베이스:
 *   /var/lib/citcpkg/installed/<name>.pkg
 *   각 파일에 PKGINFO + 설치된 파일 목록 기록
 */

#ifndef CITCPKG_PACKAGE_H
#define CITCPKG_PACKAGE_H

/* 터미널 색상 (ANSI escape code) */
#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_BOLD   "\033[1m"
#define COLOR_RESET  "\033[0m"

/* 패키지 DB 디렉토리 */
#define CPKG_DB_DIR      "/var/lib/citcpkg/installed"

/* 필드 최대 길이 */
#define CPKG_NAME_MAX    64
#define CPKG_VER_MAX     32
#define CPKG_DESC_MAX    256
#define CPKG_PATH_MAX    512
#define CPKG_MAX_DEPS    16
#define CPKG_MAX_FILES   256

/*
 * 패키지 메타데이터
 * PKGINFO 파일의 내용을 파싱하여 저장.
 */
typedef struct {
	char name[CPKG_NAME_MAX];
	char version[CPKG_VER_MAX];
	char description[CPKG_DESC_MAX];
	char depends[CPKG_MAX_DEPS][CPKG_NAME_MAX];
	int num_depends;
} pkg_info_t;

/*
 * 패키지 설치
 *
 * 과정:
 *   1. .cpkg를 임시 디렉토리에 압축 해제 (tar xzf)
 *   2. PKGINFO 파싱 → 이름, 버전, 의존성 확인
 *   3. 이미 설치되어 있는지 확인
 *   4. 의존성이 설치되어 있는지 확인
 *   5. data/ 아래 파일들을 / 에 복사
 *   6. 설치 기록 저장 (파일 목록 포함)
 *   7. 임시 디렉토리 정리
 *
 * 반환: 0 성공, -1 실패
 */
int pkg_install(const char *cpkg_path);

/*
 * 패키지 제거
 *
 * 과정:
 *   1. 설치 기록 읽기 (/var/lib/citcpkg/installed/<name>.pkg)
 *   2. 기록된 파일들을 하나씩 삭제
 *   3. 빈 디렉토리 정리
 *   4. 설치 기록 삭제
 *
 * 반환: 0 성공, -1 실패
 */
int pkg_remove(const char *name);

/*
 * 설치된 패키지 목록 출력
 *
 * /var/lib/citcpkg/installed/ 디렉토리를 순회하며
 * 각 .pkg 파일의 이름과 버전을 출력.
 *
 * 반환: 설치된 패키지 수
 */
int pkg_list(void);

/*
 * 패키지 상세 정보 출력
 *
 * 반환: 0 성공, -1 실패
 */
int pkg_info(const char *name);

/*
 * 패키지가 설치되어 있는지 확인
 *
 * 반환: 1 설치됨, 0 설치 안 됨
 */
int pkg_is_installed(const char *name);

#endif /* CITCPKG_PACKAGE_H */
