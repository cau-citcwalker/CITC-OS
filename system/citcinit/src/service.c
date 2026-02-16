/*
 * service.c - CITC OS 서비스 관리자 구현
 * ========================================
 *
 * 이 파일은 서비스의 등록, 시작, 정지, 의존성 해석을 구현합니다.
 *
 * 핵심 알고리즘:
 *   - 위상 정렬 (Topological Sort): 의존성 순서 결정
 *   - fork() + execv(): 서비스 프로세스 시작
 *   - 상태 기계 (State Machine): 서비스 생명주기 관리
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#include "service.h"

/* 로그 매크로 (main.c와 동일) */
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

/* ============================================================
 * 전역 서비스 테이블
 * ============================================================
 *
 * 모든 등록된 서비스를 배열에 저장합니다.
 *
 * 왜 배열인가? (vs 링크드 리스트, 해시맵 등)
 *   - init 시스템의 서비스 수는 보통 수십 개 (적음)
 *   - 배열은 캐시 친화적 (메모리가 연속)
 *   - 구현이 단순 (malloc 불필요 → 메모리 부족 걱정 없음)
 *   - PID 1에서 malloc 실패는 치명적이므로 정적 할당이 안전
 */
/*
 * 전역 서비스 테이블 (extern으로 socket_activation.c에서도 접근)
 *
 * 원래는 static이 맞지만, 소켓 활성화 모듈이
 * 서비스 목록을 순회해야 하므로 extern으로 변경.
 * 같은 init 프로세스 내의 모듈이므로 캡슐화 위반이 아님.
 */
service_t services[SVC_MAX_SERVICES];
int num_services = 0;

/* ============================================================
 * 헬퍼 함수: 이름으로 서비스 찾기
 * ============================================================
 * 선형 탐색 O(n). 서비스 수가 적으므로 충분.
 */
static service_t *find_service(const char *name)
{
	for (int i = 0; i < num_services; i++) {
		if (strcmp(services[i].name, name) == 0)
			return &services[i];
	}
	return NULL;
}

/* 상태를 문자열로 변환 */
const char *svc_state_str(service_state_t state)
{
	switch (state) {
	case SVC_STOPPED:  return "STOPPED";
	case SVC_STARTING: return "STARTING";
	case SVC_RUNNING:  return "RUNNING";
	case SVC_STOPPING: return "STOPPING";
	case SVC_FAILED:   return "FAILED";
	default:           return "UNKNOWN";
	}
}

/* ============================================================
 * 서비스 관리자 초기화
 * ============================================================ */
void svc_manager_init(void)
{
	memset(services, 0, sizeof(services));
	num_services = 0;
	LOG_INFO("Service manager initialized");
}

/* ============================================================
 * 서비스 등록
 * ============================================================ */
int svc_register(const char *name, const char *exec_path,
		 service_type_t type, int auto_restart)
{
	service_t *svc;

	if (num_services >= SVC_MAX_SERVICES) {
		LOG_FAIL("Max service count exceeded (%d)", SVC_MAX_SERVICES);
		return -1;
	}

	if (find_service(name) != NULL) {
		LOG_WARN("Service '%s' already registered", name);
		return -1;
	}

	svc = &services[num_services++];
	memset(svc, 0, sizeof(*svc));

	snprintf(svc->name, SVC_NAME_MAX, "%s", name);
	snprintf(svc->exec_path, SVC_PATH_MAX, "%s", exec_path);
	svc->type = type;
	svc->auto_restart = auto_restart;
	svc->max_restarts = SVC_MAX_RESTARTS;
	svc->state = SVC_STOPPED;
	svc->pid = 0;

	return 0;
}

/* ============================================================
 * 명령줄 인자 추가
 * ============================================================ */
int svc_add_arg(const char *name, const char *arg)
{
	service_t *svc = find_service(name);

	if (!svc) {
		LOG_FAIL("Service '%s' not found", name);
		return -1;
	}

	/*
	 * args 배열 구조:
	 *   args[0] = exec_path (argv[0] = 프로그램 이름)
	 *   args[1] = 첫 번째 인자
	 *   args[2] = 두 번째 인자
	 *   ...
	 *   args[n] = NULL (종료 표시)
	 *
	 * 처음 호출 시 args[0]을 exec_path로 설정.
	 */
	int idx = 0;

	/* 이미 설정된 인자 수 세기 */
	while (idx < SVC_MAX_ARGS - 1 && svc->args[idx] != NULL)
		idx++;

	/* 첫 인자 추가 시 argv[0]을 먼저 설정 */
	if (idx == 0) {
		svc->args[0] = svc->exec_path;
		idx = 1;
	}

	if (idx >= SVC_MAX_ARGS - 1) {
		LOG_FAIL("Service '%s' max args exceeded", name);
		return -1;
	}

	svc->args[idx] = strdup(arg);
	svc->args[idx + 1] = NULL;  /* NULL 종료 보장 */

	return 0;
}

/* ============================================================
 * 의존성 추가
 * ============================================================ */
int svc_add_dependency(const char *name, const char *dep_name)
{
	service_t *svc = find_service(name);

	if (!svc) {
		LOG_FAIL("Service '%s' not found", name);
		return -1;
	}

	if (svc->num_depends >= SVC_MAX_DEPS) {
		LOG_FAIL("Service '%s' max deps exceeded", name);
		return -1;
	}

	/*
	 * 의존 대상이 등록되어 있는지 확인.
	 * 등록 안 된 서비스에 의존하면 부팅이 막힐 수 있음.
	 */
	if (!find_service(dep_name)) {
		LOG_WARN("Service '%s' dependency '%s' not registered",
			 name, dep_name);
	}

	/*
	 * strdup()은 문자열을 복사하여 새 메모리에 저장.
	 * PID 1에서는 메모리 누수를 신경쓸 필요 없음
	 * (프로세스가 종료될 때까지 살아있으니까).
	 */
	svc->depends[svc->num_depends++] = strdup(dep_name);

	return 0;
}

/* ============================================================
 * 의존성 충족 확인
 * ============================================================
 * 이 서비스의 모든 의존성이 RUNNING 상태인가?
 */
static int deps_satisfied(const service_t *svc)
{
	for (int i = 0; i < svc->num_depends; i++) {
		const service_t *dep = find_service(svc->depends[i]);

		if (!dep)
			return 0;  /* 의존 대상이 없음 → 불만족 */

		/*
		 * ONESHOT 타입은 실행 완료 후 STOPPED이 정상.
		 * SIMPLE/NOTIFY 타입은 RUNNING이어야 함.
		 */
		if (dep->type == SVC_TYPE_ONESHOT) {
			/* ONESHOT은 한 번 실행되고 정상 종료되면 OK */
			if (dep->state != SVC_STOPPED || dep->exit_code != 0) {
				if (dep->state == SVC_STOPPED && dep->pid == 0
				    && dep->restart_count == 0) {
					return 0;  /* 아직 한 번도 실행 안 됨 */
				}
			}
		} else {
			if (dep->state != SVC_RUNNING)
				return 0;
		}
	}
	return 1;
}

/* ============================================================
 * 단일 서비스 시작
 * ============================================================
 *
 * fork() + execv() 패턴으로 서비스 프로세스를 생성합니다.
 *
 * 서비스 프로세스의 특징 (일반 프로세스와 다른 점):
 *   1. setsid()로 새 세션 생성 → 터미널에 묶이지 않음
 *   2. stdin/stdout/stderr를 /dev/null로 리다이렉트
 *      (데몬은 터미널에 출력하지 않음, 로그 파일을 사용)
 *   3. 작업 디렉토리를 /로 변경
 *      (서비스가 특정 디렉토리에 의존하지 않게)
 */
int svc_start(const char *name)
{
	service_t *svc = find_service(name);
	pid_t pid;

	if (!svc) {
		LOG_FAIL("Service '%s' not found", name);
		return -1;
	}

	if (svc->state == SVC_RUNNING) {
		LOG_WARN("Service '%s' already running (PID=%d)", name, svc->pid);
		return 0;
	}

	/* 의존성 확인 */
	if (!deps_satisfied(svc)) {
		LOG_WARN("Service '%s' deps not met, deferred", name);
		return -1;
	}

	/* 실행 파일 존재 확인 */
	if (access(svc->exec_path, X_OK) != 0) {
		LOG_FAIL("Service '%s' executable not found: %s", name, svc->exec_path);
		svc->state = SVC_FAILED;
		return -1;
	}

	svc->state = SVC_STARTING;

	pid = fork();

	if (pid < 0) {
		LOG_FAIL("Service '%s' fork() failed: %s", name, strerror(errno));
		svc->state = SVC_FAILED;
		return -1;
	}

	if (pid == 0) {
		/* === 자식 프로세스: 서비스 실행 === */

		/* 새 세션 생성 (터미널에서 분리) */
		setsid();

		/* 작업 디렉토리를 루트로 변경 */
		if (chdir("/") < 0)
			_exit(1);

		/*
		 * 소켓 활성화: listen fd를 fd 3으로 전달
		 *
		 * LISTEN_FDS 프로토콜 (sd_listen_fds):
		 *   fd 3부터 시작하여 N개의 소켓 fd를 전달.
		 *   환경변수 LISTEN_FDS=N, LISTEN_PID=<pid> 설정.
		 *
		 *   서비스는 getenv("LISTEN_FDS")로 확인하고,
		 *   fd 3을 사용하여 accept() 호출.
		 */
		char listen_fds_str[16] = "";
		char listen_pid_str[32] = "";

		if (svc->socket_activated && svc->listen_fd >= 0) {
			/*
			 * dup2(listen_fd, 3): listen_fd를 fd 3으로 복제.
			 * 이미 fd 3이면 아무것도 안 함.
			 */
			if (svc->listen_fd != 3) {
				if (dup2(svc->listen_fd, 3) < 0)
					_exit(1);
				close(svc->listen_fd);
			}
			snprintf(listen_fds_str, sizeof(listen_fds_str),
				 "LISTEN_FDS=1");
			snprintf(listen_pid_str, sizeof(listen_pid_str),
				 "LISTEN_PID=%d", getpid());
		}

		/*
		 * 환경 변수 설정.
		 * 서비스는 최소한의 깨끗한 환경에서 시작해야 함.
		 *
		 * 소켓 활성화 서비스는 추가로 LISTEN_FDS, LISTEN_PID를 받음.
		 */
		char *envp[] = {
			"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
			"HOME=/",
			"TERM=linux",
			listen_fds_str[0] ? listen_fds_str : NULL,
			listen_pid_str[0] ? listen_pid_str : NULL,
			NULL,
		};

		/*
		 * envp에서 NULL 항목을 제거 (compact)
		 * 소켓 활성화가 아닌 서비스는 LISTEN_FDS가 없으므로
		 * NULL이 중간에 올 수 있음 → 압축 필요.
		 */
		int dst = 0;
		for (int i = 0; i < 5; i++) {
			if (envp[i] != NULL)
				envp[dst++] = envp[i];
		}
		envp[dst] = NULL;

		/*
		 * execve()로 서비스 프로그램 실행.
		 *
		 * args가 설정되어 있으면 그것을 argv로 사용.
		 * 없으면 기본 argv (프로그램 이름만) 사용.
		 *
		 * 예: syslogd -n
		 *   args[0] = "/sbin/syslogd"
		 *   args[1] = "-n"
		 *   args[2] = NULL
		 */
		if (svc->args[0] != NULL) {
			execve(svc->exec_path, svc->args, envp);
		} else {
			char *argv[] = { svc->exec_path, NULL };
			execve(svc->exec_path, argv, envp);
		}

		/* execve 실패 */
		_exit(127);
	}

	/* === 부모 프로세스: PID 기록 === */
	svc->pid = pid;

	/*
	 * SIMPLE 타입은 fork 성공 = 시작 완료로 간주.
	 * NOTIFY 타입은 서비스가 직접 알려줄 때까지 STARTING 유지.
	 * ONESHOT 타입은 프로세스 종료까지 STARTING 유지.
	 */
	if (svc->type == SVC_TYPE_SIMPLE)
		svc->state = SVC_RUNNING;

	LOG_OK("Service '%s' started (PID=%d, type=%s)",
	       name, pid,
	       svc->type == SVC_TYPE_SIMPLE ? "simple" :
	       svc->type == SVC_TYPE_ONESHOT ? "oneshot" : "notify");

	return 0;
}

/* ============================================================
 * 서비스 정지
 * ============================================================
 *
 * 서비스를 정지하는 표준 절차:
 *   1. SIGTERM 전송 (정상 종료 요청)
 *   2. 일정 시간 대기 (프로세스가 정리할 시간)
 *   3. 아직 살아있으면 SIGKILL (강제 종료)
 *
 * SIGTERM vs SIGKILL:
 *   SIGTERM: "좀 종료해줄래?" → 프로세스가 무시하거나 처리 가능
 *            정상적인 종료 (파일 저장, 소켓 닫기 등) 가능
 *   SIGKILL: "지금 당장 죽어" → 프로세스가 무시 불가
 *            즉시 종료 (정리 작업 없음, 데이터 유실 가능)
 */
int svc_stop(const char *name)
{
	service_t *svc = find_service(name);

	if (!svc) {
		LOG_FAIL("Service '%s' not found", name);
		return -1;
	}

	if (svc->state != SVC_RUNNING && svc->state != SVC_STARTING) {
		return 0;  /* 이미 정지됨 */
	}

	LOG_INFO("Stopping service '%s' (PID=%d)...", name, svc->pid);
	svc->state = SVC_STOPPING;

	/* SIGTERM 전송 */
	if (svc->pid > 0)
		kill(svc->pid, SIGTERM);

	/*
	 * 실제로는 여기서 타이머를 걸어서 N초 후에도 안 죽으면
	 * SIGKILL을 보내야 합니다.
	 * v0.2에서는 메인 루프에서 타임아웃을 체크하도록 개선할 예정.
	 * 지금은 SIGTERM만 보내고 reap_zombies에서 처리.
	 */

	return 0;
}

/* ============================================================
 * 프로세스 종료 알림 처리
 * ============================================================
 *
 * PID 1의 좀비 리퍼(reap_zombies)에서 호출됩니다.
 * "PID XX 프로세스가 종료코드 YY로 죽었다"
 * → 어떤 서비스인지 찾아서 상태 업데이트
 * → 자동 재시작 정책에 따라 재시작
 */
void svc_notify_exit(pid_t pid, int status)
{
	service_t *svc = NULL;

	/* PID로 서비스 찾기 */
	for (int i = 0; i < num_services; i++) {
		if (services[i].pid == pid) {
			svc = &services[i];
			break;
		}
	}

	if (!svc)
		return;  /* 등록된 서비스가 아님 (쉘 등) */

	/* 종료 코드 기록 */
	if (WIFEXITED(status))
		svc->exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		svc->exit_code = 128 + WTERMSIG(status);

	svc->pid = 0;

	/* ONESHOT 타입: 정상 종료면 성공 */
	if (svc->type == SVC_TYPE_ONESHOT) {
		if (svc->exit_code == 0) {
			svc->state = SVC_STOPPED;
			LOG_OK("Service '%s' completed (oneshot)", svc->name);
		} else {
			svc->state = SVC_FAILED;
			LOG_FAIL("Service '%s' failed (exit=%d)",
				 svc->name, svc->exit_code);
		}
		return;
	}

	/* 정지 요청에 의한 종료 */
	if (svc->state == SVC_STOPPING) {
		svc->state = SVC_STOPPED;
		LOG_OK("Service '%s' stopped", svc->name);
		return;
	}

	/* 예상치 못한 종료 → 자동 재시작 판단 */
	LOG_WARN("Service '%s' unexpected exit (exit=%d, restarts=%d/%d)",
		 svc->name, svc->exit_code,
		 svc->restart_count, svc->max_restarts);

	if (svc->auto_restart && svc->restart_count < svc->max_restarts) {
		svc->restart_count++;
		LOG_INFO("Service '%s' auto-restart (%d/%d)...",
			 svc->name, svc->restart_count, svc->max_restarts);
		svc->state = SVC_STOPPED;
		svc_start(svc->name);
	} else {
		svc->state = SVC_FAILED;
		if (svc->auto_restart) {
			LOG_FAIL("Service '%s' max restarts exceeded!", svc->name);
		}
	}
}

/* ============================================================
 * 위상 정렬로 모든 서비스 시작
 * ============================================================
 *
 * 위상 정렬 (Topological Sort)이란?
 *   방향 비순환 그래프(DAG)에서 모든 노드를
 *   의존성을 위반하지 않는 순서로 나열하는 알고리즘.
 *
 * 예시:
 *   서비스:     syslog, dbus, network, compositor, desktop
 *   의존성:     network → dbus
 *               compositor → dbus
 *               desktop → compositor
 *
 *   의존성 그래프:
 *     syslog     dbus
 *                ↑  ↑
 *         network   compositor
 *                      ↑
 *                    desktop
 *
 *   위상 정렬 결과 (여러 가지 가능):
 *     [syslog, dbus, network, compositor, desktop]
 *     [dbus, syslog, compositor, network, desktop]
 *     등등...
 *
 * 알고리즘 (Kahn's Algorithm):
 *   1. 각 노드의 "진입 차수"(in-degree) 계산
 *      → 이 노드를 가리키는 간선 수 = 이 노드에 의존하는 서비스 수
 *      (반대로 생각: 이 서비스가 의존하는 서비스 수)
 *   2. 진입 차수가 0인 노드를 큐에 넣기
 *      → 아무것에도 의존하지 않는 서비스 = 먼저 시작 가능
 *   3. 큐에서 꺼내서 시작하고, 이 노드가 충족시키는 다른 노드의 진입 차수 감소
 *   4. 새로 진입 차수가 0이 된 노드를 큐에 추가
 *   5. 큐가 빌 때까지 반복
 *
 *   만약 모든 노드를 처리하지 못하면? → 순환 의존성(circular dependency) 존재!
 *   A→B→C→A 같은 상황. 이건 해결 불가능 → 에러.
 */
int svc_start_all(void)
{
	/*
	 * in_degree[i]: services[i]가 의존하는 서비스 중
	 *               아직 시작되지 않은 것의 수.
	 *               0이 되면 시작 가능.
	 */
	int in_degree[SVC_MAX_SERVICES] = {0};

	/* 큐: 시작 가능한 서비스 인덱스 */
	int queue[SVC_MAX_SERVICES];
	int q_front = 0, q_back = 0;

	/*
	 * 버그 수정: started와 processed를 분리.
	 *
	 * processed = 위상 정렬에서 꺼낸 서비스 수 (시작 시도)
	 * started   = 실제로 시작 성공한 서비스 수
	 *
	 * 순환 의존성 판단은 processed로 해야 함.
	 * started로 하면 "실행 파일이 없는 것"을 "순환 의존성"으로 오인함.
	 */
	int processed = 0;
	int started = 0;

	LOG_INFO("Starting services (topological sort)...");

	/* 1단계: 진입 차수 계산 */
	for (int i = 0; i < num_services; i++) {
		in_degree[i] = services[i].num_depends;
	}

	/* 2단계: 진입 차수 0인 서비스를 큐에 */
	for (int i = 0; i < num_services; i++) {
		if (in_degree[i] == 0)
			queue[q_back++] = i;
	}

	/* 3단계: BFS로 순서대로 시작 */
	while (q_front < q_back) {
		int idx = queue[q_front++];
		service_t *svc = &services[idx];

		/* 서비스 시작 시도 */
		processed++;
		if (svc_start(svc->name) == 0)
			started++;

		/*
		 * 이 서비스를 의존하고 있던 다른 서비스들의
		 * 진입 차수를 감소시킴.
		 *
		 * 주의: 서비스 시작이 실패해도 진입 차수는 감소시킴.
		 * 이유: 위상 정렬의 목적은 "순서 결정"이지 "성공 보장"이 아님.
		 * 의존 대상이 실패해도 의존하는 서비스에게 시도할 기회를 줌.
		 * (svc_start 내부에서 deps_satisfied가 실제 상태를 체크함)
		 */
		for (int i = 0; i < num_services; i++) {
			for (int j = 0; j < services[i].num_depends; j++) {
				if (strcmp(services[i].depends[j],
					  svc->name) == 0) {
					in_degree[i]--;
					if (in_degree[i] == 0)
						queue[q_back++] = i;
				}
			}
		}
	}

	/*
	 * 순환 의존성 확인:
	 * processed < num_services 이면 일부 서비스가 큐에 들어가지 못한 것.
	 * → 진입 차수가 0이 되지 않음 → 순환 의존성 존재.
	 */
	if (processed < num_services) {
		LOG_WARN("Circular dependency! %d services unresolvable",
			 num_services - processed);

		for (int i = 0; i < num_services; i++) {
			if (in_degree[i] > 0) {
				LOG_FAIL("  '%s' (%d unmet deps)",
					 services[i].name, in_degree[i]);
			}
		}
	}

	LOG_OK("%d/%d services started", started, num_services);
	return started;
}

/* ============================================================
 * 모든 서비스 정지 (역순)
 * ============================================================
 * 시작할 때와 반대 순서로 정지해야 합니다.
 * 의존하는 서비스가 먼저 정지되어야 안전.
 */
void svc_stop_all(void)
{
	LOG_INFO("Stopping all services...");

	/* 역순으로 정지 (나중에 시작된 것부터) */
	for (int i = num_services - 1; i >= 0; i--) {
		if (services[i].state == SVC_RUNNING ||
		    services[i].state == SVC_STARTING) {
			svc_stop(services[i].name);
		}
	}
}

/* ============================================================
 * 소켓 경로 설정
 * ============================================================
 * 이 서비스를 소켓 활성화 모드로 설정.
 * init이 이 경로에 소켓을 미리 만들어 listen.
 */
int svc_set_socket(const char *name, const char *socket_path)
{
	service_t *svc = find_service(name);

	if (!svc) {
		LOG_FAIL("Service '%s' not found", name);
		return -1;
	}

	snprintf(svc->socket_path, sizeof(svc->socket_path),
		 "%s", socket_path);
	svc->socket_activated = 1;
	svc->listen_fd = -1;  /* 아직 소켓 미생성 */

	return 0;
}

/* ============================================================
 * listen fd로 서비스 찾기
 * ============================================================
 * poll()에서 활동이 감지된 fd에 해당하는 서비스를 찾는다.
 */
service_t *svc_find_by_listen_fd(int fd)
{
	for (int i = 0; i < num_services; i++) {
		if (services[i].socket_activated &&
		    services[i].listen_fd == fd)
			return &services[i];
	}
	return NULL;
}

/* ============================================================
 * 서비스 상태 출력
 * ============================================================ */
void svc_print_status(void)
{
	printf("\n");
	printf("  %-20s %-10s %6s  %s\n",
	       "SERVICE", "STATE", "PID", "RESTARTS");
	printf("  %-20s %-10s %6s  %s\n",
	       "-------", "-----", "---", "--------");

	for (int i = 0; i < num_services; i++) {
		service_t *svc = &services[i];
		const char *color;

		switch (svc->state) {
		case SVC_RUNNING:  color = COLOR_GREEN; break;
		case SVC_FAILED:   color = COLOR_RED; break;
		case SVC_STARTING:
		case SVC_STOPPING: color = COLOR_YELLOW; break;
		default:           color = COLOR_RESET; break;
		}

		printf("  %-20s %s%-10s%s %6d  %d/%d\n",
		       svc->name,
		       color, svc_state_str(svc->state), COLOR_RESET,
		       svc->pid,
		       svc->restart_count, svc->max_restarts);
	}
	printf("\n");
}
