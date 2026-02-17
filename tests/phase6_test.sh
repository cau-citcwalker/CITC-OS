#!/bin/bash
# Phase 6 통합 테스트 — OS 인프라 강화 검증
#
# 테스트 항목:
#   1. 오디오 서버 (citcaudio) 빌드 확인
#   2. 컴포지터 빌드 확인
#   3. 쉘 빌드 확인
#   4. 터미널 빌드 확인
#   5. WCL 빌드 + 회귀 테스트 (11/11)
#   6. 새로 추가된 파일 존재 확인
#
# 사용법:
#   bash tests/phase6_test.sh
#
# QEMU 전체 테스트 (별도):
#   bash build.sh && bash tools/run-qemu.sh gui

set -e

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== CITC OS Phase 6 — Integration Test ==="
echo ""

# 1. 오디오 서버 빌드
echo "[1/6] 오디오 서버 빌드..."
if make -C audio/src >/dev/null 2>&1; then
    pass "audio server build"
else
    fail "audio server build"
fi

# 2. 컴포지터 빌드
echo "[2/6] 컴포지터 빌드..."
if make -C display/compositor >/dev/null 2>&1; then
    pass "compositor build"
else
    fail "compositor build"
fi

# 3. 쉘 빌드
echo "[3/6] 쉘 빌드..."
if make -C display/shell >/dev/null 2>&1; then
    pass "shell build"
else
    fail "shell build"
fi

# 4. 터미널 빌드
echo "[4/6] 터미널 빌드..."
if make -C display/terminal >/dev/null 2>&1; then
    pass "terminal build"
else
    fail "terminal build"
fi

# 5. WCL 빌드 + 회귀 테스트
echo "[5/6] WCL 빌드 + 회귀 테스트..."
if make -C wcl/src/loader >/dev/null 2>&1; then
    pass "WCL build"
else
    fail "WCL build"
fi

WCL_RESULT=$(bash wcl/tests/run_all_tests.sh 2>&1)
if echo "$WCL_RESULT" | grep -q "ALL PASS"; then
    WCL_SCORE=$(echo "$WCL_RESULT" | grep "Results:" | head -1)
    pass "WCL regression ($WCL_SCORE)"
else
    fail "WCL regression"
fi

# 6. 새 파일 존재 확인
echo "[6/6] Phase 6 신규 파일 확인..."
NEW_FILES=(
    "audio/src/citcaudio_proto.h"
    "audio/src/citcaudio.c"
    "audio/src/citcaudio_client.h"
    "audio/src/beep.c"
    "display/font/psf2.h"
    "tools/mkwallpaper/raw_convert.py"
)

for f in "${NEW_FILES[@]}"; do
    if [ -f "$ROOT/$f" ]; then
        pass "$f exists"
    else
        fail "$f missing"
    fi
done

# 결과 요약
echo ""
TOTAL=$((PASS + FAIL))
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
if [ $FAIL -eq 0 ]; then
    echo "ALL PASS"
    exit 0
else
    echo "SOME TESTS FAILED"
    exit 1
fi
