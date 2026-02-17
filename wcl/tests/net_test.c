/*
 * net_test.c — CITC OS WCL Winsock2 네트워크 테스트
 * ===================================================
 *
 * Class 51에서 구현한 ws2_32.dll API를 테스트:
 *   WSAStartup, socket, bind, listen, accept, connect,
 *   send, recv, sendto, recvfrom, getaddrinfo, closesocket
 *
 * 테스트 방법: 로컬 루프백 (127.0.0.1)
 *   서버 스레드: bind + listen + accept + recv + send(echo)
 *   메인 스레드: connect + send + recv + 확인
 *
 * 빌드:
 *   x86_64-w64-mingw32-gcc -nostdlib -o net_test.exe net_test.c \
 *       -lkernel32 -lws2_32 -Wl,-e,_start
 *
 * 실행:
 *   citcrun net_test.exe
 */

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
typedef const char *LPCSTR;
typedef const void *LPCVOID;
typedef unsigned long *LPDWORD;
typedef void *LPOVERLAPPED;
typedef unsigned long long ULONG_PTR;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define NULL ((void *)0)
#define INFINITE ((DWORD)-1)

/* Winsock 상수 */
#define AF_INET       2
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
#define INADDR_LOOPBACK 0x7F000001

typedef ULONG_PTR SOCKET;
#define INVALID_SOCKET ((SOCKET)(~0))
#define SOCKET_ERROR   (-1)

/* sockaddr_in (Windows 레이아웃 = POSIX 동일) */
typedef struct {
	short sin_family;
	unsigned short sin_port;
	unsigned int sin_addr;
	char sin_zero[8];
} SOCKADDR_IN;

typedef struct {
	unsigned short sa_family;
	char sa_data[14];
} SOCKADDR;

/* WSADATA */
typedef struct {
	unsigned short wVersion;
	unsigned short wHighVersion;
	char szDescription[257];
	char szSystemStatus[129];
	unsigned short iMaxSockets;
	unsigned short iMaxUdpDg;
	char *lpVendorInfo;
} WSADATA;

/* addrinfo */
typedef struct addrinfo_s {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	unsigned long long ai_addrlen;
	char *ai_canonname;
	SOCKADDR *ai_addr;
	struct addrinfo_s *ai_next;
} ADDRINFOA;

/* kernel32.dll */
__declspec(dllimport) void __stdcall ExitProcess(UINT);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, LPCVOID, DWORD,
					       LPDWORD, LPOVERLAPPED);
__declspec(dllimport) HANDLE __stdcall CreateThread(
	void *sa, ULONG_PTR stack, DWORD (__stdcall *fn)(void *),
	void *param, DWORD flags, DWORD *tid);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) void __stdcall Sleep(DWORD);

/* ws2_32.dll */
__declspec(dllimport) int __stdcall WSAStartup(unsigned short, WSADATA *);
__declspec(dllimport) int __stdcall WSACleanup(void);
__declspec(dllimport) int __stdcall WSAGetLastError(void);
__declspec(dllimport) SOCKET __stdcall socket(int, int, int);
__declspec(dllimport) int __stdcall closesocket(SOCKET);
__declspec(dllimport) int __stdcall bind(SOCKET, const SOCKADDR *, int);
__declspec(dllimport) int __stdcall listen(SOCKET, int);
__declspec(dllimport) SOCKET __stdcall accept(SOCKET, SOCKADDR *, int *);
__declspec(dllimport) int __stdcall connect(SOCKET, const SOCKADDR *, int);
__declspec(dllimport) int __stdcall send(SOCKET, const char *, int, int);
__declspec(dllimport) int __stdcall recv(SOCKET, char *, int, int);
__declspec(dllimport) int __stdcall sendto(SOCKET, const char *, int, int,
					   const SOCKADDR *, int);
__declspec(dllimport) int __stdcall recvfrom(SOCKET, char *, int, int,
					     SOCKADDR *, int *);
__declspec(dllimport) unsigned short __stdcall htons(unsigned short);
__declspec(dllimport) unsigned int __stdcall htonl(unsigned int);
__declspec(dllimport) unsigned short __stdcall ntohs(unsigned short);
__declspec(dllimport) int __stdcall getaddrinfo(const char *, const char *,
						const ADDRINFOA *,
						ADDRINFOA **);
__declspec(dllimport) void __stdcall freeaddrinfo(ADDRINFOA *);
__declspec(dllimport) int __stdcall gethostname(char *, int);

/* === 유틸리티 === */

static void print(HANDLE out, const char *s)
{
	DWORD written;
	DWORD len = 0;

	while (s[len])
		len++;
	WriteFile(out, s, len, &written, NULL);
}

static void print_num(HANDLE out, DWORD num)
{
	char buf[16];
	int i = 0;

	if (num == 0) {
		buf[i++] = '0';
	} else {
		while (num > 0) {
			buf[i++] = '0' + (char)(num % 10);
			num /= 10;
		}
	}

	DWORD written;
	char rev[16];

	for (int j = 0; j < i; j++)
		rev[j] = buf[i - 1 - j];
	WriteFile(out, rev, (DWORD)i, &written, NULL);
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

/* 서버 포트 (랜덤하게 높은 포트 사용) */
#define TEST_TCP_PORT 19876
#define TEST_UDP_PORT 19877

/* TCP 에코 서버 상태 */
static volatile int server_ready = 0;

/* === TCP 에코 서버 스레드 === */

static DWORD __stdcall tcp_server_thread(void *param)
{
	(void)param;

	SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);

	if (srv == INVALID_SOCKET)
		return 1;

	/* SO_REUSEADDR */
	int opt = 1;

	/* 직접 setsockopt 선언 안 했으므로 바인드 재시도 */
	SOCKADDR_IN addr;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_TCP_PORT);
	addr.sin_addr = htonl(INADDR_LOOPBACK);
	for (int i = 0; i < 8; i++)
		addr.sin_zero[i] = 0;

	if (bind(srv, (SOCKADDR *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		closesocket(srv);
		return 2;
	}

	if (listen(srv, 1) == SOCKET_ERROR) {
		closesocket(srv);
		return 3;
	}

	/* 서버 준비 완료 시그널 */
	server_ready = 1;

	/* 클라이언트 접속 대기 */
	SOCKET client = accept(srv, NULL, NULL);

	if (client == INVALID_SOCKET) {
		closesocket(srv);
		return 4;
	}

	/* 에코: recv → send */
	char buf[256];
	int n = recv(client, buf, sizeof(buf), 0);

	if (n > 0)
		send(client, buf, n, 0);

	closesocket(client);
	closesocket(srv);
	(void)opt;
	return 0;
}

/* === 테스트 시작 === */

void _start(void)
{
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	int pass = 0;
	int fail = 0;

	print(out, "=== Winsock2 Network Test (Class 51) ===\n\n");

	/* 1. WSAStartup */
	print(out, "[1] WSAStartup(2.2)... ");
	{
		WSADATA wsa;
		int ret = WSAStartup(0x0202, &wsa);

		if (ret == 0 && wsa.wVersion == 0x0202) {
			print(out, "OK (v");
			print_num(out, wsa.wVersion >> 8);
			print(out, ".");
			print_num(out, wsa.wVersion & 0xFF);
			print(out, ")\n");
			pass++;
		} else {
			print(out, "FAIL (ret=");
			print_num(out, (DWORD)ret);
			print(out, ")\n");
			fail++;
		}
	}

	/* 2. socket(TCP) */
	print(out, "[2] socket(AF_INET, SOCK_STREAM)... ");
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);

		if (s != INVALID_SOCKET) {
			print(out, "OK\n");
			pass++;
			closesocket(s);
		} else {
			print(out, "FAIL\n");
			fail++;
		}
	}

	/* 3-4. TCP 에코 테스트 (서버 스레드 + 클라이언트) */
	print(out, "[3] TCP server bind+listen... ");
	{
		HANDLE hThread = CreateThread(NULL, 0, tcp_server_thread,
					      NULL, 0, NULL);
		if (!hThread) {
			print(out, "FAIL (CreateThread)\n");
			fail++;
			fail++; /* [4]도 스킵 */
		} else {
			/* 서버 준비 대기 */
			int wait = 0;

			while (!server_ready && wait < 100) {
				Sleep(10);
				wait++;
			}

			if (server_ready) {
				print(out, "OK\n");
				pass++;
			} else {
				print(out, "FAIL (timeout)\n");
				fail++;
			}

			/* 4. 클라이언트: connect + send + recv */
			print(out, "[4] TCP echo (send/recv)... ");
			SOCKET cli = socket(AF_INET, SOCK_STREAM, 0);

			if (cli == INVALID_SOCKET) {
				print(out, "FAIL (socket)\n");
				fail++;
			} else {
				SOCKADDR_IN saddr;

				saddr.sin_family = AF_INET;
				saddr.sin_port = htons(TEST_TCP_PORT);
				saddr.sin_addr = htonl(INADDR_LOOPBACK);
				for (int i = 0; i < 8; i++)
					saddr.sin_zero[i] = 0;

				if (connect(cli, (SOCKADDR *)&saddr,
					    sizeof(saddr)) == 0) {
					const char *msg = "HELLO";
					int s_ret = send(cli, msg, 5, 0);
					char rbuf[32];

					for (int i = 0; i < 32; i++)
						rbuf[i] = 0;

					int r_ret = recv(cli, rbuf, 32, 0);

					if (s_ret == 5 && r_ret == 5 &&
					    str_eq(rbuf, "HELLO")) {
						print(out, "OK (\"");
						print(out, rbuf);
						print(out, "\")\n");
						pass++;
					} else {
						print(out, "FAIL (s=");
						print_num(out, (DWORD)s_ret);
						print(out, " r=");
						print_num(out, (DWORD)r_ret);
						print(out, ")\n");
						fail++;
					}
				} else {
					print(out, "FAIL (connect err=");
					print_num(out, (DWORD)WSAGetLastError());
					print(out, ")\n");
					fail++;
				}

				closesocket(cli);
			}

			WaitForSingleObject(hThread, INFINITE);
		}
	}

	/* 5. UDP sendto/recvfrom */
	print(out, "[5] UDP sendto/recvfrom... ");
	{
		SOCKET s1 = socket(AF_INET, SOCK_DGRAM, 0);
		SOCKET s2 = socket(AF_INET, SOCK_DGRAM, 0);

		if (s1 == INVALID_SOCKET || s2 == INVALID_SOCKET) {
			print(out, "FAIL (socket)\n");
			fail++;
		} else {
			SOCKADDR_IN bind_addr;

			bind_addr.sin_family = AF_INET;
			bind_addr.sin_port = htons(TEST_UDP_PORT);
			bind_addr.sin_addr = htonl(INADDR_LOOPBACK);
			for (int i = 0; i < 8; i++)
				bind_addr.sin_zero[i] = 0;

			if (bind(s2, (SOCKADDR *)&bind_addr,
				 sizeof(bind_addr)) == 0) {
				const char *msg = "UDP!";

				sendto(s1, msg, 4, 0,
				       (SOCKADDR *)&bind_addr,
				       sizeof(bind_addr));

				char rbuf[32];

				for (int i = 0; i < 32; i++)
					rbuf[i] = 0;

				int n = recvfrom(s2, rbuf, 32, 0, NULL, NULL);

				if (n == 4 && str_eq(rbuf, "UDP!")) {
					print(out, "OK (\"");
					print(out, rbuf);
					print(out, "\")\n");
					pass++;
				} else {
					print(out, "FAIL (n=");
					print_num(out, (DWORD)n);
					print(out, ")\n");
					fail++;
				}
			} else {
				print(out, "FAIL (bind)\n");
				fail++;
			}

			closesocket(s1);
			closesocket(s2);
		}
	}

	/* 6. getaddrinfo("localhost") */
	print(out, "[6] getaddrinfo(\"localhost\")... ");
	{
		ADDRINFOA hints;
		ADDRINFOA *result = NULL;

		for (int i = 0; i < (int)sizeof(hints); i++)
			((char *)&hints)[i] = 0;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;

		int ret = getaddrinfo("localhost", NULL, &hints, &result);

		if (ret == 0 && result) {
			print(out, "OK\n");
			pass++;
			freeaddrinfo(result);
		} else {
			print(out, "FAIL (ret=");
			print_num(out, (DWORD)ret);
			print(out, ")\n");
			fail++;
		}
	}

	/* 7. gethostname */
	print(out, "[7] gethostname... ");
	{
		char hostname[128];

		for (int i = 0; i < 128; i++)
			hostname[i] = 0;

		int ret = gethostname(hostname, sizeof(hostname));

		if (ret == 0 && hostname[0] != '\0') {
			print(out, "OK (\"");
			print(out, hostname);
			print(out, "\")\n");
			pass++;
		} else {
			print(out, "FAIL\n");
			fail++;
		}
	}

	/* 8. WSACleanup */
	print(out, "[8] WSACleanup... ");
	{
		int ret = WSACleanup();

		if (ret == 0) {
			print(out, "OK\n");
			pass++;
		} else {
			print(out, "FAIL\n");
			fail++;
		}
	}

	/* 결과 요약 */
	print(out, "\n=== Result: ");
	print_num(out, (DWORD)pass);
	print(out, " passed, ");
	print_num(out, (DWORD)fail);
	print(out, " failed ===\n");

	ExitProcess(fail > 0 ? 1 : 0);
}
