# fbdraw - CITC OS Framebuffer Graphics Demo
# ============================================

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/fbdraw

SOURCES = $(wildcard $(SRC_DIR)/*.c)
HEADERS = $(wildcard $(SRC_DIR)/*.h)

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    fbdraw"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)
	@echo ""
	@echo "빌드 완료: $@"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@echo "  CLEAN"
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
