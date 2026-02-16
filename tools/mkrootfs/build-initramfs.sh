#!/bin/bash
# CITC OS - initramfs 빌드 스크립트
# ====================================
#
# initramfs란?
#   "Initial RAM Filesystem"의 약자.
#   커널이 부팅할 때 사용하는 임시 파일시스템입니다.
#   RAM에 로드되며, 실제 루트 파일시스템을 마운트하기 전에 사용됩니다.
#
#   왜 필요한가?
#   커널이 부팅되면 디스크 드라이버를 로드해야 디스크에 접근할 수 있습니다.
#   그런데 드라이버가 디스크에 있으면? → 닭과 달걀 문제!
#   해결: 최소한의 파일시스템을 RAM에 올려놓고, 거기서 드라이버를 로드한 후
#         실제 디스크의 루트 파일시스템으로 전환합니다.
#
#   initramfs의 내용물:
#   - /sbin/init → citcinit (우리의 init 시스템)
#   - /bin/sh → busybox (최소 유틸리티 모음)
#   - /dev, /proc, /sys → 마운트 포인트 (빈 디렉토리)
#
# 사용법:
#   bash tools/mkrootfs/build-initramfs.sh
#
# 출력:
#   build/initramfs.cpio.gz - 부팅용 initramfs 이미지

set -e

# 프로젝트 루트 디렉토리 (이 스크립트의 위치 기준)
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
OUTPUT="${BUILD_DIR}/initramfs.cpio.gz"

# initramfs는 Linux 네이티브 파일시스템에서 빌드해야 함
# 이유: /mnt/ 아래(NTFS)에서는 mknod(장치 노드 생성)가 불가능
# NTFS는 Windows 파일시스템이라 Linux의 장치 파일 개념이 없음
# → /tmp (WSL의 ext4)에서 작업 후, 결과물만 프로젝트 디렉토리로 복사
INITRAMFS_DIR="$(mktemp -d /tmp/citcos-initramfs.XXXXXX)"
trap "rm -rf '${INITRAMFS_DIR}'" EXIT

echo "========================================="
echo "  CITC OS - initramfs 빌드"
echo "========================================="
echo ""
echo "프로젝트 루트: ${PROJECT_ROOT}"
echo ""

# 이전 빌드 정리
rm -rf "${INITRAMFS_DIR}"
mkdir -p "${BUILD_DIR}"

# ============================================
# 1단계: 디렉토리 구조 생성
# ============================================
# Linux 파일시스템 구조 (FHS - Filesystem Hierarchy Standard):
#
#   /bin      - 필수 명령어 (sh, ls, cp 등)
#   /sbin     - 시스템 관리 명령어 (init, mount 등)
#   /dev      - 장치 파일 (커널이 생성)
#   /etc      - 설정 파일
#   /proc     - 프로세스 정보 (가상 FS)
#   /sys      - 하드웨어 정보 (가상 FS)
#   /tmp      - 임시 파일
#   /run      - 런타임 데이터
#   /root     - root 사용자 홈 디렉토리
#   /usr      - 사용자 프로그램
#   /var      - 가변 데이터 (로그 등)
#   /mnt      - 임시 마운트 포인트

echo "[1/7] 디렉토리 구조 생성..."

mkdir -p "${INITRAMFS_DIR}"/{bin,sbin,dev,etc,proc,sys,tmp,run,root,usr/bin,usr/sbin,var/log,mnt}

# /dev 기본 노드는 citcinit이 생성하지만,
# 콘솔은 부팅 초기에 필요하므로 미리 생성
# mknod는 root 권한 필요
if [ "$(id -u)" = "0" ]; then
    mknod -m 600 "${INITRAMFS_DIR}/dev/console" c 5 1
    mknod -m 666 "${INITRAMFS_DIR}/dev/null" c 1 3
    mknod -m 666 "${INITRAMFS_DIR}/dev/zero" c 1 5
    mknod -m 666 "${INITRAMFS_DIR}/dev/tty" c 5 0
    echo "  디바이스 노드 생성 완료"
else
    echo "  경고: root가 아니므로 디바이스 노드 생성 건너뜀"
    echo "  (커널의 devtmpfs가 대신 생성해줍니다)"
fi

echo "  디렉토리 구조 완료"

# ============================================
# 2단계: citcinit 빌드 및 설치
# ============================================
echo ""
echo "[2/7] citcinit 빌드..."

cd "${PROJECT_ROOT}/system/citcinit"
make clean
make

# /sbin/init으로 복사
cp "${PROJECT_ROOT}/system/citcinit/build/citcinit" "${INITRAMFS_DIR}/sbin/init"
chmod 755 "${INITRAMFS_DIR}/sbin/init"

# /init으로도 심볼릭 링크 (커널이 /init도 찾음)
ln -sf /sbin/init "${INITRAMFS_DIR}/init"

echo "  citcinit 설치 완료: /sbin/init"

# shutdown 명령어 설치 + 심볼릭 링크
# argv[0] 트릭: 하나의 바이너리가 이름에 따라 다르게 동작
#   /sbin/shutdown  → 전원 끄기 (기본)
#   /sbin/reboot    → 재부팅
#   /sbin/poweroff  → 전원 끄기
#   /sbin/halt      → 시스템 정지
cp "${PROJECT_ROOT}/system/citcinit/build/shutdown" "${INITRAMFS_DIR}/sbin/shutdown"
chmod 755 "${INITRAMFS_DIR}/sbin/shutdown"
ln -sf /sbin/shutdown "${INITRAMFS_DIR}/sbin/reboot"
ln -sf /sbin/shutdown "${INITRAMFS_DIR}/sbin/poweroff"
ln -sf /sbin/shutdown "${INITRAMFS_DIR}/sbin/halt"

echo "  shutdown 설치 완료: /sbin/{shutdown,reboot,poweroff,halt}"

# ============================================
# 3단계: citcpkg 빌드 및 설치
# ============================================
echo ""
echo "[3/7] citcpkg 빌드..."

cd "${PROJECT_ROOT}/system/citcpkg"
make clean
make

# /usr/bin/citcpkg에 설치
mkdir -p "${INITRAMFS_DIR}/usr/bin"
cp "${PROJECT_ROOT}/system/citcpkg/build/citcpkg" "${INITRAMFS_DIR}/usr/bin/citcpkg"
chmod 755 "${INITRAMFS_DIR}/usr/bin/citcpkg"

echo "  citcpkg 설치 완료: /usr/bin/citcpkg"

# 샘플 패키지 빌드
echo "  샘플 패키지 빌드..."
bash "${PROJECT_ROOT}/tools/build-sample-packages.sh"

# 샘플 패키지를 initramfs에 포함 (/packages/ 디렉토리)
mkdir -p "${INITRAMFS_DIR}/packages"
if [ -d "${BUILD_DIR}/packages" ]; then
    cp "${BUILD_DIR}/packages/"*.cpkg "${INITRAMFS_DIR}/packages/" 2>/dev/null || true
    PKG_COUNT=$(ls "${INITRAMFS_DIR}/packages/"*.cpkg 2>/dev/null | wc -l)
    echo "  샘플 패키지: ${PKG_COUNT}개 포함"
fi

# ============================================
# 3.5단계: fbdraw 빌드 (프레임버퍼 그래픽)
# ============================================
echo ""
echo "[3.5/7] fbdraw 빌드..."

cd "${PROJECT_ROOT}/display/fbdraw"
make clean
make

cp "${PROJECT_ROOT}/display/fbdraw/build/fbdraw" "${INITRAMFS_DIR}/usr/bin/fbdraw"
chmod 755 "${INITRAMFS_DIR}/usr/bin/fbdraw"

echo "  fbdraw 설치 완료: /usr/bin/fbdraw"

# ============================================
# 3.6단계: drmdraw 빌드 (DRM/KMS 그래픽)
# ============================================
echo ""
echo "[3.6/7] drmdraw 빌드..."

cd "${PROJECT_ROOT}/display/drmdraw"
make clean
make

cp "${PROJECT_ROOT}/display/drmdraw/build/drmdraw" "${INITRAMFS_DIR}/usr/bin/drmdraw"
chmod 755 "${INITRAMFS_DIR}/usr/bin/drmdraw"

echo "  drmdraw 설치 완료: /usr/bin/drmdraw"

# ============================================
# 3.7단계: compositor 빌드 (윈도우 관리자)
# ============================================
echo ""
echo "[3.7/7] compositor 빌드..."

cd "${PROJECT_ROOT}/display/compositor"
make clean
make

cp "${PROJECT_ROOT}/display/compositor/build/compositor" "${INITRAMFS_DIR}/usr/bin/compositor"
chmod 755 "${INITRAMFS_DIR}/usr/bin/compositor"

echo "  compositor 설치 완료: /usr/bin/compositor"

# ============================================
# 3.8단계: CDP demo 빌드 (디스플레이 프로토콜 데모)
# ============================================
# CDP(CITC Display Protocol) 클라이언트 데모.
# 컴포지터에 소켓으로 연결하여 윈도우를 만드는 외부 앱.
# Wayland 프로토콜의 핵심 개념을 보여줌.
echo ""
echo "[3.8/7] cdp_demo 빌드..."

cd "${PROJECT_ROOT}/display/protocol"
make clean
make

cp "${PROJECT_ROOT}/display/protocol/build/cdp_demo" "${INITRAMFS_DIR}/usr/bin/cdp_demo"
chmod 755 "${INITRAMFS_DIR}/usr/bin/cdp_demo"

echo "  cdp_demo 설치 완료: /usr/bin/cdp_demo"

# ============================================
# 3.85단계: citcterm 빌드 (터미널 에뮬레이터)
# ============================================
# CDP 클라이언트 터미널 에뮬레이터.
# 컴포지터에 연결하여 그래픽 터미널 윈도우를 제공합니다.
# PTY(pseudo-terminal)를 통해 쉘 프로세스와 양방향 통신.
echo ""
echo "[3.85/7] citcterm 빌드..."

cd "${PROJECT_ROOT}/display/terminal"
make clean
make

cp "${PROJECT_ROOT}/display/terminal/build/citcterm" "${INITRAMFS_DIR}/usr/bin/citcterm"
chmod 755 "${INITRAMFS_DIR}/usr/bin/citcterm"

echo "  citcterm 설치 완료: /usr/bin/citcterm"

# ============================================
# 3.87단계: citcsh 빌드 (커스텀 쉘)
# ============================================
# 커스텀 UNIX 쉘 — 명령어 파싱, 파이프, 리다이렉션 지원.
# citcterm과 citcinit이 /bin/sh 대신 우선 실행합니다.
echo ""
echo "[3.87/7] citcsh 빌드..."

cd "${PROJECT_ROOT}/system/citcsh"
make clean
make

cp "${PROJECT_ROOT}/system/citcsh/build/citcsh" "${INITRAMFS_DIR}/bin/citcsh"
chmod 755 "${INITRAMFS_DIR}/bin/citcsh"

echo "  citcsh 설치 완료: /bin/citcsh"

# ============================================
# 3.88단계: citcshell 빌드 (데스크탑 셸)
# ============================================
# CDP 클라이언트 데스크탑 셸.
# 컴포지터에 패널(태스크바) surface를 만들어
# 앱 런처 버튼과 시계를 표시합니다.
echo ""
echo "[3.88/7] citcshell 빌드..."

cd "${PROJECT_ROOT}/display/shell"
make clean
make

cp "${PROJECT_ROOT}/display/shell/build/citcshell" "${INITRAMFS_DIR}/usr/bin/citcshell"
chmod 755 "${INITRAMFS_DIR}/usr/bin/citcshell"

echo "  citcshell 설치 완료: /usr/bin/citcshell"

# ============================================
# 3.89단계: .desktop 파일 복사 (Class 21)
# ============================================
# citcshell이 /usr/share/applications/*.desktop 파일을 읽어서
# 태스크바 버튼을 동적으로 생성합니다.
echo ""
echo "[3.89/7] .desktop files..."

DESKTOP_SRC="${PROJECT_ROOT}/display/shell/applications"
DESKTOP_DST="${INITRAMFS_DIR}/usr/share/applications"
mkdir -p "${DESKTOP_DST}"

if [ -d "${DESKTOP_SRC}" ]; then
    cp "${DESKTOP_SRC}"/*.desktop "${DESKTOP_DST}/" 2>/dev/null
    DT_COUNT=$(ls -1 "${DESKTOP_DST}"/*.desktop 2>/dev/null | wc -l)
    echo "  .desktop files: ${DT_COUNT} installed"
else
    echo "  Warning: no .desktop source directory"
fi

# ============================================
# 3.895단계: citc-ipc 빌드 (IPC Message Bus, Class 20)
# ============================================
echo ""
echo "[3.895/7] citc-ipc build (IPC daemon)..."

cd "${PROJECT_ROOT}/system/citc-ipc"
make clean
make

mkdir -p "${INITRAMFS_DIR}/usr/sbin"
cp "${PROJECT_ROOT}/system/citc-ipc/build/citc-ipc" "${INITRAMFS_DIR}/usr/sbin/citc-ipc"
chmod 755 "${INITRAMFS_DIR}/usr/sbin/citc-ipc"

echo "  citc-ipc installed: /usr/sbin/citc-ipc"

# ============================================
# 3.9단계: 커널 모듈 복사 (입력 장치용)  [이전 3.8단계]
# ============================================
# 현재 Ubuntu 호스트 커널을 사용 중인데, 마우스/HID 드라이버가
# 모듈(=m)로 빌드되어 있음. initramfs에 모듈을 포함하고
# 부팅 시 로딩해야 마우스가 작동함.
#
# 필요한 모듈:
#   virtio_input.ko — VirtIO 입력 드라이버 (QEMU virtio-tablet/keyboard)
#   psmouse.ko      — PS/2 마우스 드라이버 (폴백)
#   hid.ko          — HID(Human Interface Device) 코어
#   hid-generic.ko  — 일반 HID 드라이버
#   usbhid.ko       — USB HID 드라이버
echo ""
echo "[3.9/7] 커널 모듈 복사 (입력 장치)..."

# 커널 모듈 버전은 QEMU에서 부팅할 커널과 일치해야 함!
# uname -r은 WSL 커널 버전(6.6.x-microsoft)을 반환하지만,
# QEMU에서 부팅하는 커널은 Ubuntu 커널(6.8.x-generic)임.
# → /boot/vmlinuz-*에서 Ubuntu 커널 버전을 추출하거나,
#   /lib/modules/에서 -generic 버전을 찾아 사용.
KVER=""
# 1순위: /lib/modules/에서 -generic 커널 버전 찾기
for d in /lib/modules/*-generic; do
    if [ -d "$d/kernel/drivers" ]; then
        KVER="$(basename "$d")"
        break
    fi
done
# 2순위: 현재 커널
if [ -z "$KVER" ]; then
    KVER="$(uname -r)"
fi
echo "  커널 모듈 버전: ${KVER}"
KMOD_SRC="/lib/modules/${KVER}/kernel/drivers"
KMOD_DST="${INITRAMFS_DIR}/lib/modules"
mkdir -p "${KMOD_DST}"

# 모듈 파일 복사 (.ko.zst 압축 → 풀어서 .ko로 저장)
# insmod는 압축된 모듈을 읽을 수 없으므로 미리 압축 해제
MODULES=(
    "${KMOD_SRC}/virtio/virtio_input.ko.zst"
    "${KMOD_SRC}/input/mouse/psmouse.ko.zst"
    "${KMOD_SRC}/hid/hid.ko.zst"
    "${KMOD_SRC}/hid/hid-generic.ko.zst"
    "${KMOD_SRC}/hid/usbhid/usbhid.ko.zst"
)

MOD_COUNT=0
for mod in "${MODULES[@]}"; do
    if [ -f "$mod" ]; then
        basename_ko="$(basename "$mod" .zst)"
        zstd -dqf "$mod" -o "${KMOD_DST}/${basename_ko}" 2>/dev/null \
            || cp "$mod" "${KMOD_DST}/${basename_ko}.zst"
        MOD_COUNT=$((MOD_COUNT + 1))
    fi
done

# .ko (비압축) 모듈도 확인
for mod_path in "${MODULES[@]}"; do
    ko_path="${mod_path%.zst}"
    if [ -f "$ko_path" ] && [ ! -f "${KMOD_DST}/$(basename "$ko_path")" ]; then
        cp "$ko_path" "${KMOD_DST}/"
        MOD_COUNT=$((MOD_COUNT + 1))
    fi
done

echo "  커널 모듈: ${MOD_COUNT}개 복사됨 → /lib/modules/"

# 모듈 로딩 스크립트 생성
# citcinit이 부팅 초기에 이 스크립트를 실행하거나,
# 쉘에서 수동으로 실행 가능
mkdir -p "${INITRAMFS_DIR}/etc/citc/scripts"
cat > "${INITRAMFS_DIR}/etc/citc/scripts/load-input-modules.sh" << 'MODSCRIPT'
#!/bin/sh
# 입력 장치 커널 모듈 로딩
# 마우스와 USB HID 장치를 사용하려면 이 모듈이 필요합니다.
MODDIR=/lib/modules

echo "[MOD] Loading input device modules..."

# Load order matters! Dependency order:
# 1. virtio_input (VirtIO input) — for QEMU virtio-tablet/keyboard
# 2. hid (HID core) — required by hid-generic, usbhid
# 3. psmouse (PS/2 mouse) — standalone
# 4. hid-generic (generic HID) — requires hid
# 5. usbhid (USB HID) — requires hid
for mod in virtio_input hid psmouse hid-generic usbhid; do
    if [ -f "${MODDIR}/${mod}.ko" ]; then
        ERR=$(insmod "${MODDIR}/${mod}.ko" 2>&1) && \
            echo "[MOD]   ${mod} loaded" || \
            echo "[MOD]   ${mod} failed: ${ERR}"
    else
        echo "[MOD]   ${mod}.ko not found"
    fi
done

echo "[MOD] Module loading complete"
# Brief wait — device registration takes time
sleep 1
ls /dev/input/ 2>/dev/null && echo "[MOD] /dev/input/ devices found" \
    || echo "[MOD] /dev/input/ no devices"
MODSCRIPT
chmod +x "${INITRAMFS_DIR}/etc/citc/scripts/load-input-modules.sh"

echo "  모듈 로딩 스크립트: /etc/citc/scripts/load-input-modules.sh"

# ============================================
# 3.10단계: citcrun 빌드 (Windows PE 로더)
# ============================================
# WCL(Windows Compatibility Layer)의 첫 번째 도구.
# Windows .exe 파일을 로드하고 실행하는 프로그램.
echo ""
echo "[3.10] citcrun 빌드 (PE Loader)..."

cd "${PROJECT_ROOT}/wcl/src/loader"
make clean
make

cp "${PROJECT_ROOT}/wcl/src/loader/build/citcrun" "${INITRAMFS_DIR}/usr/bin/citcrun"
chmod 755 "${INITRAMFS_DIR}/usr/bin/citcrun"

echo "  citcrun 설치 완료: /usr/bin/citcrun"

# 레지스트리 기본 디렉토리 생성 (Class 24: Windows Registry v0.1)
mkdir -p "${INITRAMFS_DIR}/etc/citc-registry/HKLM/SYSTEM/DriveMapping"
mkdir -p "${INITRAMFS_DIR}/etc/citc-registry/HKCU"
mkdir -p "${INITRAMFS_DIR}/etc/citc-registry/HKU"
mkdir -p "${INITRAMFS_DIR}/etc/citc-registry/HKCR"
echo "  레지스트리 디렉토리 생성 완료: /etc/citc-registry/"

# ============================================
# 3.11단계: WCL 테스트 프로그램 크로스 컴파일
# ============================================
# MinGW로 Windows .exe를 Linux에서 빌드합니다.
# 전제조건: x86_64-w64-mingw32-gcc 설치 필요
echo ""
echo "[3.11] WCL 테스트 프로그램 빌드..."

if command -v x86_64-w64-mingw32-gcc &>/dev/null; then
    cd "${PROJECT_ROOT}/wcl/tests"
    make clean
    make

    mkdir -p "${INITRAMFS_DIR}/opt/wcl-tests"
    for exe in "${PROJECT_ROOT}/wcl/tests/build/"*.exe; do
        if [ -f "$exe" ]; then
            cp "$exe" "${INITRAMFS_DIR}/opt/wcl-tests/"
            echo "  $(basename "$exe") 설치 완료: /opt/wcl-tests/$(basename "$exe")"
        fi
    done
else
    echo "  경고: x86_64-w64-mingw32-gcc 없음 — WCL 테스트 건너뜀"
    echo "  설치: sudo apt install gcc-mingw-w64-x86-64"
fi

# ============================================
# 4단계: Busybox 설치
# ============================================
# Busybox란?
#   하나의 실행파일에 수백 개의 Unix 유틸리티가 들어있는 프로그램.
#   ls, cp, cat, sh, mount, ifconfig, vi, grep, find, ps, top...
#   임베디드 Linux, initramfs에서 가장 많이 사용됨.
#
#   작동 원리:
#   busybox 실행파일 하나를 /bin/busybox에 놓고,
#   /bin/sh → /bin/busybox
#   /bin/ls → /bin/busybox
#   형태로 심볼릭 링크를 생성.
#   busybox는 자신이 어떤 이름으로 호출되었는지 확인(argv[0])하여
#   해당 유틸리티로 동작.
echo ""
echo "[4/7] Busybox 설치..."

if command -v busybox &>/dev/null; then
    BUSYBOX="$(which busybox)"
    echo "  시스템 busybox 사용: ${BUSYBOX}"
elif [ -f "${BUILD_DIR}/busybox" ]; then
    BUSYBOX="${BUILD_DIR}/busybox"
    echo "  빌드된 busybox 사용: ${BUSYBOX}"
else
    echo "  busybox를 찾을 수 없습니다."
    echo "  설치: sudo apt-get install busybox-static"
    echo "  또는: wget -O ${BUILD_DIR}/busybox https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
    echo ""
    echo "  busybox-static 설치를 시도합니다..."
    sudo apt-get install -y busybox-static 2>/dev/null || true
    if command -v busybox &>/dev/null; then
        BUSYBOX="$(which busybox)"
    else
        echo "  ERROR: busybox를 설치할 수 없습니다!"
        exit 1
    fi
fi

# busybox를 /bin/busybox에 복사
cp "${BUSYBOX}" "${INITRAMFS_DIR}/bin/busybox"
chmod 755 "${INITRAMFS_DIR}/bin/busybox"

# 심볼릭 링크 생성 (busybox가 제공하는 유틸리티들)
# busybox --list로 전체 목록을 볼 수 있음
BUSYBOX_CMDS=(
    # 쉘
    sh ash

    # 파일 작업
    ls cat cp mv rm mkdir rmdir ln touch chmod chown
    find grep sed awk head tail wc sort

    # 시스템
    # reboot/poweroff/halt는 커스텀 shutdown 바이너리를 사용
    mount umount ps top kill sleep date hostname uname
    dmesg

    # 텍스트 편집
    vi

    # 네트워크
    ifconfig ip ping wget nslookup traceroute nc

    # 아카이브
    tar gzip gunzip

    # 기타
    echo printf test true false env clear
)

for cmd in "${BUSYBOX_CMDS[@]}"; do
    ln -sf /bin/busybox "${INITRAMFS_DIR}/bin/${cmd}"
done

# sbin에도 필요한 것들
# reboot/poweroff/halt는 커스텀 shutdown 바이너리가 설치됨 (위에서)
for cmd in mount umount syslogd klogd udhcpc insmod modprobe lsmod; do
    ln -sf /bin/busybox "${INITRAMFS_DIR}/sbin/${cmd}"
done

echo "  Busybox 설치 완료 (${#BUSYBOX_CMDS[@]}개 유틸리티)"

# ============================================
# 5단계: 설정 파일 생성
# ============================================
echo ""
echo "[5/7] 설정 파일 생성..."

# /etc/hostname
echo "citcos" > "${INITRAMFS_DIR}/etc/hostname"

# /etc/os-release (시스템 정보)
cat > "${INITRAMFS_DIR}/etc/os-release" << 'EOF'
NAME="CITC OS"
VERSION="0.1"
ID=citcos
PRETTY_NAME="CITC OS v0.1"
HOME_URL="https://github.com/citcos"
EOF

# /etc/passwd (사용자 정보 - 최소)
cat > "${INITRAMFS_DIR}/etc/passwd" << 'EOF'
root:x:0:0:root:/root:/bin/sh
EOF

# /etc/group (그룹 정보 - 최소)
cat > "${INITRAMFS_DIR}/etc/group" << 'EOF'
root:x:0:
EOF

# /etc/profile (쉘 프로필 - 프롬프트 설정)
cat > "${INITRAMFS_DIR}/etc/profile" << 'PROFILE'
# CITC OS Shell Profile

# 환경 변수
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root
export TERM=linux
export PS1='\[\033[1;32m\]citcos\[\033[0m\]:\[\033[1;34m\]\w\[\033[0m\]# '

# 환영 메시지
echo ""
echo "  Welcome to CITC OS v0.1"
echo "  Type 'help' for available commands"
echo ""
PROFILE

# /etc/citc/services/ (서비스 설정 파일)
# citcinit이 이 디렉토리의 *.conf 파일을 읽어서 서비스를 등록합니다.
mkdir -p "${INITRAMFS_DIR}/etc/citc/services"
if [ -d "${PROJECT_ROOT}/system/citcinit/services" ]; then
    cp "${PROJECT_ROOT}/system/citcinit/services/"*.conf \
       "${INITRAMFS_DIR}/etc/citc/services/" 2>/dev/null || true
    SVC_COUNT=$(ls "${INITRAMFS_DIR}/etc/citc/services/"*.conf 2>/dev/null | wc -l)
    echo "  서비스 설정 파일: ${SVC_COUNT}개 복사됨"
else
    echo "  경고: 서비스 설정 디렉토리 없음"
fi

# /etc/citc/scripts/ (시스템 스크립트)
# 네트워크 초기화, DHCP 핸들러 등 서비스가 사용하는 스크립트
mkdir -p "${INITRAMFS_DIR}/etc/citc/scripts"
if [ -d "${PROJECT_ROOT}/system/citcinit/scripts" ]; then
    cp "${PROJECT_ROOT}/system/citcinit/scripts/"* \
       "${INITRAMFS_DIR}/etc/citc/scripts/" 2>/dev/null || true
    # 스크립트에 실행 권한 부여
    # Windows(NTFS)에서 만든 파일은 실행 권한이 없을 수 있음
    chmod +x "${INITRAMFS_DIR}/etc/citc/scripts/"*
    SCRIPT_COUNT=$(ls "${INITRAMFS_DIR}/etc/citc/scripts/"* 2>/dev/null | wc -l)
    echo "  시스템 스크립트: ${SCRIPT_COUNT}개 복사됨"
else
    echo "  경고: 시스템 스크립트 디렉토리 없음"
fi

# /etc/resolv.conf (DNS 리졸버 기본 설정)
# udhcpc가 DHCP로 DNS를 받으면 이 파일을 덮어씀.
# 여기서는 빈 상태로 생성.
touch "${INITRAMFS_DIR}/etc/resolv.conf"

echo "  설정 파일 생성 완료"

# ============================================
# 6단계: 패키지 관련 디렉토리 생성
# ============================================
echo ""
echo "[6/7] 패키지 관리자 디렉토리 생성..."

# citcpkg가 사용하는 데이터베이스 디렉토리
mkdir -p "${INITRAMFS_DIR}/var/lib/citcpkg/installed"
mkdir -p "${INITRAMFS_DIR}/var/lib/citcpkg/cache"
echo "  /var/lib/citcpkg/installed 생성 완료"

# /etc/citcpkg/repo.conf (저장소 설정)
# citcpkg update/install이 이 URL에서 패키지를 다운로드.
# QEMU 유저모드 네트워킹에서 호스트는 10.0.2.2.
mkdir -p "${INITRAMFS_DIR}/etc/citcpkg"
cat > "${INITRAMFS_DIR}/etc/citcpkg/repo.conf" << 'REPOCONF'
# CITC OS Package Repository
# 호스트에서 bash tools/serve-repo.sh 실행 필요
url=http://10.0.2.2:8080
REPOCONF
echo "  /etc/citcpkg/repo.conf 생성 완료"

# ============================================
# 7단계: initramfs 아카이브 생성
# ============================================
# cpio + gzip으로 아카이브를 만듭니다.
#
# cpio란?
#   "Copy In and Out"의 약자. 파일 아카이브 형식.
#   tar와 비슷하지만, Linux 커널은 cpio 형식의 initramfs를 사용.
#
# 형식: newc (new ASCII format)
#   find . | cpio -H newc -o | gzip > initramfs.cpio.gz
#
#   find .          → 현재 디렉토리의 모든 파일 나열
#   cpio -H newc -o → cpio 아카이브로 변환 (newc 형식)
#   gzip            → gzip으로 압축
echo ""
echo "[7/7] initramfs 아카이브 생성..."

cd "${INITRAMFS_DIR}"
find . | cpio -H newc -o --quiet 2>/dev/null | gzip > "${OUTPUT}"

echo ""
echo "========================================="
echo "  빌드 완료!"
echo "========================================="
echo ""
echo "  initramfs: ${OUTPUT}"
echo "  크기: $(du -h "${OUTPUT}" | cut -f1)"
echo ""
echo "  내용물:"
echo "    /sbin/init  → citcinit (PID 1)"
echo "    /bin/sh     → busybox ash 쉘"
echo "    + $(find "${INITRAMFS_DIR}" -type f -o -type l | wc -l)개 파일/링크"
echo ""
echo "  다음 단계: QEMU에서 부팅 테스트"
echo "    bash tools/run-qemu.sh"
