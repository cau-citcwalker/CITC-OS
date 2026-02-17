# WCL 테스트 프로그램 — MinGW 크로스 컴파일
# =============================================
#
# Linux에서 Windows .exe를 빌드합니다.
#
# 전제조건:
#   sudo apt install gcc-mingw-w64-x86-64
#
# 빌드: make
# 정리: make clean

CROSS_CC = x86_64-w64-mingw32-gcc
CROSS_CFLAGS = -Wall -Wextra -nostdlib
CROSS_LDFLAGS = -lkernel32 -Wl,-e,_start

BUILD_DIR = build
TARGETS = $(BUILD_DIR)/hello.exe \
          $(BUILD_DIR)/file_test.exe \
          $(BUILD_DIR)/api_test.exe \
          $(BUILD_DIR)/reg_test.exe \
          $(BUILD_DIR)/gui_test.exe \
          $(BUILD_DIR)/dx_test.exe \
          $(BUILD_DIR)/cube_test.exe \
          $(BUILD_DIR)/thread_test.exe \
          $(BUILD_DIR)/com_test.exe \
          $(BUILD_DIR)/net_test.exe \
          $(BUILD_DIR)/d3d12_test.exe \
          $(BUILD_DIR)/app_test.exe

all: $(TARGETS)

$(BUILD_DIR)/hello.exe: hello_win.c | $(BUILD_DIR)
	@echo "  MINGW hello.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS)
	@echo "  OK    $@"

$(BUILD_DIR)/file_test.exe: file_test.c | $(BUILD_DIR)
	@echo "  MINGW file_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS)
	@echo "  OK    $@"

$(BUILD_DIR)/api_test.exe: api_test.c | $(BUILD_DIR)
	@echo "  MINGW api_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS)
	@echo "  OK    $@"

$(BUILD_DIR)/reg_test.exe: reg_test.c | $(BUILD_DIR)
	@echo "  MINGW reg_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -ladvapi32
	@echo "  OK    $@"

$(BUILD_DIR)/gui_test.exe: gui_test.c | $(BUILD_DIR)
	@echo "  MINGW gui_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -luser32 -lgdi32
	@echo "  OK    $@"

$(BUILD_DIR)/dx_test.exe: dx_test.c | $(BUILD_DIR)
	@echo "  MINGW dx_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -luser32 -ld3d11 -ldxgi
	@echo "  OK    $@"

$(BUILD_DIR)/cube_test.exe: cube_test.c | $(BUILD_DIR)
	@echo "  MINGW cube_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -luser32 -ld3d11 -ldxgi -ldsound
	@echo "  OK    $@"

$(BUILD_DIR)/thread_test.exe: thread_test.c | $(BUILD_DIR)
	@echo "  MINGW thread_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS)
	@echo "  OK    $@"

$(BUILD_DIR)/com_test.exe: com_test.c | $(BUILD_DIR)
	@echo "  MINGW com_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -lole32
	@echo "  OK    $@"

$(BUILD_DIR)/net_test.exe: net_test.c | $(BUILD_DIR)
	@echo "  MINGW net_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -lws2_32
	@echo "  OK    $@"

$(BUILD_DIR)/d3d12_test.exe: d3d12_test.c | $(BUILD_DIR)
	@echo "  MINGW d3d12_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -ld3d12
	@echo "  OK    $@"

$(BUILD_DIR)/app_test.exe: app_test.c | $(BUILD_DIR)
	@echo "  MINGW app_test.exe"
	@$(CROSS_CC) $(CROSS_CFLAGS) -o $@ $< $(CROSS_LDFLAGS) -ladvapi32 -lws2_32 -lole32 -ld3d12
	@echo "  OK    $@"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
