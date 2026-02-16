# citcpkg - CITC OS Package Manager
# ====================================
#
# 사용법 (WSL에서):
#   make          # 빌드
#   make clean    # 빌드 결과물 삭제

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/citcpkg

SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
HEADERS = $(wildcard $(SRC_DIR)/*.h)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -o $@ $^
	@echo ""
	@echo "빌드 완료: $@"
	@echo "파일 크기: $$(du -h $@ | cut -f1)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@echo "  CLEAN"
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
