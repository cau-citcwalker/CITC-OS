#!/bin/bash
# CITC OS - 개발 환경 설치 스크립트
# WSL Ubuntu에서 실행: bash tools/setup-toolchain.sh
#
# 이 스크립트가 하는 일:
# 1. C 컴파일러(GCC) 설치 - citcinit, WCL 코드 빌드에 사용
# 2. 빌드 시스템(Make, Meson, Ninja) 설치 - 프로젝트 빌드 관리
# 3. QEMU 설치 - OS를 가상머신에서 테스트
# 4. 커널 빌드 도구 설치 - Linux 커널 빌드에 필요
# 5. ISO 생성 도구 설치 - 부팅 가능한 이미지 생성

set -e

echo "========================================="
echo "  CITC OS 개발 환경 설치"
echo "========================================="
echo ""

# 색상 정의
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

step() {
    echo -e "${GREEN}[STEP]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

# 1. 패키지 목록 업데이트
step "패키지 목록 업데이트 중..."
sudo apt-get update -qq

# 2. 필수 빌드 도구
step "C 컴파일러 및 빌드 도구 설치 중..."
sudo apt-get install -y \
    build-essential \
    gcc \
    g++ \
    make \
    nasm \
    cpio \
    gzip \
    bc \
    bison \
    flex \
    libelf-dev \
    libssl-dev \
    libncurses-dev

# 3. Meson + Ninja 빌드 시스템
step "Meson + Ninja 빌드 시스템 설치 중..."
sudo apt-get install -y \
    meson \
    ninja-build \
    pkg-config

# 4. QEMU 가상 머신
step "QEMU 설치 중 (OS 테스트용)..."
sudo apt-get install -y \
    qemu-system-x86 \
    qemu-utils

# 5. ISO 생성 도구
step "ISO 생성 도구 설치 중..."
sudo apt-get install -y \
    grub-pc-bin \
    grub-efi-amd64-bin \
    grub-common \
    xorriso \
    mtools

# 6. 기타 유용한 도구
step "추가 도구 설치 중..."
sudo apt-get install -y \
    git \
    curl \
    wget \
    strace \
    gdb \
    file

# 7. 설치 확인
echo ""
echo "========================================="
echo "  설치 확인"
echo "========================================="
echo ""

check_tool() {
    if command -v "$1" &>/dev/null; then
        VERSION=$($1 --version 2>&1 | head -1)
        echo -e "  ${GREEN}✓${NC} $1: $VERSION"
    else
        echo -e "  ${YELLOW}✗${NC} $1: 설치되지 않음"
    fi
}

check_tool gcc
check_tool make
check_tool nasm
check_tool meson
check_tool ninja
check_tool qemu-system-x86_64
check_tool cpio
check_tool grub-mkrescue
check_tool xorriso
check_tool gdb
check_tool strace

echo ""
echo "========================================="
echo "  설치 완료!"
echo "========================================="
echo ""
echo "다음 단계:"
echo "  1. cd /mnt/e/personal_project/citc_os"
echo "  2. make -C system/citcinit    (citcinit 빌드)"
echo "  3. bash tools/mkiso/build.sh  (ISO 이미지 생성)"
echo "  4. bash tools/run-qemu.sh     (QEMU에서 테스트)"
