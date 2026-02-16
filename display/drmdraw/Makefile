# drmdraw - CITC OS DRM/KMS Graphics Demo
# ==========================================
#
# DRM 헤더 의존성:
#   drm/drm.h, drm/drm_mode.h → linux-libc-dev (기본 설치)
#   libdrm 별도 설치 불필요!

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/drmdraw

SOURCES = $(wildcard $(SRC_DIR)/*.c)
HEADERS = $(wildcard $(SRC_DIR)/*.h)

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    drmdraw"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)
	@echo ""
	@echo "빌드 완료: $@"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@echo "  CLEAN"
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
