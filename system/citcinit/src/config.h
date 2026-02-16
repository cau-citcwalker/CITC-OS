/*
 * config.h - CITC OS 서비스 설정 파일 파서
 * ============================================
 *
 * 서비스 정의 파일(.conf)을 읽어서 서비스를 등록합니다.
 *
 * 왜 설정 파일이 필요한가?
 *   코드에 서비스를 하드코딩하면:
 *   - 서비스 추가/수정 시 재컴파일 필요
 *   - 사용자가 커스텀 서비스를 추가할 수 없음
 *   - 배포판마다 다른 서비스 구성이 불가능
 *
 *   설정 파일로 분리하면:
 *   - 텍스트 편집만으로 서비스 추가/수정
 *   - 패키지 매니저가 .conf 파일을 설치/제거
 *   - init 바이너리는 한 번 빌드하면 변경 불필요
 *
 * 설정 파일 포맷 (key=value):
 *   # 주석
 *   name=syslog            # 서비스 이름 (필수)
 *   exec=/sbin/syslogd     # 실행 파일 경로 (필수)
 *   type=simple             # simple, oneshot, notify (기본: simple)
 *   restart=yes             # 자동 재시작 yes/no (기본: no)
 *   args=-n                 # 명령줄 인자 (여러 번 사용 가능)
 *   depends=syslog          # 의존 서비스 (여러 번 사용 가능)
 *
 * 파일 위치: /etc/citc/services/ (확장자 .conf)
 */

#ifndef CITCINIT_CONFIG_H
#define CITCINIT_CONFIG_H

/*
 * 서비스 설정 디렉토리 경로
 * 이 디렉토리의 모든 .conf 파일을 읽습니다.
 */
#define SVC_CONFIG_DIR "/etc/citc/services"

/*
 * 설정 디렉토리에서 모든 .conf 파일을 읽고 서비스를 등록.
 *
 * 내부 동작:
 *   1. opendir()로 디렉토리 열기
 *   2. readdir()로 파일 목록 순회
 *   3. .conf로 끝나는 파일만 처리
 *   4. 각 파일을 파싱하여 svc_register() 호출
 *
 * 반환: 성공적으로 등록된 서비스 수
 */
int config_load_services(const char *config_dir);

/*
 * 단일 .conf 파일을 파싱하여 서비스 등록.
 *
 * C에서 파일 파싱의 기본 패턴:
 *   1. fopen()으로 파일 열기
 *   2. fgets()로 한 줄씩 읽기
 *   3. 각 줄을 파싱 (key=value 분리)
 *   4. fclose()로 파일 닫기
 *
 * 반환: 0 성공, -1 실패
 */
int config_load_file(const char *filepath);

#endif /* CITCINIT_CONFIG_H */
