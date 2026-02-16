#!/bin/sh
# network-setup.sh - CITC OS 네트워크 초기화 스크립트
# ==================================================
#
# 이 스크립트는 oneshot 서비스로 실행됩니다.
# 네트워크를 설정한 후 종료하면 "완료" 상태가 됩니다.
#
# 네트워크 초기화 순서:
#   1. loopback 인터페이스 활성화 (lo = 127.0.0.1)
#   2. 이더넷 인터페이스 찾기 (eth0 등)
#   3. DHCP로 IP 주소 받기
#
# loopback(lo)이란?
#   자기 자신에게 통신하는 가상 인터페이스.
#   127.0.0.1 (localhost) 주소.
#   왜 필요? 프로그램끼리 네트워크로 통신할 때 사용.
#   예: 웹 브라우저가 로컬 웹 서버에 접속 (127.0.0.1:8080)

echo "[NET] Starting network initialization..."

# === 1단계: loopback 활성화 ===
#
# 모든 OS에서 가장 먼저 하는 네트워크 작업.
# ip link set lo up = lo 인터페이스 켜기
# ip addr add 127.0.0.1/8 = localhost IP 할당
ip link set lo up
ip addr add 127.0.0.1/8 dev lo 2>/dev/null
echo "[NET] lo interface up (127.0.0.1)"

# === 2단계: 이더넷 인터페이스 찾기 ===
#
# Linux에서 네트워크 인터페이스 이름 규칙:
#   전통:  eth0, eth1 (이더넷), wlan0 (와이파이)
#   현대:  enp0s3 (PCI 위치 기반), ens3 (슬롯 기반)
#
# /sys/class/net/ 디렉토리에 모든 네트워크 인터페이스가 나열됨.
# lo(루프백)를 제외한 첫 번째 인터페이스를 사용.
IFACE=""
for iface in /sys/class/net/*; do
    name=$(basename "$iface")
    # lo 와 sit0(IPv6 터널) 건너뛰기
    case "$name" in
        lo|sit*) continue ;;
    esac
    IFACE="$name"
    break
done

if [ -z "$IFACE" ]; then
    echo "[NET] Error: no network interface found"
    echo "[NET] Check QEMU -netdev option"
    exit 1
fi

echo "[NET] Interface found: $IFACE"

# === 3단계: 인터페이스 활성화 ===
ip link set "$IFACE" up
echo "[NET] $IFACE interface up"

# === 4단계: DHCP로 IP 주소 받기 ===
#
# udhcpc 옵션:
#   -i eth0          = 이 인터페이스에서 DHCP 실행
#   -s /path/script  = DHCP 이벤트 핸들러 스크립트
#   -n               = 실패하면 즉시 종료 (무한 재시도 안 함)
#   -q               = IP 받으면 즉시 종료 (백그라운드 안 함)
#   -t 5             = 최대 5번 시도
#
# -q 옵션이 중요:
#   없으면 udhcpc가 백그라운드에서 계속 실행되면서
#   IP 갱신을 관리함 (데몬 모드).
#   -q를 쓰면 한 번 받고 끝. oneshot 서비스에 적합.
echo "[NET] DHCP request... ($IFACE)"
udhcpc -i "$IFACE" \
       -s /etc/citc/scripts/udhcpc-default.script \
       -n -q -t 5

if [ $? -eq 0 ]; then
    echo "[NET] Network setup complete!"
    echo "[NET] Interface status:"
    ip addr show "$IFACE" | head -3
    echo "[NET] Routing table:"
    ip route
else
    echo "[NET] DHCP failed! Manual setup:"
    echo "  ip addr add 10.0.2.15/24 dev $IFACE"
    echo "  ip route add default via 10.0.2.2"
    exit 1
fi

exit 0
