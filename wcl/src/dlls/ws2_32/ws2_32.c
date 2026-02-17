/*
 * ws2_32.c — CITC OS Winsock2 구현
 * ==================================
 *
 * Windows 소켓 API를 POSIX 소켓으로 1:1 매핑.
 *
 * 핵심 매핑:
 *   SOCKET           → int (POSIX fd)
 *   INVALID_SOCKET   → -1
 *   SOCKET_ERROR     → -1
 *   WSAStartup       → no-op (Linux 소켓은 초기화 불필요)
 *   WSAGetLastError   → errno → WSA 에러코드 변환
 *   closesocket      → close()
 *   socket/bind/listen/accept/connect/send/recv → 동일 이름
 *
 * 주의: Windows SOCKET은 unsigned, POSIX fd는 signed.
 *       앱이 INVALID_SOCKET(-1 or ~0)을 체크하므로 문제없음.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "ws2_32.h"

/* ============================================================
 * Winsock 상수
 * ============================================================ */

#define WSA_INVALID_SOCKET  ((uintptr_t)-1)
#define WSA_SOCKET_ERROR    (-1)

/* WSA 에러코드 */
#define WSAEWOULDBLOCK      10035
#define WSAEINPROGRESS      10036
#define WSAEALREADY         10037
#define WSAENOTSOCK         10038
#define WSAEMSGSIZE         10040
#define WSAEADDRINUSE       10048
#define WSAEADDRNOTAVAIL    10049
#define WSAECONNREFUSED     10061
#define WSAETIMEDOUT        10060
#define WSAENETUNREACH      10051
#define WSAECONNRESET       10054
#define WSAENOTCONN         10057
#define WSAECONNABORTED     10053
#define WSAEINVAL           10022
#define WSAEISCONN          10056
#define WSANOTINITIALISED   10093

/* WSADATA 구조체 (간소화) */
struct wsadata {
	uint16_t wVersion;
	uint16_t wHighVersion;
	char szDescription[257];
	char szSystemStatus[129];
	uint16_t iMaxSockets;
	uint16_t iMaxUdpDg;
	char *lpVendorInfo;
};

/* ============================================================
 * errno → WSA 에러코드 변환
 * ============================================================ */

static int last_wsa_error = 0;

static int errno_to_wsa(int err)
{
	switch (err) {
	case EWOULDBLOCK:  return WSAEWOULDBLOCK;
#if EAGAIN != EWOULDBLOCK
	case EAGAIN:       return WSAEWOULDBLOCK;
#endif
	case EINPROGRESS:  return WSAEINPROGRESS;
	case EALREADY:     return WSAEALREADY;
	case ENOTSOCK:     return WSAENOTSOCK;
	case EMSGSIZE:     return WSAEMSGSIZE;
	case EADDRINUSE:   return WSAEADDRINUSE;
	case EADDRNOTAVAIL:return WSAEADDRNOTAVAIL;
	case ECONNREFUSED: return WSAECONNREFUSED;
	case ETIMEDOUT:    return WSAETIMEDOUT;
	case ENETUNREACH:  return WSAENETUNREACH;
	case ECONNRESET:   return WSAECONNRESET;
	case ENOTCONN:     return WSAENOTCONN;
	case ECONNABORTED: return WSAECONNABORTED;
	case EINVAL:       return WSAEINVAL;
	case EISCONN:      return WSAEISCONN;
	default:           return err + 10000;
	}
}

static void set_wsa_error(void)
{
	last_wsa_error = errno_to_wsa(errno);
}

/* ============================================================
 * WSAStartup / WSACleanup / WSAGetLastError
 * ============================================================ */

__attribute__((ms_abi))
static int ws_WSAStartup(uint16_t wVersionRequested, struct wsadata *lpWSAData)
{
	(void)wVersionRequested;

	if (lpWSAData) {
		memset(lpWSAData, 0, sizeof(*lpWSAData));
		lpWSAData->wVersion = 0x0202;     /* 2.2 */
		lpWSAData->wHighVersion = 0x0202;

		/* 안전한 최소 복사 */
		const char *desc = "CITC Winsock 2.2";
		size_t dlen = strlen(desc);

		if (dlen > 256) dlen = 256;
		memcpy(lpWSAData->szDescription, desc, dlen);
		lpWSAData->szDescription[dlen] = '\0';

		const char *status = "Running";
		size_t slen = strlen(status);

		if (slen > 128) slen = 128;
		memcpy(lpWSAData->szSystemStatus, status, slen);
		lpWSAData->szSystemStatus[slen] = '\0';

		lpWSAData->iMaxSockets = 1024;
		lpWSAData->iMaxUdpDg = 65507;
	}

	last_wsa_error = 0;
	return 0;
}

__attribute__((ms_abi))
static int ws_WSACleanup(void)
{
	return 0;
}

__attribute__((ms_abi))
static int ws_WSAGetLastError(void)
{
	return last_wsa_error;
}

__attribute__((ms_abi))
static void ws_WSASetLastError(int iError)
{
	last_wsa_error = iError;
}

/* ============================================================
 * 소켓 생성 / 해제
 * ============================================================ */

__attribute__((ms_abi))
static uintptr_t ws_socket(int af, int type, int protocol)
{
	int fd = socket(af, type, protocol);

	if (fd < 0) {
		set_wsa_error();
		return WSA_INVALID_SOCKET;
	}

	return (uintptr_t)fd;
}

__attribute__((ms_abi))
static int ws_closesocket(uintptr_t s)
{
	if (close((int)s) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

/* ============================================================
 * 연결
 * ============================================================ */

__attribute__((ms_abi))
static int ws_bind(uintptr_t s, const struct sockaddr *name, int namelen)
{
	if (bind((int)s, name, (socklen_t)namelen) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

__attribute__((ms_abi))
static int ws_listen(uintptr_t s, int backlog)
{
	if (listen((int)s, backlog) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

__attribute__((ms_abi))
static uintptr_t ws_accept(uintptr_t s, struct sockaddr *addr, int *addrlen)
{
	socklen_t len = addrlen ? (socklen_t)*addrlen : 0;
	int fd = accept((int)s, addr, addrlen ? &len : NULL);

	if (fd < 0) {
		set_wsa_error();
		return WSA_INVALID_SOCKET;
	}

	if (addrlen)
		*addrlen = (int)len;

	return (uintptr_t)fd;
}

__attribute__((ms_abi))
static int ws_connect(uintptr_t s, const struct sockaddr *name, int namelen)
{
	if (connect((int)s, name, (socklen_t)namelen) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

/* ============================================================
 * 데이터 전송
 * ============================================================ */

__attribute__((ms_abi))
static int ws_send(uintptr_t s, const char *buf, int len, int flags)
{
	ssize_t n = send((int)s, buf, (size_t)len, flags);

	if (n < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return (int)n;
}

__attribute__((ms_abi))
static int ws_recv(uintptr_t s, char *buf, int len, int flags)
{
	ssize_t n = recv((int)s, buf, (size_t)len, flags);

	if (n < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return (int)n;
}

__attribute__((ms_abi))
static int ws_sendto(uintptr_t s, const char *buf, int len, int flags,
		     const struct sockaddr *to, int tolen)
{
	ssize_t n = sendto((int)s, buf, (size_t)len, flags,
			   to, (socklen_t)tolen);
	if (n < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return (int)n;
}

__attribute__((ms_abi))
static int ws_recvfrom(uintptr_t s, char *buf, int len, int flags,
		       struct sockaddr *from, int *fromlen)
{
	socklen_t flen = fromlen ? (socklen_t)*fromlen : 0;

	ssize_t n = recvfrom((int)s, buf, (size_t)len, flags,
			     from, fromlen ? &flen : NULL);
	if (n < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}

	if (fromlen)
		*fromlen = (int)flen;

	return (int)n;
}

/* ============================================================
 * 멀티플렉싱
 * ============================================================ */

__attribute__((ms_abi))
static int ws_select(int nfds, fd_set *readfds, fd_set *writefds,
		     fd_set *exceptfds, struct timeval *timeout)
{
	int ret = select(nfds, readfds, writefds, exceptfds, timeout);

	if (ret < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return ret;
}

__attribute__((ms_abi))
static int ws_ioctlsocket(uintptr_t s, long cmd, unsigned long *argp)
{
	if (ioctl((int)s, cmd, argp) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

/* ============================================================
 * 소켓 옵션
 * ============================================================ */

/* Windows → Linux 소켓 옵션 상수 변환 */
static int translate_sol_level(int winsock_level)
{
	switch (winsock_level) {
	case 0xFFFF: return SOL_SOCKET;          /* Windows SOL_SOCKET */
	case 6:      return IPPROTO_TCP;         /* Windows IPPROTO_TCP */
	default:     return winsock_level;
	}
}

static int translate_so_optname(int winsock_level, int winsock_opt)
{
	if (winsock_level == 0xFFFF) { /* SOL_SOCKET */
		switch (winsock_opt) {
		case 0x0004: return SO_REUSEADDR;    /* Windows SO_REUSEADDR */
		case 0x0020: return SO_BROADCAST;    /* Windows SO_BROADCAST */
		case 0x0080: return SO_KEEPALIVE;    /* Windows SO_KEEPALIVE */
		case 0x1005: return SO_RCVBUF;       /* Windows SO_RCVBUF */
		case 0x1001: return SO_SNDBUF;       /* Windows SO_SNDBUF */
		case 0x1006: return SO_RCVTIMEO;     /* Windows SO_RCVTIMEO */
		case 0x1002: return SO_SNDTIMEO;     /* Windows SO_SNDTIMEO */
		default:     return winsock_opt;
		}
	}
	if (winsock_level == 6) { /* IPPROTO_TCP */
		switch (winsock_opt) {
		case 0x0001: return TCP_NODELAY;     /* Windows TCP_NODELAY */
		default:     return winsock_opt;
		}
	}
	return winsock_opt;
}

__attribute__((ms_abi))
static int ws_setsockopt(uintptr_t s, int level, int optname,
			 const char *optval, int optlen)
{
	int lx_level = translate_sol_level(level);
	int lx_opt = translate_so_optname(level, optname);

	if (setsockopt((int)s, lx_level, lx_opt, optval,
		       (socklen_t)optlen) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

__attribute__((ms_abi))
static int ws_getsockopt(uintptr_t s, int level, int optname,
			 char *optval, int *optlen)
{
	int lx_level = translate_sol_level(level);
	int lx_opt = translate_so_optname(level, optname);
	socklen_t olen = optlen ? (socklen_t)*optlen : 0;

	if (getsockopt((int)s, lx_level, lx_opt, optval,
		       optlen ? &olen : NULL) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}

	if (optlen)
		*optlen = (int)olen;
	return 0;
}

/* ============================================================
 * 이름 해석
 * ============================================================ */

__attribute__((ms_abi))
static int ws_getaddrinfo(const char *pNodeName, const char *pServiceName,
			  const struct addrinfo *pHints,
			  struct addrinfo **ppResult)
{
	int ret = getaddrinfo(pNodeName, pServiceName, pHints, ppResult);

	if (ret != 0)
		last_wsa_error = WSAEINVAL;
	return ret;
}

__attribute__((ms_abi))
static void ws_freeaddrinfo(struct addrinfo *pAddrInfo)
{
	freeaddrinfo(pAddrInfo);
}

__attribute__((ms_abi))
static int ws_gethostname(char *name, int namelen)
{
	if (gethostname(name, (size_t)namelen) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

__attribute__((ms_abi))
static struct hostent *ws_gethostbyname(const char *name)
{
	struct hostent *he = gethostbyname(name);

	if (!he)
		last_wsa_error = WSAEINVAL;
	return he;
}

/* ============================================================
 * 바이트 오더 / 주소 변환
 * ============================================================ */

__attribute__((ms_abi))
static uint16_t ws_htons(uint16_t hostshort)
{
	return htons(hostshort);
}

__attribute__((ms_abi))
static uint32_t ws_htonl(uint32_t hostlong)
{
	return htonl(hostlong);
}

__attribute__((ms_abi))
static uint16_t ws_ntohs(uint16_t netshort)
{
	return ntohs(netshort);
}

__attribute__((ms_abi))
static uint32_t ws_ntohl(uint32_t netlong)
{
	return ntohl(netlong);
}

__attribute__((ms_abi))
static uint32_t ws_inet_addr(const char *cp)
{
	return inet_addr(cp);
}

__attribute__((ms_abi))
static char *ws_inet_ntoa(struct in_addr in)
{
	return inet_ntoa(in);
}

/* ============================================================
 * getpeername / getsockname
 * ============================================================ */

__attribute__((ms_abi))
static int ws_getpeername(uintptr_t s, struct sockaddr *name, int *namelen)
{
	socklen_t len = namelen ? (socklen_t)*namelen : 0;

	if (getpeername((int)s, name, namelen ? &len : NULL) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}

	if (namelen)
		*namelen = (int)len;
	return 0;
}

__attribute__((ms_abi))
static int ws_getsockname(uintptr_t s, struct sockaddr *name, int *namelen)
{
	socklen_t len = namelen ? (socklen_t)*namelen : 0;

	if (getsockname((int)s, name, namelen ? &len : NULL) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}

	if (namelen)
		*namelen = (int)len;
	return 0;
}

/* ============================================================
 * shutdown
 * ============================================================ */

__attribute__((ms_abi))
static int ws_shutdown(uintptr_t s, int how)
{
	if (shutdown((int)s, how) < 0) {
		set_wsa_error();
		return WSA_SOCKET_ERROR;
	}
	return 0;
}

/* ============================================================
 * ws2_32 스텁 테이블
 * ============================================================ */

struct stub_entry ws2_32_stub_table[] = {
	/* 초기화 */
	{ "ws2_32.dll", "WSAStartup",      (void *)ws_WSAStartup },
	{ "ws2_32.dll", "WSACleanup",      (void *)ws_WSACleanup },
	{ "ws2_32.dll", "WSAGetLastError",  (void *)ws_WSAGetLastError },
	{ "ws2_32.dll", "WSASetLastError",  (void *)ws_WSASetLastError },

	/* 소켓 생성/해제 */
	{ "ws2_32.dll", "socket",           (void *)ws_socket },
	{ "ws2_32.dll", "closesocket",      (void *)ws_closesocket },

	/* 연결 */
	{ "ws2_32.dll", "bind",             (void *)ws_bind },
	{ "ws2_32.dll", "listen",           (void *)ws_listen },
	{ "ws2_32.dll", "accept",           (void *)ws_accept },
	{ "ws2_32.dll", "connect",          (void *)ws_connect },

	/* 데이터 전송 */
	{ "ws2_32.dll", "send",             (void *)ws_send },
	{ "ws2_32.dll", "recv",             (void *)ws_recv },
	{ "ws2_32.dll", "sendto",           (void *)ws_sendto },
	{ "ws2_32.dll", "recvfrom",         (void *)ws_recvfrom },

	/* 멀티플렉싱 */
	{ "ws2_32.dll", "select",           (void *)ws_select },
	{ "ws2_32.dll", "ioctlsocket",      (void *)ws_ioctlsocket },

	/* 소켓 옵션 */
	{ "ws2_32.dll", "setsockopt",       (void *)ws_setsockopt },
	{ "ws2_32.dll", "getsockopt",       (void *)ws_getsockopt },

	/* 이름 해석 */
	{ "ws2_32.dll", "getaddrinfo",      (void *)ws_getaddrinfo },
	{ "ws2_32.dll", "freeaddrinfo",     (void *)ws_freeaddrinfo },
	{ "ws2_32.dll", "gethostname",      (void *)ws_gethostname },
	{ "ws2_32.dll", "gethostbyname",    (void *)ws_gethostbyname },

	/* 바이트 오더 */
	{ "ws2_32.dll", "htons",            (void *)ws_htons },
	{ "ws2_32.dll", "htonl",            (void *)ws_htonl },
	{ "ws2_32.dll", "ntohs",            (void *)ws_ntohs },
	{ "ws2_32.dll", "ntohl",            (void *)ws_ntohl },
	{ "ws2_32.dll", "inet_addr",        (void *)ws_inet_addr },
	{ "ws2_32.dll", "inet_ntoa",        (void *)ws_inet_ntoa },

	/* 기타 */
	{ "ws2_32.dll", "getpeername",      (void *)ws_getpeername },
	{ "ws2_32.dll", "getsockname",      (void *)ws_getsockname },
	{ "ws2_32.dll", "shutdown",         (void *)ws_shutdown },

	{ NULL, NULL, NULL }
};
