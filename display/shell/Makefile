# citcshell — CITC OS 데스크탑 셸
# ===================================
#
# 태스크바(패널) + 앱 런처.
# CDP 클라이언트로 컴포지터에 연결하여 화면 하단에 패널을 표시.
#
# 빌드: make
# 정리: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build
INCLUDE_DIR = ../protocol
TARGET = $(BUILD_DIR)/citcshell

# 의존하는 헤더 (변경 시 재빌드)
HEADERS = $(INCLUDE_DIR)/cdp_proto.h $(INCLUDE_DIR)/cdp_client.h \
	  ../fbdraw/src/font8x8.h

all: $(TARGET)

$(TARGET): $(SRC_DIR)/citcshell.c $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    citcshell"
	@$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $(LDFLAGS) -o $@ $(SRC_DIR)/citcshell.c
	@echo "  OK    $(TARGET)"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
