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
CTEST_TEST_TIMEOUT_S="${CTEST_TEST_TIMEOUT_S:-60}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

FILTER="${1:-all}"
PASS=0
FAIL=0

section() { echo -e "\n${GREEN}=== $1 ===${NC}"; }
skip()    { echo -e "${YELLOW}  SKIP: $1${NC}"; }

require_ctest_tests() {
    local suite="$1"
    local listing
    listing="$(ctest -N)"
    echo "$listing" | tail -3
    if echo "$listing" | grep -q "Total Tests: 0"; then
        echo -e "${RED}  FAIL: ${suite} configured zero CTest tests${NC}"
        return 1
    fi
    return 0
}

setup_pythonpath() {
    export PYTHONPATH="$WORKSPACE:$WORKSPACE/manyforge_behavior:${PYTHONPATH:-}"
}

reset_stale_python_build_dir() {
    local build_dir="$1"
    local cache_file="$build_dir/CMakeCache.txt"
    local current_site
    current_site=$(python3 -c "import site; print(site.getsitepackages()[0])")

    if [ -f "$cache_file" ] && grep -q "/usr/local/lib/python3\\." "$cache_file" && ! grep -q "$current_site" "$cache_file"; then
        rm -rf "$build_dir"
    fi
}

python_has_modules() {
    python3 - "$@" <<'PY'
import importlib
import sys

for name in sys.argv[1:]:
    try:
        importlib.import_module(name)
    except Exception:
        raise SystemExit(1)
raise SystemExit(0)
PY
}

run_cpp_tests() {
    section "C++ manyforge_core tests"
    local build="$BUILD_DIR/core"
    reset_stale_python_build_dir "$build"
    mkdir -p "$build" && cd "$build"
    cmake "$WORKSPACE/manyforge_core" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=ON \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -5
    # Both layers matter: the per-test TIMEOUT property baked by cmake
    # above (the -D flag re-evaluates on every run, so the env var
    # propagates correctly through the launcher path) AND the ctest
    # CLI flag as a safety net for any ad-hoc `ctest` invocation that
    # bypasses test-all.sh. ctest takes the min of the two.
    if ! require_ctest_tests "manyforge_core"; then
        FAIL=$((FAIL + 1))
    elif ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_roboplan_tests() {
    section "C++ manyforge_roboplan tests"
    if [ -f /opt/ros/jazzy/setup.bash ]; then
        # RoboPlan's ROS-enabled targets may pull in ament_cmake. Source the
        # ROS environment so ament's Python helpers are importable during CMake
        # configure; this keeps the standard suite aligned with launch.sh.
        # shellcheck disable=SC1091
        set +u
        source /opt/ros/jazzy/setup.bash
        set -u
    fi
    local build="$BUILD_DIR/roboplan"
    reset_stale_python_build_dir "$build"
    mkdir -p "$build" && cd "$build"

    PINOCCHIO_CMAKE_DIR=$(
        python3 - <<'PY'
import pathlib
import site

roots = [pathlib.Path("/usr"), pathlib.Path("/usr/local")]
roots.extend(pathlib.Path(p) for p in site.getsitepackages())
seen = set()
for root in roots:
    if root in seen or not root.exists():
        continue
    seen.add(root)
    for candidate in root.glob("**/pinocchioConfig.cmake"):
        print(candidate.parent)
        raise SystemExit(0)
raise SystemExit(1)
PY
    ) || true
    if [ -z "$PINOCCHIO_CMAKE_DIR" ]; then
        skip "Pinocchio CMake package not found; run roboplan tests in manyforge-dev or install pinocchio locally"
        return
    fi
    PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "$PINOCCHIO_CMAKE_DIR")")")
    local boost_root=""
    if [ -f "${PINOCCHIO_PREFIX}/include/boost/version.hpp" ]; then
        boost_root="${PINOCCHIO_PREFIX}"
    fi
    local pybind11_dir
    pybind11_dir=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())" 2>/dev/null || echo "")
    CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}" \
        BOOST_ROOT="${boost_root}" \
        BOOST_INCLUDEDIR="${boost_root:+${boost_root}/include}" \
        BOOST_LIBRARYDIR="${boost_root:+${boost_root}/lib}" \
        cmake "$WORKSPACE/manyforge_roboplan" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=ON \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" \
        ${pybind11_dir:+-Dpybind11_DIR="$pybind11_dir"} 2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -5
    if ! require_ctest_tests "manyforge_roboplan"; then
        FAIL=$((FAIL + 1))
    elif ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S"; then
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
    reset_stale_python_build_dir "$pybind_build"

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
    setup_pythonpath
    if pytest manyforge_behavior/tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_hmi_tests() {
    section "Python manyforge_hmi tests"
    cd "$WORKSPACE"
    setup_pythonpath
    if pytest manyforge_hmi/tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_integration_tests() {
    section "Integration tests"
    cd "$WORKSPACE"
    setup_pythonpath
    if pytest tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_composer_backend_tests() {
    section "Composer backend tests"
    cd "$WORKSPACE"
    setup_pythonpath
    if ! python_has_modules fastapi py_trees httpx; then
        skip "Missing Python deps for composer backend tests (need fastapi, py_trees, httpx)"
        return
    fi
    # The full Composer lane runs ALL backend tests, including MCP. `mcp` is a
    # declared image dependency (see Dockerfile); it is intentionally NOT part
    # of the skip gate above, so a missing `mcp` fails the MCP tests loudly with
    # a clear dependency error instead of silently skipping unrelated Composer
    # coverage. A lightweight local lane may skip MCP-specific tests explicitly,
    # but test-all must not.
    #
    # Provide a commit hash for the verification-runner metadata so
    # test_scenario_runner stays meaningful even when the container workspace is
    # not git-aware (uid mismatch / git absent). Honors a pre-set value.
    export MANYFORGE_GIT_COMMIT="${MANYFORGE_GIT_COMMIT:-$(git -c safe.directory='*' -C "$WORKSPACE" rev-parse HEAD 2>/dev/null || true)}"
    if pytest manyforge_composer/tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_composer_frontend_build() {
    section "Composer frontend build"
    local frontend_dir="$WORKSPACE/manyforge_composer/frontend"
    if ! command -v npm >/dev/null 2>&1; then
        skip "npm not available"
        return
    fi
    if [ ! -f "$frontend_dir/package.json" ]; then
        skip "Composer frontend package.json not found"
        return
    fi
    cd "$frontend_dir"
    if npm run build; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_qualification_tests() {
    section "Qualification scenario matrix"
    cd "$WORKSPACE"
    setup_pythonpath
    if ! python_has_modules fastapi py_trees httpx uvicorn yaml; then
        skip "Missing Python deps for qualification runner"
        return
    fi
    chmod +x ./scripts/qualify-scenarios.sh ./scripts/run-scenarios.py
    if ./scripts/qualify-scenarios.sh; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_qualification_native_tests() {
    section "Native live-evaluation qualification matrix"
    cd "$WORKSPACE"
    setup_pythonpath
    export PYTHONPATH="$WORKSPACE/manyforge_roboplan/python:${PYTHONPATH:-}"
    if ! python_has_modules fastapi py_trees httpx uvicorn yaml manyforge_roboplan_py native_kernel_client; then
        skip "Missing Python deps for native qualification runner (need manyforge_roboplan_py, native_kernel_client)"
        return
    fi
    chmod +x ./scripts/qualify-native-scenarios.sh ./scripts/run-scenarios.py
    if ./scripts/qualify-native-scenarios.sh; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_qualification_soak_tests() {
    section "Soak qualification matrix"
    cd "$WORKSPACE"
    setup_pythonpath
    if ! python_has_modules fastapi py_trees httpx uvicorn yaml; then
        skip "Missing Python deps for soak qualification runner"
        return
    fi
    chmod +x ./scripts/qualify-soak-scenarios.sh ./scripts/run-scenarios.py
    if ./scripts/qualify-soak-scenarios.sh; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_ros_tests() {
    section "ROS adapter tests"
    cd "$WORKSPACE"
    setup_pythonpath
    export PYTHONPATH="$WORKSPACE/manyforge_ros:${PYTHONPATH:-}"
    if ! python_has_modules rclpy control_msgs trajectory_msgs action_msgs; then
        skip "Missing ROS 2 Python deps; run in the ros-jazzy image"
        return
    fi
    if pytest manyforge_ros/tests/ -v; then
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
    composer)
        run_composer_backend_tests
        run_composer_frontend_build
        ;;
    qualification)
        run_qualification_tests
        ;;
    qualification-native)
        run_qualification_native_tests
        ;;
    qualification-soak)
        run_qualification_soak_tests
        ;;
    ros)
        run_ros_tests
        ;;
    qualification-all)
        run_qualification_tests
        run_qualification_native_tests
        run_qualification_soak_tests
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
        run_composer_backend_tests
        run_composer_frontend_build
        run_qualification_tests
        run_qualification_native_tests
        ;;
    *)
        echo "Usage: $0 [all|cpp|roboplan|bindings|python|composer|qualification|qualification-native|qualification-soak|qualification-all|integration|ros]"
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
