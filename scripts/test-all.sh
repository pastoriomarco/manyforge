#!/usr/bin/env bash
# Run all ManyForge tests.
# Intended to run inside the Docker dev container.
#
# Usage (from /workspace):
#   ./scripts/test-all.sh          # all tests
#   ./scripts/test-all.sh cpp      # C++ only
#   ./scripts/test-all.sh python   # Python only
#   ./scripts/test-all.sh bindings # pybind11 only

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

FILTER="${1:-all}"
PASS=0
FAIL=0

section() { echo -e "\n${GREEN}=== $1 ===${NC}"; }
skip()    { echo -e "${YELLOW}  SKIP: $1${NC}"; }

run_cpp_tests() {
    section "C++ manyforge_core tests"
    local build="$BUILD_DIR/core"
    mkdir -p "$build" && cd "$build"
    cmake "$WORKSPACE/manyforge_core" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -5
    if ctest --output-on-failure; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_roboplan_tests() {
    section "C++ manyforge_roboplan tests"
    local build="$BUILD_DIR/roboplan"
    mkdir -p "$build" && cd "$build"

    PINOCCHIO_CMAKE_DIR=$(find /usr -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1)
    PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "$PINOCCHIO_CMAKE_DIR")")")
    local pybind11_dir
    pybind11_dir=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())" 2>/dev/null || echo "")
    CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}" \
        cmake "$WORKSPACE/manyforge_roboplan" -DCMAKE_BUILD_TYPE=Release \
        ${pybind11_dir:+-Dpybind11_DIR="$pybind11_dir"} 2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -5
    if ctest --output-on-failure; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi

    # Export roboplan build dir + Python wrappers so integration tests can import
    if ls "$build"/manyforge_roboplan_py*.so 1>/dev/null 2>&1; then
        export PYTHONPATH="$build:$WORKSPACE/manyforge_roboplan/python:${PYTHONPATH:-}"
    fi
}

run_pybind_tests() {
    section "pybind11 bindings tests"
    local core_build="$BUILD_DIR/core"
    local pybind_build="$BUILD_DIR/pybind"

    if [ ! -f "$core_build/libmanyforge_core.a" ]; then
        skip "Build manyforge_core first (run with 'all' or 'cpp')"
        return
    fi

    local pybind11_dir
    pybind11_dir=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())" 2>/dev/null || echo "")

    mkdir -p "$pybind_build" && cd "$pybind_build"
    cmake "$WORKSPACE/manyforge_core/python" \
        -DMANYFORGE_CORE_DIR="$core_build" \
        -DMANYFORGE_CORE_INCLUDE_DIR="$WORKSPACE/manyforge_core/include" \
        ${pybind11_dir:+-Dpybind11_DIR="$pybind11_dir"} \
        2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -3
    export PYTHONPATH="$pybind_build:${PYTHONPATH:-}"
    cd "$WORKSPACE"
    if pytest manyforge_core/python/tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_python_tests() {
    section "Python manyforge_behavior tests"
    cd "$WORKSPACE"
    if pytest manyforge_behavior/tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_hmi_tests() {
    section "Python manyforge_hmi tests"
    cd "$WORKSPACE"
    if pytest manyforge_hmi/tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_integration_tests() {
    section "Integration tests"
    cd "$WORKSPACE"
    if pytest tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

case "$FILTER" in
    cpp)
        run_cpp_tests
        ;;
    roboplan)
        run_roboplan_tests
        ;;
    bindings)
        run_pybind_tests
        ;;
    python)
        run_python_tests
        run_hmi_tests
        ;;
    integration)
        run_integration_tests
        ;;
    all)
        run_cpp_tests
        run_roboplan_tests
        run_pybind_tests
        run_python_tests
        run_hmi_tests
        run_integration_tests
        ;;
    *)
        echo "Usage: $0 [all|cpp|roboplan|bindings|python|integration]"
        exit 1
        ;;
esac

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}All $PASS test suite(s) passed.${NC}"
else
    echo -e "${RED}$FAIL test suite(s) FAILED${NC} ($PASS passed)"
    exit 1
fi
