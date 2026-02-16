# citc-ipc - CITC OS IPC Message Bus
# ====================================
#
# D-Bus의 교육적 단순화 구현.
# 프로세스 간 통신(IPC) 데몬.
#
# 사용법:
#   make        # 빌드
#   make clean  # 정리

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build

TARGET = $(BUILD_DIR)/citc-ipc

all: $(TARGET)

$(TARGET): $(SRC_DIR)/ipc_daemon.c $(SRC_DIR)/ipc_proto.h | $(BUILD_DIR)
	@echo "  CC    citc-ipc"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/ipc_daemon.c

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
