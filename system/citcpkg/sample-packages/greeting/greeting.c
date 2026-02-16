/*
 * greeting.c - CITC OS 의존성 테스트 패키지
 *
 * 이 패키지는 hello 패키지에 의존합니다 (PKGINFO의 depends=hello).
 * citcpkg install greeting을 실행하면:
 *   1. greeting이 hello에 의존함을 발견
 *   2. hello를 먼저 다운로드 & 설치
 *   3. greeting을 다운로드 & 설치
 *
 * 의존성 테스트:
 *   citcpkg install greeting    → hello 자동 설치 후 greeting 설치
 *   citcpkg remove hello        → greeting은 남아있지만 hello만 제거
 */

#include <stdio.h>

int main(void)
{
	printf("===========================\n");
	printf("  CITC OS에 오신 것을 환영합니다!\n");
	printf("===========================\n");
	printf("\n");
	printf("이 프로그램은 hello 패키지에 의존합니다.\n");
	printf("citcpkg가 의존성을 자동으로 해결했습니다.\n");
	printf("\n");
	printf("사용해보기:\n");
	printf("  hello       - Hello World 출력\n");
	printf("  greeting    - 이 환영 메시지\n");
	printf("  citcpkg list - 설치된 패키지 확인\n");
	return 0;
}
