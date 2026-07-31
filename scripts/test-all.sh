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
export PYTHONPYCACHEPREFIX="${PYTHONPYCACHEPREFIX:-/tmp/manyforge-pycache}"
# The canonical `docker compose run --rm dev ...` lane does not mount a
# persistent build volume. Its container-local default therefore cannot mix
# CMake state from different dev images. Runtime launchers that intentionally
# retain build products pass an explicit image-bound MANYFORGE_BUILD_DIR.
BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"
CTEST_TEST_TIMEOUT_S="${CTEST_TEST_TIMEOUT_S:-60}"
BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
if [[ ! "${BUILD_PARALLEL_LEVEL}" =~ ^[1-9][0-9]*$ ]]; then
    echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
    exit 2
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

if (( $# > 1 )); then
    echo "test-all.sh accepts at most one suite name" >&2
    exit 2
fi
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

build_manyforge_core() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_core" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING="$build_testing" \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

build_manyforge_deployment() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_deployment" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING="$build_testing" \
        -DMANYFORGE_CORE_INCLUDE_DIR="$WORKSPACE/manyforge_core/include" \
        -DMANYFORGE_VERIFY_CANONICAL_EMPTY_RELEASE_GOLDEN=ON \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

build_manyforge_component_manager() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_component_manager" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING="$build_testing" \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

run_cpp_tests() {
    section "C++ manyforge_core tests"
    local core_build="$BUILD_DIR/core"
    build_manyforge_core "$core_build" ON
    cd "$core_build"
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

    section "C++ manyforge_deployment tests"
    local deployment_build="$BUILD_DIR/deployment"
    build_manyforge_deployment "$deployment_build" ON
    cd "$deployment_build"
    if ! require_ctest_tests "manyforge_deployment"; then
        FAIL=$((FAIL + 1))
    elif ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi

    section "C++ manyforge_component_manager tests"
    local manager_build="$BUILD_DIR/component-manager"
    build_manyforge_component_manager "$manager_build" ON
    cd "$manager_build"
    if ! require_ctest_tests "manyforge_component_manager"; then
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
    mkdir -p "$build"
    cd "$build"

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
        -DMANYFORGE_ENABLE_ROS2_CONTROL=OFF \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" \
        ${pybind11_dir:+-Dpybind11_DIR="$pybind11_dir"} 2>&1 | tail -3
    make -j"$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
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
    local deployment_build="$BUILD_DIR/deployment"
    local manager_build="$BUILD_DIR/component-manager-production"
    local pybind_build="$BUILD_DIR/pybind"
    # Always run both incremental ROS-free library builds. Merely finding an
    # archive does not prove it was built from the current source tree.
    build_manyforge_core "$core_build" OFF
    build_manyforge_deployment "$deployment_build" OFF
    build_manyforge_component_manager "$manager_build" OFF

    local pybind11_dir
    pybind11_dir=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())" 2>/dev/null || echo "")

    mkdir -p "$pybind_build"
    cd "$pybind_build"
    cmake "$WORKSPACE/manyforge_core/python" \
        -DMANYFORGE_CORE_DIR="$core_build" \
        -DMANYFORGE_CORE_INCLUDE_DIR="$WORKSPACE/manyforge_core/include" \
        -DMANYFORGE_DEPLOYMENT_DIR="$deployment_build" \
        -DMANYFORGE_DEPLOYMENT_INCLUDE_DIR="$WORKSPACE/manyforge_deployment/include" \
        ${pybind11_dir:+-Dpybind11_DIR="$pybind11_dir"} \
        2>&1 | tail -3
    make -j"$BUILD_PARALLEL_LEVEL" 2>&1 | tail -3
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

run_bridge_tests() {
    section "Assistant bridge tests"
    cd "$WORKSPACE"
    setup_pythonpath
    # Hard failure, not a skip: the bridge suite's own importorskip("fastapi")
    # already vanishes silently on dep-less hosts, so a skip here would make
    # the lane green while running zero tests. The dev container has the deps.
    if ! python_has_modules fastapi httpx; then
        echo -e "${RED}  FAIL: missing Python deps for assistant bridge tests (need fastapi, httpx) — run in the dev container${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    if pytest manyforge_assistant_bridge/tests/ -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_composer_backend_tests() {
    section "Composer backend tests"
    cd "$WORKSPACE"
    setup_pythonpath
    # Hard failure, not a skip: many Composer backend tests use
    # pytest.importorskip("fastapi") and related imports, so a dep-less host can
    # otherwise turn this lane green while running only a frontend subset. The
    # dev container has these declared dependencies; local lightweight runs
    # should use a narrower filter instead of shrinking the Composer lane.
    if ! python_has_modules fastapi py_trees httpx; then
        echo -e "${RED}  FAIL: missing Python deps for composer backend tests (need fastapi, py_trees, httpx) — run in the dev container${NC}"
        FAIL=$((FAIL + 1))
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

run_composer_frontend_tests() {
    section "Composer frontend unit tests"
    local frontend_dir="$WORKSPACE/manyforge_composer/frontend"
    if ! command -v npm >/dev/null 2>&1; then
        skip "npm not available"
        return
    fi
    if [ ! -f "$frontend_dir/package.json" ]; then
        skip "Composer frontend package.json not found"
        return
    fi
    # No node_modules guard beyond the ones above: if npm exists but deps are
    # missing, `npm test` must fail loudly rather than skip — a silent skip
    # here would green-light untested frontend logic.
    cd "$frontend_dir"
    if npm test; then
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
    local roboplan_build="$BUILD_DIR/roboplan"
    if ls "$roboplan_build"/manyforge_roboplan_py*.so 1>/dev/null 2>&1; then
        export PYTHONPATH="$roboplan_build:${PYTHONPATH:-}"
    fi
    if ! python_has_modules fastapi py_trees httpx uvicorn yaml manyforge_roboplan_py native_kernel_client; then
        echo -e "${RED}  FAIL: missing Python deps for native qualification runner (need manyforge_roboplan_py, native_kernel_client)${NC}"
        FAIL=$((FAIL + 1))
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
    if [ -f /opt/ros/jazzy/setup.bash ]; then
        # The Isaac/ROS lane does not source ROS for non-interactive shells.
        # Probe and execute against the lane's actual ROS environment rather
        # than reporting a misleading zero-suite success.
        # shellcheck disable=SC1091
        set +u
        source /opt/ros/jazzy/setup.bash
        set -u
    fi
    cd "$WORKSPACE"
    setup_pythonpath
    # These are hermetic adapter/unit tests with in-process fake services. Do
    # not let an active Isaac or hardware graph on domain 0 answer the same
    # standard simulation_interfaces service names nondeterministically.
    export ROS_DOMAIN_ID="${MANYFORGE_TEST_ROS_DOMAIN_ID:-119}"
    export PYTHONPATH="$WORKSPACE/manyforge_ros:${PYTHONPATH:-}"
    local roboplan_build="$BUILD_DIR/roboplan"
    if ls "$roboplan_build"/manyforge_roboplan_py*.so 1>/dev/null 2>&1; then
        export PYTHONPATH="$roboplan_build:$WORKSPACE/manyforge_roboplan/python:${PYTHONPATH:-}"
    fi
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
        ;;
    composer)
        run_composer_backend_tests
        run_composer_frontend_build
        run_composer_frontend_tests
        ;;
    bridge)
        run_bridge_tests
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
        run_integration_tests
        run_bridge_tests
        run_composer_backend_tests
        run_composer_frontend_build
        run_composer_frontend_tests
        run_qualification_tests
        run_qualification_native_tests
        ;;
    *)
        echo "Usage: $0 [all|cpp|roboplan|bindings|python|composer|bridge|qualification|qualification-native|qualification-soak|qualification-all|integration|ros]"
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
