# citcrun — CITC OS Windows PE Loader
# =====================================
#
# Windows .exe 파일을 로드하고 실행하는 프로그램.
# WCL(Windows Compatibility Layer)의 첫 번째 컴포넌트.
#
# 정적 링크 — initramfs에서 독립 실행.
# VULKAN=1 이면 Vulkan GPU 백엔드 포함 (동적 링크).
#
# 빌드: make          (SW 래스터라이저만)
#       make VULKAN=1 (Vulkan GPU 백엔드 포함)
# 정리: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -O2

# Vulkan 조건부 빌드
ifdef VULKAN
CFLAGS += -DCITC_VULKAN_ENABLED
LDFLAGS =
LIBS = -ldl -lpthread -lm
else
LDFLAGS = -static
LIBS = -lpthread -lm
endif

SRC_DIR = .
BUILD_DIR = build
INCLUDE_DIR = ../../include
KERNEL32_DIR = ../dlls/kernel32
USER32_DIR = ../dlls/user32
GDI32_DIR = ../dlls/gdi32
DXGI_DIR = ../dlls/dxgi
D3D11_DIR = ../dlls/d3d11
DSOUND_DIR = ../dlls/dsound
XAUDIO2_DIR = ../dlls/xaudio2
XINPUT_DIR = ../dlls/xinput
OLE32_DIR = ../dlls/ole32
WS2_32_DIR = ../dlls/ws2_32
D3D12_DIR = ../dlls/d3d12
NTEMU_DIR = ../ntemu
TARGET = $(BUILD_DIR)/citcrun

SRCS = $(SRC_DIR)/citcrun.c \
       $(KERNEL32_DIR)/kernel32.c \
       $(USER32_DIR)/user32.c \
       $(GDI32_DIR)/gdi32.c \
       $(DXGI_DIR)/dxgi.c \
       $(D3D11_DIR)/d3d11.c \
       $(D3D11_DIR)/dxbc.c \
       $(D3D11_DIR)/spirv_emit.c \
       $(D3D11_DIR)/shader_cache.c \
       $(DSOUND_DIR)/dsound.c \
       $(XAUDIO2_DIR)/xaudio2.c \
       $(XINPUT_DIR)/xinput.c \
       $(OLE32_DIR)/ole32.c \
       $(WS2_32_DIR)/ws2_32.c \
       $(D3D12_DIR)/d3d12.c \
       $(NTEMU_DIR)/ntdll.c \
       $(NTEMU_DIR)/object_manager.c \
       $(NTEMU_DIR)/registry.c

# Vulkan 소스 (VULKAN=1일 때만)
ifdef VULKAN
SRCS += $(D3D11_DIR)/vk_backend.c \
        $(D3D11_DIR)/vk_pipeline.c
endif

HEADERS = $(INCLUDE_DIR)/pe.h $(INCLUDE_DIR)/win32.h \
          $(INCLUDE_DIR)/d3d11_types.h \
          $(INCLUDE_DIR)/stub_entry.h \
          $(KERNEL32_DIR)/kernel32.h \
          $(USER32_DIR)/user32.h \
          $(GDI32_DIR)/gdi32.h \
          $(DXGI_DIR)/dxgi.h \
          $(D3D11_DIR)/d3d11.h \
          $(D3D11_DIR)/dxbc.h \
          $(D3D11_DIR)/spirv_emit.h \
          $(D3D11_DIR)/shader_cache.h \
          $(D3D11_DIR)/vk_backend.h \
          $(D3D11_DIR)/vk_pipeline.h \
          $(DSOUND_DIR)/dsound.h \
          $(XAUDIO2_DIR)/xaudio2.h \
          $(XINPUT_DIR)/xinput.h \
          $(OLE32_DIR)/ole32.h \
          $(WS2_32_DIR)/ws2_32.h \
          $(D3D12_DIR)/d3d12.h \
          $(INCLUDE_DIR)/d3d12_types.h \
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
