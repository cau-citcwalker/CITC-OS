# CITC Display Protocol — 클라이언트 빌드
# ==========================================
#
# cdp_demo: CDP 프로토콜 데모 클라이언트
#
# 이 프로그램은 컴포지터와 별도의 프로세스로 실행됩니다.
# 정적 링킹(-static)으로 initramfs에서 독립 실행 가능.
#
# 빌드: make
# 정리: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = .
BUILD_DIR = build
TARGET = $(BUILD_DIR)/cdp_demo

# 소스와 헤더 의존성
SRCS = $(SRC_DIR)/cdp_demo.c
HEADERS = $(SRC_DIR)/cdp_proto.h $(SRC_DIR)/cdp_client.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    cdp_demo"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)
	@echo "  OK    $(TARGET)"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
