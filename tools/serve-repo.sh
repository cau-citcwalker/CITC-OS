#!/bin/bash
# CITC OS - 패키지 저장소 HTTP 서버
# ===================================
#
# 패키지 저장소를 HTTP로 제공하는 간단한 서버.
#
# 작동 원리:
#   Python의 내장 HTTP 서버를 사용.
#   build/repo/ 디렉토리의 파일을 HTTP로 제공.
#
# 네트워크 구조:
#   ┌─────────────┐          ┌─────────────┐
#   │ WSL (호스트)  │  ◄────  │ QEMU (게스트) │
#   │ :8080       │  wget   │ 10.0.2.15   │
#   │ 10.0.2.2    │          │             │
#   └─────────────┘          └─────────────┘
#
#   QEMU 유저모드 네트워킹에서 호스트는 항상 10.0.2.2.
#   게스트에서 wget http://10.0.2.2:8080/PKGINDEX 로 접근.
#
# 사용법:
#   bash tools/serve-repo.sh
#
# 전제조건:
#   먼저 bash tools/build-sample-packages.sh 실행하여
#   build/repo/ 디렉토리를 생성해야 합니다.

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="${PROJECT_ROOT}/build/repo"

if [ ! -d "${REPO_DIR}" ]; then
    echo "오류: ${REPO_DIR} 없음"
    echo "먼저 실행: bash tools/build-sample-packages.sh"
    exit 1
fi

if [ ! -f "${REPO_DIR}/PKGINDEX" ]; then
    echo "오류: PKGINDEX 없음"
    echo "먼저 실행: bash tools/build-sample-packages.sh"
    exit 1
fi

echo "========================================="
echo "  CITC OS 패키지 저장소 서버"
echo "========================================="
echo ""
echo "  디렉토리: ${REPO_DIR}"
echo "  주소:     http://0.0.0.0:8080/"
echo ""
echo "  QEMU에서: http://10.0.2.2:8080/"
echo ""
echo "  제공 파일:"
ls -la "${REPO_DIR}/"
echo ""
echo "  Ctrl+C로 종료"
echo "========================================="
echo ""

cd "${REPO_DIR}"
python3 -m http.server 8080
