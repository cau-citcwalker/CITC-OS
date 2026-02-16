/*
 * socket_activation.h - CITC OS 소켓 활성화 시스템
 * ==================================================
 *
 * 소켓 활성화(Socket Activation)란?
 *   init 시스템이 서비스 대신 미리 소켓을 만들어 두고,
 *   클라이언트가 연결하면 그때 서비스를 시작하는 기법.
 *
 *   systemd가 도입하여 현대 Linux의 표준이 된 기술.
 *   (systemd의 .socket 유닛 파일이 이것)
 *
 * 왜 소켓 활성화를 쓰는가?
 *
 *   1. 부팅 속도 향상:
 *      모든 서비스를 부팅 시 시작하지 않고,
 *      실제로 필요할 때만 시작. (lazy start)
 *
 *   2. 의존성 해결 단순화:
 *      서비스 B가 서비스 A에 의존할 때,
 *      A의 소켓만 준비되면 B를 바로 시작할 수 있음.
 *      A가 완전히 초기화될 때까지 기다릴 필요 없음.
 *      (B의 연결 요청은 커널의 소켓 백로그에 큐잉됨)
 *
 *   3. 서비스 재시작 시 연결 유지:
 *      소켓은 init이 들고 있으므로, 서비스가 재시작해도
 *      클라이언트의 연결이 끊기지 않음.
 *
 * LISTEN_FDS 프로토콜:
 *   systemd가 정의한 표준 프로토콜.
 *   서비스에 소켓 fd를 전달하는 방법.
 *
 *   환경변수:
 *     LISTEN_FDS=N     → fd가 N개 전달됨
 *     LISTEN_PID=<pid> → 이 PID에게 전달된 것
 *
 *   fd 번호:
 *     3부터 시작 (0=stdin, 1=stdout, 2=stderr 다음)
 *     LISTEN_FDS=1 → fd 3이 소켓
 *     LISTEN_FDS=2 → fd 3과 4가 소켓
 *
 *   서비스 코드에서:
 *     char *fds = getenv("LISTEN_FDS");
 *     if (fds && atoi(fds) > 0) {
 *         listen_fd = 3;  // init이 전달한 소켓 사용
 *     } else {
 *         listen_fd = socket(...);  // 직접 소켓 생성
 *     }
 *
 * self-pipe 트릭:
 *   poll()은 시그널을 직접 감지하지 못함.
 *   pause()는 시그널이 오면 깨어나지만,
 *   poll()은 fd 이벤트만 감지.
 *
 *   해결: 시그널 핸들러에서 pipe에 1바이트를 write.
 *   poll()이 이 pipe의 POLLIN을 감지해서 깨어남.
 *
 *   이것이 "self-pipe trick" — Unix 프로그래밍의 클래식 패턴.
 *   (Daniel J. Bernstein이 고안)
 */

#ifndef CITCINIT_SOCKET_ACTIVATION_H
#define CITCINIT_SOCKET_ACTIVATION_H

#include <poll.h>

/*
 * 소켓 활성화 초기화
 *
 * 모든 등록된 서비스를 순회하면서
 * socket_path가 설정된 서비스의 리스닝 소켓을 생성.
 *
 * 호출 시점: svc_start_all() 이전.
 * 소켓이 먼저 준비되어야 다른 서비스들이 연결 가능.
 *
 * 반환: 생성된 소켓 개수 (0이면 소켓 활성화 서비스 없음)
 */
int sa_init(void);

/*
 * self-pipe 생성
 *
 * 시그널 핸들러에서 이 pipe에 write하면
 * 메인 루프의 poll()이 깨어남.
 *
 * 반환: 0 성공, -1 실패
 */
int sa_create_signal_pipe(void);

/*
 * 시그널 핸들러에서 호출 — poll()을 깨우기 위해 pipe에 write
 *
 * 중요: 이 함수는 시그널 핸들러 안에서 호출되므로
 *       async-signal-safe 함수만 사용해야 함.
 *       write()는 POSIX에서 async-signal-safe로 보장됨.
 */
void sa_signal_notify(void);

/*
 * poll()용 fd 배열 구성
 *
 * 모든 리스닝 소켓 fd + self-pipe read fd를 fds 배열에 채움.
 *
 * 반환: 채워진 fd 개수
 */
int sa_build_poll_fds(struct pollfd *fds, int max_fds);

/*
 * poll() 이벤트 처리
 *
 * poll()이 반환된 후 호출.
 * - 리스닝 소켓에 POLLIN → 해당 서비스 시작 + fd 전달
 * - self-pipe에 POLLIN → pipe 비우기 (시그널 처리는 main에서)
 *
 * 반환: 처리된 이벤트 수
 */
int sa_handle_events(struct pollfd *fds, int nfds);

/*
 * 정리 (시스템 종료 시)
 * 모든 리스닝 소켓과 self-pipe 닫기.
 */
void sa_cleanup(void);

#endif /* CITCINIT_SOCKET_ACTIVATION_H */
