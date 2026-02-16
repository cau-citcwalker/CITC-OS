#!/bin/bash
# CITC OS - 샘플 패키지 & 저장소 빌드 스크립트
# ==============================================
#
# 테스트용 .cpkg 패키지 파일을 생성하고,
# HTTP 서버로 제공할 저장소 디렉토리도 구성합니다.
#
# 생성되는 것:
#   build/packages/hello-1.0.cpkg      - hello 패키지
#   build/packages/greeting-1.0.cpkg   - greeting 패키지 (hello 의존)
#   build/repo/PKGINDEX                - 패키지 인덱스
#   build/repo/hello-1.0.cpkg          - 저장소용 패키지
#   build/repo/greeting-1.0.cpkg       - 저장소용 패키지
#
# 사용법:
#   bash tools/build-sample-packages.sh
#
# 저장소 서버 실행:
#   bash tools/serve-repo.sh

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
PKG_OUTPUT="${BUILD_DIR}/packages"
REPO_DIR="${BUILD_DIR}/repo"
SAMPLE_DIR="${PROJECT_ROOT}/system/citcpkg/sample-packages"

mkdir -p "${PKG_OUTPUT}" "${REPO_DIR}"

echo "========================================="
echo "  CITC OS - 샘플 패키지 빌드"
echo "========================================="
echo ""

# 임시 빌드 디렉토리
PKG_TMP="$(mktemp -d /tmp/citcpkg-build.XXXXXX)"
trap "rm -rf '${PKG_TMP}'" EXIT

# ============================================
# 1. hello 패키지
# ============================================
echo "[1/3] hello 패키지 빌드..."

# 정적 컴파일 (-static): 공유 라이브러리 없이 독립 실행
gcc -static -O2 -o "${PKG_TMP}/hello" "${SAMPLE_DIR}/hello/hello.c"

# 패키지 디렉토리 구조: data/ 아래에 루트 기준 경로
mkdir -p "${PKG_TMP}/hello-pkg/data/usr/bin"
cp "${PKG_TMP}/hello" "${PKG_TMP}/hello-pkg/data/usr/bin/hello"
chmod 755 "${PKG_TMP}/hello-pkg/data/usr/bin/hello"
cp "${SAMPLE_DIR}/hello/PKGINFO" "${PKG_TMP}/hello-pkg/PKGINFO"

cd "${PKG_TMP}/hello-pkg"
tar czf "${PKG_OUTPUT}/hello-1.0.cpkg" PKGINFO data
echo "  hello-1.0.cpkg 생성 완료"

# ============================================
# 2. greeting 패키지
# ============================================
echo "[2/3] greeting 패키지 빌드..."

gcc -static -O2 -o "${PKG_TMP}/greeting" "${SAMPLE_DIR}/greeting/greeting.c"

mkdir -p "${PKG_TMP}/greeting-pkg/data/usr/bin"
cp "${PKG_TMP}/greeting" "${PKG_TMP}/greeting-pkg/data/usr/bin/greeting"
chmod 755 "${PKG_TMP}/greeting-pkg/data/usr/bin/greeting"
cp "${SAMPLE_DIR}/greeting/PKGINFO" "${PKG_TMP}/greeting-pkg/PKGINFO"

cd "${PKG_TMP}/greeting-pkg"
tar czf "${PKG_OUTPUT}/greeting-1.0.cpkg" PKGINFO data
echo "  greeting-1.0.cpkg 생성 완료"

# ============================================
# 3. 패키지 저장소 구성
# ============================================
# 저장소(repository)란?
#   패키지 파일과 인덱스를 제공하는 HTTP 서버.
#
# 구조:
#   build/repo/
#     PKGINDEX              ← 패키지 목록 (citcpkg update가 다운로드)
#     hello-1.0.cpkg        ← 패키지 파일 (citcpkg install이 다운로드)
#     greeting-1.0.cpkg
#
# PKGINDEX 형식:
#   빈 줄로 패키지 구분.
#   각 패키지는 name, version, description, depends, filename 필드.
echo ""
echo "[3/3] 패키지 저장소 구성..."

# 패키지 파일을 저장소 디렉토리에 복사
cp "${PKG_OUTPUT}/"*.cpkg "${REPO_DIR}/"

# PKGINDEX 생성
cat > "${REPO_DIR}/PKGINDEX" << 'EOF'
name=hello
version=1.0
description=Hello World 테스트 프로그램
depends=
filename=hello-1.0.cpkg

name=greeting
version=1.0
description=환영 인사 프로그램
depends=hello
filename=greeting-1.0.cpkg
EOF

echo "  저장소 디렉토리: ${REPO_DIR}"
echo "  PKGINDEX 생성 완료"

# 결과 출력
echo ""
echo "========================================="
echo "  패키지 빌드 완료!"
echo "========================================="
echo ""
echo "  생성된 패키지:"
for pkg in "${PKG_OUTPUT}"/*.cpkg; do
    echo "    $(basename "$pkg")  ($(du -h "$pkg" | cut -f1))"
done
echo ""
echo "  저장소 서버 실행:"
echo "    bash tools/serve-repo.sh"
echo ""
echo "  QEMU에서 테스트:"
echo "    citcpkg update"
echo "    citcpkg search"
echo "    citcpkg install greeting"
