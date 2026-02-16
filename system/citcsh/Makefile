# citcsh — CITC OS Custom Shell
# ==============================
#
# 커스텀 UNIX 쉘.
# 명령어 파싱, 파이프, 리다이렉션, 환경변수, 시그널 처리를 구현.
#
# 빌드: make
# 정리: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/citcsh

SRCS = $(SRC_DIR)/citcsh.c

all: $(TARGET)

$(TARGET): $(SRCS) | $(BUILD_DIR)
	@echo "  CC    citcsh"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)
	@echo "  OK    $(TARGET)"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
