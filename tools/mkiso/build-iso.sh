#!/bin/bash
# CITC OS - ISO 이미지 빌드 스크립트
# ======================================
#
# ISO 9660이란?
#   CD/DVD의 표준 파일시스템 형식입니다.
#   USB에 구워도 동작합니다 (하이브리드 ISO).
#
#   하이브리드 ISO = ISO 9660 + MBR 부트 섹터
#   → CD/DVD에서도 부팅 가능
#   → USB에서도 부팅 가능
#   → QEMU에서도 부팅 가능 (우리가 테스트하는 방식)
#
# El Torito란?
#   CD/DVD에서 부팅하기 위한 표준 규격.
#   ISO 안에 "부트 카탈로그"와 "부트 이미지"를 포함시켜서
#   BIOS가 이것을 찾아 부팅할 수 있게 합니다.
#
# 사용법:
#   sudo bash tools/mkiso/build-iso.sh
#
# 출력:
#   build/citcos.iso - 부팅 가능한 ISO 이미지
#
# 이 ISO를 사용하는 방법:
#   1. QEMU:  qemu-system-x86_64 -cdrom build/citcos.iso -m 1G
#   2. USB:   sudo dd if=build/citcos.iso of=/dev/sdX bs=4M status=progress
#   3. VirtualBox: 새 VM 만들고 ISO를 CD로 마운트

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
ISO_DIR="${BUILD_DIR}/iso_root"
OUTPUT="${BUILD_DIR}/citcos.iso"

echo "========================================="
echo "  CITC OS - ISO 이미지 빌드"
echo "========================================="
echo ""

# ============================================
# 사전 조건 확인
# ============================================
echo "[사전 확인] 필요한 도구 확인..."

MISSING=""
for tool in grub-mkrescue xorriso mformat; do
    if ! command -v "$tool" &>/dev/null; then
        MISSING="${MISSING} ${tool}"
    fi
done

if [ -n "$MISSING" ]; then
    echo ""
    echo "ERROR: 다음 도구가 설치되지 않았습니다:${MISSING}"
    echo ""
    echo "설치 방법:"
    echo "  sudo apt-get install grub-pc-bin grub-efi-amd64-bin \\"
    echo "    grub-common xorriso mtools"
    echo ""
    echo "또는 전체 툴체인 설치:"
    echo "  bash tools/setup-toolchain.sh"
    exit 1
fi
echo "  도구 확인 완료"

# ============================================
# 커널 이미지 찾기
# ============================================
echo ""
echo "[1/4] 커널 이미지 찾기..."

KERNEL=""
if [ -f "${BUILD_DIR}/vmlinuz" ]; then
    KERNEL="${BUILD_DIR}/vmlinuz"
    echo "  직접 빌드한 커널: ${KERNEL}"
elif [ -f "/boot/vmlinuz-$(uname -r)" ]; then
    KERNEL="/boot/vmlinuz-$(uname -r)"
    echo "  호스트 커널 사용: ${KERNEL}"
    echo "  (참고: 나중에 직접 빌드한 커널로 교체하세요)"
elif ls /boot/vmlinuz-* 1>/dev/null 2>&1; then
    KERNEL="$(ls /boot/vmlinuz-* | sort -V | tail -1)"
    echo "  시스템 커널: ${KERNEL}"
else
    echo "  ERROR: 커널을 찾을 수 없습니다!"
    echo ""
    echo "  해결 방법 (택 1):"
    echo "  a) 호스트 커널 설치: sudo apt install linux-image-generic"
    echo "  b) 커널 직접 빌드:   bash tools/build-kernel.sh"
    exit 1
fi

# ============================================
# initramfs 확인
# ============================================
INITRAMFS="${BUILD_DIR}/initramfs.cpio.gz"
if [ ! -f "${INITRAMFS}" ]; then
    echo "  ERROR: initramfs를 찾을 수 없습니다!"
    echo "  먼저 빌드: sudo bash tools/mkrootfs/build-initramfs.sh"
    exit 1
fi
echo "  initramfs: ${INITRAMFS}"

# ============================================
# ISO 디렉토리 구조 생성
# ============================================
# ISO 안에 들어갈 파일 구조를 만듭니다.
#
# 구조:
#   iso_root/
#     boot/
#       grub/
#         grub.cfg     ← GRUB 설정 파일
#       vmlinuz        ← 커널
#       initramfs.cpio.gz ← 루트 파일시스템
echo ""
echo "[2/4] ISO 디렉토리 구조 생성..."

rm -rf "${ISO_DIR}"
mkdir -p "${ISO_DIR}/boot/grub"

# 커널 복사
cp "${KERNEL}" "${ISO_DIR}/boot/vmlinuz"
echo "  커널 복사 완료"

# initramfs 복사
cp "${INITRAMFS}" "${ISO_DIR}/boot/initramfs.cpio.gz"
echo "  initramfs 복사 완료"

# ============================================
# GRUB 설정 파일 생성
# ============================================
# grub.cfg는 GRUB 부트로더의 설정 파일입니다.
#
# GRUB이 하는 일:
#   1. 화면에 부팅 메뉴를 표시
#   2. 사용자가 선택하면 (또는 타임아웃 후)
#   3. 커널을 RAM에 로드
#   4. initramfs를 RAM에 로드
#   5. 커널에 명령줄 파라미터를 전달
#   6. 커널 실행!
#
# grub.cfg 문법:
#   menuentry "이름" { ... }  → 부팅 메뉴 항목
#   linux /boot/vmlinuz ...   → 커널 로드
#   initrd /boot/initramfs... → initramfs 로드
#   boot                       → 부팅 실행
echo ""
echo "[3/4] GRUB 설정 파일 생성..."

cat > "${ISO_DIR}/boot/grub/grub.cfg" << 'GRUBCFG'
# CITC OS GRUB Configuration
# ===========================

# 타임아웃 설정
#   5초 동안 메뉴를 표시하고, 선택이 없으면 기본 항목으로 부팅
set timeout=5
set default=0

# 그래픽 모드 (텍스트 메뉴)
set gfxmode=auto
terminal_output console

# 메뉴 색상
set menu_color_normal=white/black
set menu_color_highlight=black/light-gray

# ── 메인 부팅 항목 ──
menuentry "CITC OS v0.1" {
    echo "Booting CITC OS..."

    # 커널 로드
    #   root=none      : 루트 디바이스 없음 (initramfs에서 동작)
    #   init=/sbin/init: PID 1 = citcinit
    #   console=ttyS0  : 시리얼 콘솔 (QEMU 터미널)
    #   console=tty0   : 물리 콘솔 (모니터) — 마지막=주 콘솔
    #   loglevel=7     : 커널 메시지 전부 출력
    #   panic=10       : kernel panic 시 10초 후 재부팅
    linux /boot/vmlinuz \
        init=/sbin/init \
        console=ttyS0,115200 \
        console=tty0 \
        loglevel=7 \
        panic=10

    # initramfs 로드
    initrd /boot/initramfs.cpio.gz

    boot
}

# ── 디버그 모드 (커널 메시지 상세 출력) ──
menuentry "CITC OS v0.1 (Debug Mode)" {
    echo "Booting in debug mode..."

    linux /boot/vmlinuz \
        init=/sbin/init \
        console=ttyS0,115200 \
        console=tty0 \
        loglevel=8 \
        earlyprintk=serial \
        panic=10 \
        debug

    initrd /boot/initramfs.cpio.gz

    boot
}

# ── 비상 쉘 (citcinit 대신 /bin/sh 직접 실행) ──
# citcinit에 문제가 있을 때 사용
menuentry "Emergency Shell (bypass citcinit)" {
    echo "Starting emergency shell..."

    # init=/bin/sh: citcinit 대신 busybox sh를 직접 PID 1로 실행
    # 이러면 아무 초기화도 되지 않은 상태에서 쉘이 뜸
    # 수동으로 mount -t proc proc /proc 등을 해야 함
    linux /boot/vmlinuz \
        init=/bin/sh \
        console=ttyS0,115200 \
        console=tty0 \
        loglevel=7

    initrd /boot/initramfs.cpio.gz

    boot
}
GRUBCFG

echo "  grub.cfg 생성 완료"
echo ""
echo "  부팅 메뉴:"
echo "    1. CITC OS v0.1            (기본)"
echo "    2. CITC OS v0.1 (Debug)    (커널 디버그)"
echo "    3. Emergency Shell          (비상 쉘)"

# ============================================
# ISO 이미지 생성
# ============================================
# grub-mkrescue가 실제로 하는 일:
#
#   1. ISO 9660 파일시스템 이미지를 생성
#   2. BIOS 부팅을 위한 El Torito 부트 이미지 삽입
#      - GRUB의 core.img를 부트 이미지로 사용
#      - BIOS가 이 이미지를 메모리에 로드하여 GRUB 시작
#   3. UEFI 부팅을 위한 EFI 시스템 파티션 이미지 삽입
#      - FAT 파티션 안에 /EFI/BOOT/BOOTX64.EFI 배치
#      - UEFI 펌웨어가 이 파일을 자동으로 찾아 실행
#   4. 하이브리드 MBR 추가 (isohybrid)
#      - ISO를 USB에 dd로 구워도 부팅 가능하게 함
#
# 옵션:
#   -o: 출력 파일
#   --: 포함할 디렉토리
echo ""
echo "[4/4] ISO 이미지 생성..."
echo "  (grub-mkrescue 실행 중...)"

grub-mkrescue \
    -o "${OUTPUT}" \
    "${ISO_DIR}" \
    2>&1 | while read -r line; do
        # 진행 상황 출력 (너무 장황한 출력 필터링)
        case "$line" in
            *xorriso*|*NOTE*|*WARNING*)
                ;;
            *)
                [ -n "$line" ] && echo "  ${line}"
                ;;
        esac
    done

# ============================================
# 결과 확인
# ============================================
if [ ! -f "${OUTPUT}" ]; then
    echo ""
    echo "ERROR: ISO 생성 실패!"
    echo "위의 에러 메시지를 확인하세요."
    exit 1
fi

echo ""
echo "========================================="
echo "  ISO 빌드 완료!"
echo "========================================="
echo ""
echo "  파일: ${OUTPUT}"
echo "  크기: $(du -h "${OUTPUT}" | cut -f1)"
echo ""

# ISO 내용 확인
echo "  ISO 내용물:"
if command -v xorriso &>/dev/null; then
    xorriso -indev "${OUTPUT}" -ls / 2>/dev/null | grep -v "^$" | head -20 | sed 's/^/    /'
fi

echo ""
echo "========================================="
echo "  사용 방법"
echo "========================================="
echo ""
echo "  1. QEMU에서 테스트 (가장 빠름):"
echo "     qemu-system-x86_64 -cdrom ${OUTPUT} -m 1G -nographic \\"
echo "       -append 'console=ttyS0'"
echo ""
echo "  2. QEMU에서 테스트 (GUI 창):"
echo "     qemu-system-x86_64 -cdrom ${OUTPUT} -m 1G"
echo ""
echo "  3. USB에 굽기 (실제 PC 부팅):"
echo "     sudo dd if=${OUTPUT} of=/dev/sdX bs=4M status=progress"
echo "     ※ /dev/sdX를 USB 장치로 정확히 교체하세요!"
echo "     ※ 잘못된 장치에 쓰면 데이터가 날아갑니다!"
echo ""
echo "  4. VirtualBox/VMware:"
echo "     새 VM 생성 → ISO를 CD-ROM으로 마운트 → 부팅"
echo ""
echo "  5. 이 프로젝트의 QEMU 스크립트:"
echo "     bash tools/run-qemu.sh"
