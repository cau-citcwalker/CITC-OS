/*
 * hello.c - CITC OS 테스트 패키지
 *
 * 이것은 패키지 매니저 테스트를 위한 간단한 프로그램입니다.
 * citcpkg install hello-1.0.cpkg 로 설치하면
 * /usr/bin/hello 에 설치됩니다.
 */

#include <stdio.h>

int main(void)
{
	printf("Hello from CITC OS!\n");
	printf("이 프로그램은 citcpkg로 설치되었습니다.\n");
	printf("제거: citcpkg remove hello\n");
	return 0;
}
