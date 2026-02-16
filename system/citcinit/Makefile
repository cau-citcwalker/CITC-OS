# citcinit - CITC OS Init System
# ================================
#
# Makefile 설명:
#   Makefile은 빌드 자동화 도구입니다.
#   "make" 명령을 실행하면 정의된 규칙에 따라 컴파일합니다.
#
# 사용법 (WSL에서):
#   make          # 빌드 (citcinit + shutdown)
#   make clean    # 빌드 결과물 삭제
#   make test     # 로컬에서 테스트 (PID 1이 아니므로 경고 표시)
#
# 빌드 결과:
#   build/citcinit - PID 1 init 시스템
#   build/shutdown - 시스템 종료/재부팅 명령어

# 컴파일러 설정
CC = gcc

# 컴파일 플래그 설명:
#   -Wall:     모든 경고 활성화 (잠재적 버그를 찾아줌)
#   -Wextra:   추가 경고 활성화
#   -Werror:   경고를 에러로 취급 (경고 있으면 빌드 실패)
#   -std=gnu11: C11 표준 + GNU 확장 (Linux 시스템 프로그래밍에 필요)
#   -O2:       최적화 레벨 2 (적당한 최적화)
#   -static:   정적 링크!!! 매우 중요!!!
#              initramfs에는 공유 라이브러리가 없으므로
#              모든 코드를 실행파일에 포함시켜야 합니다.
#              이거 없으면 "not found" 에러로 실행 불가.
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

# 소스/빌드 디렉토리
SRC_DIR = src
BUILD_DIR = build

# ============================================
# 타겟 정의
# ============================================
# citcinit: init 시스템 (main.c + service.c + config.c)
# shutdown: 종료 명령어 (shutdown.c 단독)
#
# shutdown.c는 citcinit과 별개의 프로그램이므로
# 소스 파일을 명시적으로 분리합니다.

CITCINIT_TARGET = $(BUILD_DIR)/citcinit
SHUTDOWN_TARGET = $(BUILD_DIR)/shutdown

# citcinit 소스 (shutdown.c 제외)
CITCINIT_SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/service.c $(SRC_DIR)/config.c $(SRC_DIR)/socket_activation.c
CITCINIT_OBJS = $(CITCINIT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# shutdown 소스 (단독)
SHUTDOWN_SRCS = $(SRC_DIR)/shutdown.c
SHUTDOWN_OBJS = $(SHUTDOWN_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# 헤더 파일 (어떤 .c든 헤더가 바뀌면 재컴파일)
HEADERS = $(wildcard $(SRC_DIR)/*.h)

# 기본 타겟: 둘 다 빌드
all: $(CITCINIT_TARGET) $(SHUTDOWN_TARGET)

# ============================================
# citcinit 링크
# ============================================
$(CITCINIT_TARGET): $(CITCINIT_OBJS)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -o $@ $^
	@echo ""
	@echo "빌드 완료: $@"
	@echo "파일 크기: $$(du -h $@ | cut -f1)"

# ============================================
# shutdown 링크
# ============================================
$(SHUTDOWN_TARGET): $(SHUTDOWN_OBJS)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -o $@ $^
	@echo ""
	@echo "빌드 완료: $@"
	@echo "파일 크기: $$(du -h $@ | cut -f1)"

# ============================================
# 컴파일: .c → .o
# ============================================
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

# 빌드 디렉토리 생성
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# 정리
clean:
	@echo "  CLEAN"
	@rm -rf $(BUILD_DIR)

# 테스트 (PID 1이 아니므로 경고가 뜨지만 기본 로직 확인 가능)
test: $(CITCINIT_TARGET)
	@echo "=== citcinit 테스트 (PID 1이 아님 - 일부 기능 제한) ==="
	@echo "Ctrl+C로 종료하세요"
	@$(CITCINIT_TARGET)

.PHONY: all clean test
