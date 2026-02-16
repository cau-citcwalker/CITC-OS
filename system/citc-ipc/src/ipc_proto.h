/*
 * ipc_proto.h - CITC IPC 프로토콜 정의
 * =======================================
 *
 * CITC IPC란?
 *   CITC OS의 프로세스 간 통신(IPC) 시스템.
 *   D-Bus의 핵심 개념을 교육적으로 단순화한 구현.
 *
 * D-Bus란?
 *   Linux 데스크탑의 표준 IPC 시스템. freedesktop.org에서 정의.
 *   거의 모든 데스크탑 앱이 D-Bus를 통해 통신합니다.
 *
 *   예시:
 *     - 음악 재생기가 "곡 변경됨" 시그널 발송
 *     - 파일 관리자가 "파일 열기" 요청을 앱에 전달
 *     - 네트워크 관리자가 "연결 상태 변경" 알림
 *     - 데스크탑 셸이 "앱 목록" 요청
 *
 * D-Bus vs CITC IPC 비교:
 *
 *   | D-Bus              | CITC IPC          |
 *   |--------------------|-------------------|
 *   | XML 인트로스펙션   | 없음 (단순화)     |
 *   | 타입 시스템 (sig)  | 고정 포맷         |
 *   | 매치 룰           | 전체 브로드캐스트  |
 *   | 인증 (SASL)       | 없음 (단순화)     |
 *   | 서비스 이름 (org.) | 짧은 문자열       |
 *   | 수천 줄           | ~300줄            |
 *
 *   핵심은 같음: 이름 등록 + 메시지 라우팅 + 브로드캐스트
 *
 * 프로토콜 구조:
 *   모든 메시지 = [헤더(12바이트)] + [페이로드(가변)]
 *
 *   헤더:
 *     type   (4B) = 메시지 종류
 *     length (4B) = 페이로드 길이
 *     serial (4B) = 메시지 일련번호 (응답 매칭용)
 *
 * 통신 소켓: /run/citc-ipc (Unix domain socket)
 */

#ifndef CITC_IPC_PROTO_H
#define CITC_IPC_PROTO_H

#include <stdint.h>

/* IPC 소켓 경로 */
#define CITC_IPC_SOCKET "/run/citc-ipc"

/* 이름 최대 길이 */
#define IPC_NAME_MAX 64

/* 데이터 최대 길이 */
#define IPC_DATA_MAX 256

/* ============================================================
 * 메시지 타입
 * ============================================================
 *
 * 클라이언트 → 버스:
 *   REGISTER   : "나는 이런 이름으로 서비스를 제공한다"
 *   SEND       : "이 이름의 서비스에게 메시지를 보내라"
 *   BROADCAST  : "모든 연결된 클라이언트에게 알림을 보내라"
 *
 * 버스 → 클라이언트:
 *   WELCOME    : "연결 성공, 너의 클라이언트 ID는 X이다"
 *   DELIVER    : "누군가 너에게 메시지를 보냈다"
 *   SIGNAL     : "누군가 브로드캐스트를 보냈다"
 *   ERROR      : "요청 처리 실패"
 */
enum ipc_msg_type {
	/* 클라이언트 → 버스 */
	IPC_MSG_REGISTER   = 1,   /* 서비스 이름 등록 */
	IPC_MSG_SEND       = 2,   /* 특정 서비스에 메시지 전달 */
	IPC_MSG_BROADCAST  = 3,   /* 모든 클라이언트에 브로드캐스트 */

	/* 버스 → 클라이언트 */
	IPC_MSG_WELCOME    = 100, /* 연결 확인 + 클라이언트 ID */
	IPC_MSG_DELIVER    = 101, /* 수신된 메시지 전달 */
	IPC_MSG_SIGNAL     = 102, /* 브로드캐스트 수신 */
	IPC_MSG_ERROR      = 103, /* 에러 응답 */
};

/* ============================================================
 * 메시지 헤더
 * ============================================================
 *
 * 모든 메시지의 앞 12바이트.
 * 고정 크기이므로 먼저 헤더를 읽고,
 * length만큼 추가로 읽으면 페이로드를 얻을 수 있음.
 *
 * serial 필드:
 *   요청-응답 매칭에 사용.
 *   클라이언트가 serial=42로 SEND → 서버가 serial=42로 DELIVER
 *   → 클라이언트는 "42번 요청의 응답"임을 알 수 있음.
 */
struct ipc_header {
	uint32_t type;     /* enum ipc_msg_type */
	uint32_t length;   /* 페이로드 길이 (바이트) */
	uint32_t serial;   /* 메시지 일련번호 */
};

/* ============================================================
 * 페이로드 구조체들
 * ============================================================ */

/* IPC_MSG_REGISTER 페이로드 */
struct ipc_register {
	char name[IPC_NAME_MAX];   /* 등록할 서비스 이름 (예: "display") */
};

/* IPC_MSG_SEND 페이로드 */
struct ipc_send {
	char destination[IPC_NAME_MAX]; /* 대상 서비스 이름 */
	char method[IPC_NAME_MAX];      /* 메서드/액션 이름 */
	uint32_t data_len;              /* data의 실제 길이 */
	uint8_t data[IPC_DATA_MAX];     /* 임의 데이터 */
};

/* IPC_MSG_BROADCAST 페이로드 */
struct ipc_broadcast {
	char sender[IPC_NAME_MAX];      /* 발신자 이름 */
	char signal_name[IPC_NAME_MAX]; /* 시그널 이름 (예: "package-installed") */
	uint32_t data_len;
	uint8_t data[IPC_DATA_MAX];
};

/* IPC_MSG_WELCOME 페이로드 */
struct ipc_welcome {
	uint32_t client_id;   /* 할당된 클라이언트 ID */
};

/* IPC_MSG_DELIVER 페이로드 (= ipc_send와 동일 구조) */
struct ipc_deliver {
	char sender[IPC_NAME_MAX];   /* 발신자 이름 */
	char method[IPC_NAME_MAX];   /* 메서드/액션 이름 */
	uint32_t data_len;
	uint8_t data[IPC_DATA_MAX];
};

/* IPC_MSG_ERROR 페이로드 */
struct ipc_error {
	uint32_t code;                /* 에러 코드 */
	char message[128];            /* 에러 메시지 */
};

#endif /* CITC_IPC_PROTO_H */
