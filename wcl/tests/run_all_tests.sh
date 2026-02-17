#!/bin/bash
# run_all_tests.sh — Phase 5 전체 회귀 테스트
# ================================================
#
# 사용법:
#   cd /mnt/e/personal_project/citc_os
#   bash wcl/tests/run_all_tests.sh
#
# 또는 CITCRUN 경로 지정:
#   CITCRUN=./build/citcrun bash wcl/tests/run_all_tests.sh

CITCRUN="${CITCRUN:-./wcl/src/loader/build/citcrun}"
TESTS="./wcl/tests/build"

PASS=0
FAIL=0
TOTAL=0

run_test() {
    local name="$1"
    ((TOTAL++))
    if $CITCRUN "$TESTS/${name}.exe" > /dev/null 2>&1; then
        echo "  PASS: $name"
        ((PASS++))
    else
        echo "  FAIL: $name"
        ((FAIL++))
    fi
}

echo ""
echo "=== CITC OS WCL — Full Test Suite ==="
echo ""

run_test hello
run_test api_test
run_test reg_test
run_test gui_test
run_test dx_test
run_test cube_test
run_test thread_test
run_test com_test
run_test net_test
run_test d3d12_test
run_test app_test

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="

if [ $FAIL -eq 0 ]; then
    echo "ALL PASS"
    exit 0
else
    echo "SOME TESTS FAILED"
    exit 1
fi
