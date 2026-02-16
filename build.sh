#!/bin/bash
# CITC OS - 전체 빌드 스크립트
# ================================
#
# 이 스크립트 하나로 OS 이미지를 처음부터 끝까지 빌드합니다.
#
# 빌드 순서:
#   1. citcinit 컴파일 (C → 바이너리)
#   2. initramfs 생성 (citcinit + busybox → cpio 아카이브)
#   3. ISO 이미지 생성 (커널 + initramfs + GRUB → 부팅 가능 ISO)
#
# 사용법:
#   sudo bash build.sh          # 전체 빌드
#   sudo bash build.sh --quick  # initramfs + ISO만 (citcinit 이미 빌드됨)
#   sudo bash build.sh --clean  # 빌드 결과물 전부 삭제
#
# 전체 흐름 도식:
#
#   main.c ──(gcc)──→ citcinit
#                         │
#                         ▼
#   citcinit + busybox ──(cpio+gzip)──→ initramfs.cpio.gz
#                                            │
#                                            ▼
#   vmlinuz + initramfs.cpio.gz ──(grub-mkrescue)──→ citcos.iso
#                                                        │
#                                                        ▼
#                                                    QEMU 또는 USB

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# 색상
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

header() {
    echo ""
    echo -e "${BOLD}${BLUE}═══════════════════════════════════════${NC}"
    echo -e "${BOLD}${BLUE}  $1${NC}"
    echo -e "${BOLD}${BLUE}═══════════════════════════════════════${NC}"
    echo ""
}

# 클린 모드
if [ "$1" = "--clean" ]; then
    header "빌드 결과물 삭제"
    rm -rf "${BUILD_DIR}"
    make -C "${PROJECT_ROOT}/system/citcinit" clean 2>/dev/null || true
    echo -e "${GREEN}정리 완료${NC}"
    exit 0
fi

header "CITC OS 빌드 시작"

START_TIME=$(date +%s)

# ============================================
# Step 1: citcinit 빌드
# ============================================
if [ "$1" != "--quick" ]; then
    header "Step 1/3: citcinit 빌드"
    cd "${PROJECT_ROOT}/system/citcinit"
    make clean
    make
    cd "${PROJECT_ROOT}"
else
    echo "  [건너뜀] citcinit 빌드 (--quick 모드)"
fi

# ============================================
# Step 2: initramfs 빌드
# ============================================
header "Step 2/3: initramfs 빌드"
bash "${PROJECT_ROOT}/tools/mkrootfs/build-initramfs.sh"

# ============================================
# Step 3: ISO 빌드
# ============================================
header "Step 3/3: ISO 이미지 빌드"
bash "${PROJECT_ROOT}/tools/mkiso/build-iso.sh"

# ============================================
# 완료
# ============================================
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

header "빌드 완료!"

echo "  빌드 시간: ${ELAPSED}초"
echo ""
echo "  결과물:"
echo "    citcinit:  ${PROJECT_ROOT}/system/citcinit/build/citcinit"
echo "    initramfs: ${BUILD_DIR}/initramfs.cpio.gz"
echo "    ISO:       ${BUILD_DIR}/citcos.iso"
echo ""

if [ -f "${BUILD_DIR}/citcos.iso" ]; then
    echo "  ISO 크기: $(du -h "${BUILD_DIR}/citcos.iso" | cut -f1)"
    echo ""
    echo "  테스트하려면:"
    echo "    bash tools/run-qemu.sh"
    echo ""
    echo "  또는 ISO로 직접 QEMU 실행:"
    echo "    qemu-system-x86_64 -cdrom build/citcos.iso -m 1G"
fi
