# CITC Audio Server — 빌드
# ========================
#
# 빌드: make
# 정리: make clean
#
# 결과물:
#   build/citcaudio    — 오디오 서버 데몬
#   build/audio_test   — 440Hz 사인파 테스트
#   build/beep         — 네이티브 비프 도구

CC = gcc
CFLAGS = -Wall -Wextra -static
LDFLAGS = -lm

BUILD_DIR = build

TARGETS = $(BUILD_DIR)/citcaudio \
          $(BUILD_DIR)/audio_test \
          $(BUILD_DIR)/beep

all: $(TARGETS)

$(BUILD_DIR)/citcaudio: citcaudio.c citcaudio_proto.h | $(BUILD_DIR)
	@echo "  CC    citcaudio"
	@$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  OK    $@"

$(BUILD_DIR)/audio_test: audio_test.c citcaudio_client.h citcaudio_proto.h | $(BUILD_DIR)
	@echo "  CC    audio_test"
	@$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  OK    $@"

$(BUILD_DIR)/beep: beep.c citcaudio_client.h citcaudio_proto.h | $(BUILD_DIR)
	@echo "  CC    beep"
	@$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  OK    $@"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
