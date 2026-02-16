# citcrun — CITC OS Windows PE Loader
# =====================================
#
# Windows .exe 파일을 로드하고 실행하는 프로그램.
# WCL(Windows Compatibility Layer)의 첫 번째 컴포넌트.
#
# 정적 링크 — initramfs에서 독립 실행.
#
# 빌드: make
# 정리: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2
LDFLAGS = -static
LIBS = -lpthread -lm

SRC_DIR = .
BUILD_DIR = build
INCLUDE_DIR = ../../include
KERNEL32_DIR = ../dlls/kernel32
USER32_DIR = ../dlls/user32
GDI32_DIR = ../dlls/gdi32
DXGI_DIR = ../dlls/dxgi
D3D11_DIR = ../dlls/d3d11
NTEMU_DIR = ../ntemu
TARGET = $(BUILD_DIR)/citcrun

SRCS = $(SRC_DIR)/citcrun.c \
       $(KERNEL32_DIR)/kernel32.c \
       $(USER32_DIR)/user32.c \
       $(GDI32_DIR)/gdi32.c \
       $(DXGI_DIR)/dxgi.c \
       $(D3D11_DIR)/d3d11.c \
       $(NTEMU_DIR)/ntdll.c \
       $(NTEMU_DIR)/object_manager.c \
       $(NTEMU_DIR)/registry.c
HEADERS = $(INCLUDE_DIR)/pe.h $(INCLUDE_DIR)/win32.h \
          $(INCLUDE_DIR)/d3d11_types.h \
          $(INCLUDE_DIR)/stub_entry.h \
          $(KERNEL32_DIR)/kernel32.h \
          $(USER32_DIR)/user32.h \
          $(GDI32_DIR)/gdi32.h \
          $(DXGI_DIR)/dxgi.h \
          $(D3D11_DIR)/d3d11.h \
          $(NTEMU_DIR)/ntdll.h $(NTEMU_DIR)/object_manager.h \
          $(NTEMU_DIR)/registry.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS) | $(BUILD_DIR)
	@echo "  CC    citcrun"
	@$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)
	@echo "  OK    $(TARGET)"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
