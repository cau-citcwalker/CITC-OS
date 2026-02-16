# compositor - CITC OS 윈도우 관리자 + 컴포지터
# ================================================
#
# DRM/KMS + evdev 입력 + 윈도우 관리를 하나로 합친 프로그램.
# 정적 링크 (initramfs에서 실행 - 공유 라이브러리 없음).
#
# 의존성:
#   drm/drm.h, drm/drm_mode.h → linux-libc-dev (기본 설치)
#   linux/input.h → linux-libc-dev
#   libdrm, libinput 별도 설치 불필요!

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/compositor

SOURCES = $(wildcard $(SRC_DIR)/*.c)
HEADERS = $(wildcard $(SRC_DIR)/*.h)

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    compositor"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)
	@echo ""
	@echo "빌드 완료: $@"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@echo "  CLEAN"
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
