/*
 * service.h - CITC OS 서비스 관리자
 * ==================================
 *
 * 서비스(Service)란?
 *   백그라운드에서 계속 실행되는 프로그램. "데몬(daemon)"이라고도 함.
 *   예: 네트워크 관리자, 디스플레이 서버, 오디오 서버, 로그 서비스
 *
 * 서비스 관리자가 하는 일:
 *   1. 서비스 정의 읽기 (어떤 프로그램을, 어떤 순서로 실행할지)
 *   2. 의존성 해석 (A가 B에 의존하면 B를 먼저 시작)
 *   3. 서비스 시작/정지/재시작
 *   4. 상태 추적 (실행 중? 죽었나? 재시작 몇 번?)
 *   5. 자동 재시작 (서비스가 예상치 못하게 죽으면 다시 시작)
 *
 * 실제 OS의 서비스 관리자들:
 *   - systemd (대부분의 Linux 배포판)
 *   - OpenRC (Gentoo)
 *   - runit (Void Linux)
 *   - s6 (경량)
 *   - citcinit (우리것!)
 *
 * 이 헤더는 서비스 관리자의 "인터페이스"를 정의합니다.
 * .h 파일은 "이런 함수들이 존재한다"고 알려주는 역할.
 * 실제 구현은 service.c에 있습니다.
 */

#ifndef CITCINIT_SERVICE_H
#define CITCINIT_SERVICE_H

/*
 * #ifndef ... #define ... #endif 패턴:
 *   "인클루드 가드(include guard)"라고 합니다.
 *   같은 헤더가 여러 번 포함되는 것을 방지.
 *   없으면 "중복 정의" 컴파일 에러가 남.
 */

#include <sys/types.h>

/* ============================================================
 * 서비스 상태 (Service State)
 * ============================================================
 *
 * 서비스의 생명주기:
 *
 *   STOPPED ──(start)──→ STARTING ──(ready)──→ RUNNING
 *      ▲                    │                     │
 *      │                    │(fail)               │(crash/stop)
 *      │                    ▼                     ▼
 *      └────────────── FAILED ←───────────── STOPPING
 *
 * enum이란?
 *   열거형. 관련된 상수들을 이름으로 묶어놓은 것.
 *   내부적으로는 0, 1, 2, 3... 정수이지만
 *   이름을 쓰면 코드 가독성이 훨씬 좋아짐.
 */
typedef enum {
	SVC_STOPPED,    /* 0: 정지됨 - 아직 시작하지 않았거나 정상 종료 */
	SVC_STARTING,   /* 1: 시작 중 - fork()는 했지만 아직 준비 안 됨 */
	SVC_RUNNING,    /* 2: 실행 중 - 정상 동작 */
	SVC_STOPPING,   /* 3: 정지 중 - SIGTERM 보냈고 종료 대기 */
	SVC_FAILED,     /* 4: 실패 - 비정상 종료 (재시작 시도 가능) */
} service_state_t;

/* ============================================================
 * 서비스 타입 (Service Type)
 * ============================================================
 *
 * 서비스마다 시작/관리 방식이 다릅니다:
 *
 *   SIMPLE:
 *     fork()하면 바로 "시작됨"으로 간주.
 *     대부분의 데몬이 이 타입.
 *     예: 로그 서비스, 간단한 데몬
 *
 *   ONESHOT:
 *     한 번 실행하고 종료. 종료 후 "완료"로 간주.
 *     예: 네트워크 설정 스크립트, 초기화 작업
 *     "ifconfig eth0 up" 같은 명령을 실행하고 끝.
 *
 *   NOTIFY:
 *     서비스가 준비되면 직접 알려줌.
 *     systemd의 sd_notify("READY=1")과 비슷.
 *     복잡한 초기화가 필요한 서비스에 사용.
 *     예: 데이터베이스 서버 (초기화에 수초 걸림)
 */
typedef enum {
	SVC_TYPE_SIMPLE,
	SVC_TYPE_ONESHOT,
	SVC_TYPE_NOTIFY,
} service_type_t;

/* ============================================================
 * 서비스 정의 (Service Definition)
 * ============================================================
 *
 * 하나의 서비스에 필요한 모든 정보를 담는 구조체.
 *
 * struct란?
 *   관련된 데이터를 하나로 묶는 C의 방법.
 *   클래스(class)와 비슷하지만, 메서드(함수)는 없음.
 *   C에서는 구조체 + 함수 포인터로 비슷한 효과를 냄.
 */

#define SVC_NAME_MAX      64    /* 서비스 이름 최대 길이 */
#define SVC_PATH_MAX      256   /* 실행 파일 경로 최대 길이 */
#define SVC_MAX_DEPS      16    /* 최대 의존성 개수 */
#define SVC_MAX_ARGS      32    /* 최대 인자 개수 */
#define SVC_MAX_SERVICES  64    /* 최대 서비스 개수 */
#define SVC_MAX_RESTARTS  5     /* 최대 자동 재시작 횟수 */

typedef struct service {
	/* === 식별 정보 === */
	char name[SVC_NAME_MAX];         /* 서비스 이름 (예: "network") */
	char description[128];            /* 설명 (예: "Network Manager") */

	/* === 실행 정보 === */
	char exec_path[SVC_PATH_MAX];    /* 실행 파일 경로 */
	char *args[SVC_MAX_ARGS];        /* 명령줄 인자 */
	service_type_t type;              /* 서비스 타입 */

	/* === 의존성 === */
	/*
	 * 의존성 = "이 서비스보다 먼저 시작되어야 하는 서비스 목록"
	 *
	 * 예: network 서비스가 depends = {"dbus", "syslog"} 이면
	 *     dbus와 syslog가 둘 다 RUNNING 상태여야
	 *     network를 시작할 수 있음.
	 */
	char *depends[SVC_MAX_DEPS];     /* 의존하는 서비스 이름 목록 */
	int num_depends;                  /* 의존성 개수 */

	/* === 재시작 정책 === */
	int auto_restart;                 /* 1: 죽으면 자동 재시작, 0: 안 함 */
	int restart_count;                /* 현재까지 재시작 횟수 */
	int max_restarts;                 /* 최대 재시작 횟수 (초과하면 FAILED) */

	/* === 소켓 활성화 (Socket Activation) === */
	/*
	 * 소켓 활성화란?
	 *   init이 미리 소켓을 만들어서 listen 상태로 대기.
	 *   누군가 소켓에 연결하면 → 서비스를 시작.
	 *   서비스에게 소켓 fd를 LISTEN_FDS 환경변수로 전달.
	 *
	 * systemd에서의 대응:
	 *   socket_path   ↔  [Socket] ListenStream=/path
	 *   listen_fd     ↔  내부 소켓 fd
	 *   LISTEN_FDS=1  ↔  sd_listen_fds() API
	 *
	 * 장점:
	 *   1. 부팅 빨라짐 (사용하지 않는 서비스는 시작 안 함)
	 *   2. 의존성 해결 단순화 (소켓만 있으면 됨)
	 *   3. 서비스 재시작 시 클라이언트 연결이 끊기지 않음
	 *      (소켓은 init이 들고 있으므로)
	 */
	char socket_path[SVC_PATH_MAX];  /* Unix socket 경로 (""이면 미사용) */
	int listen_fd;                    /* init이 만든 리스닝 소켓 fd (-1=미사용) */
	int socket_activated;             /* 1이면 소켓 활성화 서비스 */

	/* === 런타임 상태 === */
	service_state_t state;            /* 현재 상태 */
	pid_t pid;                        /* 프로세스 ID (실행 중일 때) */
	int exit_code;                    /* 마지막 종료 코드 */
} service_t;

/* ============================================================
 * 서비스 관리자 API
 * ============================================================
 * 외부에서 호출할 수 있는 함수들.
 */

/* 서비스 관리자 초기화 */
void svc_manager_init(void);

/*
 * 서비스 등록
 * 서비스 정의를 관리자에 추가.
 * 반환: 0 성공, -1 실패
 */
int svc_register(const char *name, const char *exec_path,
		 service_type_t type, int auto_restart);

/*
 * 서비스에 명령줄 인자 추가
 *
 * 왜 필요한가?
 *   많은 데몬은 기본적으로 "데몬화"(double-fork 후 백그라운드 실행)를 함.
 *   init 시스템이 서비스를 관리하려면 PID를 추적해야 하는데,
 *   데몬이 자체적으로 fork()하면 PID가 바뀌어서 추적 불가.
 *
 *   해결: -n (no-fork) 플래그로 포그라운드 실행 요청.
 *   이것이 현대 init 시스템(systemd, s6, runit)의 표준 방식.
 *
 *   예: svc_add_arg("syslog", "-n");  → syslogd -n (포그라운드)
 */
int svc_add_arg(const char *name, const char *arg);

/*
 * 서비스 의존성 추가
 * "name 서비스는 dep_name 서비스에 의존한다"
 */
int svc_add_dependency(const char *name, const char *dep_name);

/*
 * 모든 서비스를 의존성 순서대로 시작
 *
 * 내부적으로 위상 정렬(topological sort)을 사용.
 *
 * 위상 정렬이란?
 *   의존성 그래프에서 "안전한 실행 순서"를 찾는 알고리즘.
 *   A→B (A가 B에 의존) 관계가 있으면 B가 A보다 앞에 옴.
 *
 *   예시:
 *     syslog → (없음)          → 1번째로 시작
 *     dbus → (없음)            → 1번째로 시작 (syslog와 병렬 가능)
 *     network → {dbus}         → dbus 다음에 시작
 *     compositor → {dbus}      → dbus 다음에 시작
 *     desktop → {compositor}   → compositor 다음에 시작
 */
int svc_start_all(void);

/* 특정 서비스 시작 */
int svc_start(const char *name);

/* 특정 서비스 정지 */
int svc_stop(const char *name);

/*
 * 프로세스 종료 알림
 * PID 1의 좀비 리퍼에서 호출.
 * 어떤 서비스의 프로세스가 죽었는지 확인하고
 * 상태를 업데이트하거나 자동 재시작.
 */
void svc_notify_exit(pid_t pid, int status);

/* 모든 서비스 정지 (시스템 종료 시) */
void svc_stop_all(void);

/* 서비스 상태 출력 (디버그용) */
void svc_print_status(void);

/* 서비스 이름으로 상태 반환 */
const char *svc_state_str(service_state_t state);

/*
 * 서비스에 소켓 경로 설정
 * 이 소켓은 init이 미리 생성하고 listen 상태로 유지.
 * 연결이 오면 서비스를 시작하고 fd를 전달.
 */
int svc_set_socket(const char *name, const char *socket_path);

/*
 * 소켓 활성화된 서비스 찾기 (listen_fd로 검색)
 * poll()에서 활동이 감지된 fd에 해당하는 서비스를 찾는다.
 */
service_t *svc_find_by_listen_fd(int fd);

#endif /* CITCINIT_SERVICE_H */
