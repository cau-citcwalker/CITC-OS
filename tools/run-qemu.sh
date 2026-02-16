#!/bin/bash
# CITC OS - QEMU 테스트 실행 스크립트
# ======================================
#
# QEMU란?
#   "Quick EMUlator"의 약자.
#   하드웨어를 소프트웨어로 에뮬레이션하는 가상 머신.
#   CPU, RAM, 디스크, GPU, 네트워크를 모두 가상으로 만들어줌.
#
#   왜 QEMU를 사용하는가?
#   - OS 개발에서 가장 많이 쓰이는 도구
#   - 빠른 반복: 코드 수정 → 빌드 → QEMU 실행 (몇 초)
#   - 안전: 실제 하드웨어를 망가뜨릴 위험 없음
#   - 디버깅: GDB 연결, 시리얼 콘솔 등 디버그 기능
#
# 사용법:
#   bash tools/run-qemu.sh          # 텍스트 모드 (시리얼 콘솔)
#   bash tools/run-qemu.sh --gui    # 그래픽 모드 (프레임버퍼)
#   bash tools/run-qemu.sh --debug  # GDB 디버그 모드
#
# 요구사항:
#   - QEMU 설치됨 (sudo apt install qemu-system-x86)
#   - Linux 커널 이미지 (build/vmlinuz 또는 시스템 커널)
#   - initramfs 이미지 (build/initramfs.cpio.gz)

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# ============================================
# 커널 이미지 찾기
# ============================================
# 우선순위:
# 1. 직접 빌드한 커널 (build/vmlinuz)
# 2. 호스트 시스템 커널 (개발 중 빠른 테스트용)
#
# 처음에는 직접 커널을 빌드하지 않고,
# 호스트(WSL)의 커널을 빌려서 쓸 수 있습니다.
# 우리 citcinit만 테스트하는 것이니까.

KERNEL=""
if [ -f "${BUILD_DIR}/vmlinuz" ]; then
    KERNEL="${BUILD_DIR}/vmlinuz"
    echo "커널: 직접 빌드한 커널 사용"
elif [ -f "/boot/vmlinuz-$(uname -r)" ]; then
    KERNEL="/boot/vmlinuz-$(uname -r)"
    echo "커널: 호스트 시스템 커널 사용 ($(uname -r))"
elif ls /boot/vmlinuz-* 1>/dev/null 2>&1; then
    KERNEL="$(ls /boot/vmlinuz-* | sort -V | tail -1)"
    echo "커널: ${KERNEL}"
else
    echo "ERROR: 사용 가능한 커널을 찾을 수 없습니다!"
    echo ""
    echo "해결 방법:"
    echo "  1. 커널 빌드: bash tools/build-kernel.sh"
    echo "  2. 호스트 커널 설치: sudo apt install linux-image-generic"
    exit 1
fi

# ============================================
# initramfs 확인
# ============================================
INITRAMFS="${BUILD_DIR}/initramfs.cpio.gz"
if [ ! -f "${INITRAMFS}" ]; then
    echo "ERROR: initramfs를 찾을 수 없습니다!"
    echo "먼저 빌드하세요: bash tools/mkrootfs/build-initramfs.sh"
    exit 1
fi

echo "initramfs: ${INITRAMFS}"
echo ""

# ============================================
# QEMU 옵션 설명
# ============================================
# 각 옵션이 하는 일을 상세히 설명합니다.
# OS 개발에서 이 옵션들을 이해하는 것이 중요합니다.

# ============================================
# 공통 옵션 (모든 모드에서 사용)
# ============================================
# -append는 모드별로 다르므로 여기에 포함하지 않음!
QEMU_OPTS=(
    # === CPU/메모리 ===
    -m 1G
    -smp 2

    # === 부팅 ===
    -kernel "${KERNEL}"
    -initrd "${INITRAMFS}"

    # === 네트워크 ===
    -netdev user,id=net0
    -device virtio-net-pci,netdev=net0
)

# KVM 지원 확인
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    echo "KVM 지원: 활성화 (하드웨어 가속)"
    QEMU_OPTS+=(-enable-kvm -cpu host)
else
    echo "KVM 지원: 비활성화 (소프트웨어 에뮬레이션 - 느릴 수 있음)"
fi

# ============================================
# 모드 선택: 텍스트 vs 그래픽
# ============================================
# -append와 디스플레이 옵션을 모드별로 추가.
# 배열 수술 대신 처음부터 분리하는 것이 깔끔!

if [ "$1" = "--gui" ]; then
    echo ""
    echo "=== 그래픽 모드 ==="

    # -vga std: 표준 VGA (bochs 호환) → /dev/fb0 생성
    # -serial stdio: 시리얼을 이 터미널에 연결 (명령 입력용)
    # vga=789: VESA 800x600 24bpp
    # console=ttyS0 console=tty0: 양쪽 출력, /dev/console=tty0 (화면)
    #
    # VirtIO 입력 장치:
    #   virtio-keyboard-pci: VirtIO 키보드 (PS/2와 별도)
    #   virtio-tablet-pci: VirtIO 태블릿 (절대좌표 마우스)
    #     → 절대좌표(EV_ABS)를 사용하므로 마우스 캡처 없이 동작.
    #     → QEMU 창과 게스트 OS의 마우스 위치가 항상 동기화됨.
    #     → CONFIG_VIRTIO_INPUT=y 필요 (모듈이면 virtio_input.ko 로딩 필요)
    QEMU_OPTS+=(
        -vga std
        -device virtio-keyboard-pci
        -device virtio-tablet-pci
        -serial stdio
        -append "console=ttyS0 console=tty0 vga=789 init=/sbin/init loglevel=3 panic=5"
    )

    echo "  VGA: std (800x600 24bpp)"
    echo "  입력: VirtIO keyboard + tablet (절대좌표)"
    echo "  시리얼: stdio (이 터미널에서 명령 입력)"
    echo "  QEMU 창에서 그래픽 출력 확인"

elif [ "$1" = "--debug" ]; then
    echo "디버그 모드: GDB 서버 활성화 (localhost:1234)"
    QEMU_OPTS+=(
        -nographic
        -append "console=ttyS0 init=/sbin/init loglevel=3 panic=5"
        -s -S
    )
    echo "GDB 연결: gdb -ex 'target remote :1234' build/vmlinux"

else
    # 기본: 텍스트 모드 (시리얼 콘솔만)
    QEMU_OPTS+=(
        -nographic
        -append "console=ttyS0 init=/sbin/init loglevel=3 panic=5"
    )
fi

echo ""
echo "========================================="
echo "  CITC OS 부팅 시작"
echo "========================================="
echo ""
echo "  종료: Ctrl+A, X"
echo "  재부팅: Ctrl+A, C → 'system_reset'"
echo ""
echo "-----------------------------------------"

# QEMU 실행
exec qemu-system-x86_64 "${QEMU_OPTS[@]}"
