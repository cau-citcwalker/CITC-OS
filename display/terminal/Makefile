# citcterm — CITC OS Terminal Emulator
# =====================================
#
# CDP 클라이언트 터미널 에뮬레이터.
# 컴포지터에 연결하여 그래픽 터미널 윈도우를 제공.
# PTY(pseudo-terminal)를 통해 쉘 프로세스와 양방향 통신.
#
# 빌드: make
# 정리: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/citcterm

SRCS = $(SRC_DIR)/citcterm.c
HEADERS = ../protocol/cdp_proto.h \
          ../protocol/cdp_client.h \
          ../fbdraw/src/font8x8.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    citcterm"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)
	@echo "  OK    $(TARGET)"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
