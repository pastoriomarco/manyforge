#!/usr/bin/env bash
# Run an explicitly selected ManyForge test suite.
# Intended to run inside the Docker dev container.
#
# Usage (from /workspace):
#   ./scripts/test-all.sh retained # current reduced aggregate
#   ./scripts/test-all.sh cpp      # retained C++ only
#   ./scripts/test-all.sh headless # qualified I5/H0 owner and client surfaces
#   ./scripts/test-all.sh python   # retained Python only
#   ./scripts/test-all.sh bindings # ROS-free I2 compiler bindings

set -euo pipefail

# Resolve the helper with Bash parameter expansion so the legacy aggregate
# gate below runs before even a utility subprocess such as `dirname` or
# `nproc`.  Sourcing this helper only defines fixed values and one function.
SCRIPT_DIR="${BASH_SOURCE[0]%/*}"
if [[ "$SCRIPT_DIR" == "${BASH_SOURCE[0]}" ]]; then
    SCRIPT_DIR="."
fi
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "$SCRIPT_DIR/lib/composable-cell-runtime-availability.sh"

# No argument and literal `all` selected the historical product-wide matrix,
# including bindings, integration, runtime qualification, and native lanes.
# Reusing either spelling for today's smaller retained subset would report a
# false green.  Reject both before build defaults, imports, or suite runners.
if (( $# == 0 )); then
    manyforge_fail_composable_cell_runtime_unavailable
fi
if (( $# > 1 )); then
    echo "test-all.sh accepts exactly one suite name" >&2
    exit 2
fi
if [[ "$1" == "all" ]]; then
    manyforge_fail_composable_cell_runtime_unavailable
fi
FILTER="$1"

WORKSPACE="${WORKSPACE:-/workspace}"
export PYTHONPYCACHEPREFIX="${PYTHONPYCACHEPREFIX:-/tmp/manyforge-pycache}"
# The canonical `docker compose run --rm dev ...` lane does not mount a
# persistent build volume. Its container-local default therefore cannot mix
# CMake state from different dev images. Runtime launchers that intentionally
# retain build products pass an explicit image-bound MANYFORGE_BUILD_DIR.
BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"
CTEST_TEST_TIMEOUT_S="${CTEST_TEST_TIMEOUT_S:-60}"
BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
BUILD_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
case "$BUILD_GENERATOR" in
    Ninja|"Unix Makefiles") ;;
    *) echo "Unsupported test build generator: $BUILD_GENERATOR (use Ninja or Unix Makefiles)" >&2; exit 2 ;;
esac
if [[ ! "${BUILD_PARALLEL_LEVEL}" =~ ^[1-9][0-9]*$ ]]; then
    echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
    exit 2
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

section() { echo -e "\n${GREEN}=== $1 ===${NC}"; }
skip()    { echo -e "${YELLOW}  SKIP: $1${NC}"; }

require_ctest_tests() {
    local suite="$1"
    shift
    local listing
    listing="$(ctest -N)"
    echo "$listing" | tail -3
    if echo "$listing" | grep -q "Total Tests: 0"; then
        echo -e "${RED}  FAIL: ${suite} configured zero CTest tests${NC}"
        return 1
    fi
    local required_test
    for required_test in "$@"; do
        if ! grep -Fq "$required_test" <<<"$listing"; then
            echo -e "${RED}  FAIL: ${suite} omitted required ${required_test} test${NC}"
            return 1
        fi
    done
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

# Read the configured tree, not the operator's shell, before passing native
# keep-going flags. CMake itself rejects a different -G for an existing cache.
set_build_keep_going_args() {
    local build="$1"
    local generator
    generator=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$build/CMakeCache.txt")
    case "$generator" in
        Ninja) BUILD_KEEP_GOING_ARGS=(-k 0) ;;
        "Unix Makefiles") BUILD_KEEP_GOING_ARGS=(-k) ;;
        *) echo "Unsupported or missing configured generator in $build/CMakeCache.txt" >&2; return 1 ;;
    esac
}

build_manyforge_core() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_core" -G "$BUILD_GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING="$build_testing" \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

build_manyforge_content_identity() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_content_identity" -G "$BUILD_GENERATOR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DMANYFORGE_CONTENT_IDENTITY_BUILD_TESTING="$build_testing" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

build_manyforge_persistence() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_persistence" -G "$BUILD_GENERATOR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DMANYFORGE_PERSISTENCE_BUILD_TESTING="$build_testing" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

build_manyforge_scene() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_scene" -G "$BUILD_GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING="$build_testing" \
        -DMANYFORGE_SCENE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

configure_manyforge_deployment() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_deployment" -G "$BUILD_GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING="$build_testing" \
        -DMANYFORGE_CORE_INCLUDE_DIR="$WORKSPACE/manyforge_core/include" \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
}

build_manyforge_deployment() {
    local build="$1"
    local build_testing="$2"
    configure_manyforge_deployment "$build" "$build_testing"
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

build_manyforge_component_manager() {
    local build="$1"
    local build_testing="$2"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_component_manager" -G "$BUILD_GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING="$build_testing" \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

build_manyforge_headless() {
    local build="$1"
    local ros_execution_host="$2"
    local pybind11_cmake_dir
    pybind11_cmake_dir="$(
        python3 -c 'import pybind11; print(pybind11.get_cmake_dir())'
    )"
    mkdir -p "$build"
    cd "$build"
    cmake "$WORKSPACE/manyforge_headless" -G "$BUILD_GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=ON \
        -DMANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL=ON \
        -DMANYFORGE_HEADLESS_BUILD_PYTHON=ON \
        -Dpybind11_DIR="$pybind11_cmake_dir" \
        -DMANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST="$ros_execution_host" \
        -DMANYFORGE_HEADLESS_TEST_ROS_SETUP="$BUILD_DIR/isaac/ros-overlay/install/setup.bash" \
        -DMANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" 2>&1 | tail -3
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" 2>&1 | tail -5
}

# Python Program validation, the runner and Composer evaluate expressions
# through the one C++ engine binding; build just that module when the lane
# has not built the headless tree yet, and put it on PYTHONPATH.
setup_expression_binding() {
    local build="$BUILD_DIR/headless"
    local pybind11_cmake_dir
    if [[ ! -f "$build/CMakeCache.txt" ]]; then
        pybind11_cmake_dir="$(
            python3 -c 'import pybind11; print(pybind11.get_cmake_dir())'
        )"
        cmake -S "$WORKSPACE/manyforge_headless" -B "$build" -G "$BUILD_GENERATOR" \
            -DCMAKE_BUILD_TYPE=Release \
            -DMANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL=ON \
            -DMANYFORGE_HEADLESS_BUILD_PYTHON=ON \
            -Dpybind11_DIR="$pybind11_cmake_dir" 2>&1 | tail -3
    fi
    cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" \
        --target manyforge_headless_py 2>&1 | tail -3
    export PYTHONPATH="$build:${PYTHONPATH:-}"
    python_has_modules manyforge_headless_py
}

run_cpp_tests() {
    section "C++ manyforge_content_identity tests"
    local identity_build="$BUILD_DIR/content-identity"
    build_manyforge_content_identity "$identity_build" ON
    cd "$identity_build"
    if ! require_ctest_tests "manyforge_content_identity"; then
        FAIL=$((FAIL + 1))
    elif ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi

    section "C++ manyforge_persistence tests"
    local persistence_build="$BUILD_DIR/persistence"
    build_manyforge_persistence "$persistence_build" ON
    cd "$persistence_build"
    if ! require_ctest_tests "manyforge_persistence"; then
        FAIL=$((FAIL + 1))
    elif ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi

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

    section "C++ manyforge_scene tests"
    local scene_build="$BUILD_DIR/scene"
    build_manyforge_scene "$scene_build" ON
    cd "$scene_build"
    if ! require_ctest_tests "manyforge_scene"; then
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
    if ! setup_expression_binding; then
        echo -e "${RED}  FAIL: the expression engine binding (manyforge_headless_py) is unavailable${NC}"
        FAIL=$((FAIL + 1))
    elif ! require_ctest_tests "manyforge_deployment"; then
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

ROBOPLAN_CONFIGURED_BUILD=""
ROBOPLAN_HEADLESS_PRODUCER_BUILD=""

configure_roboplan_build() {
    local build="$BUILD_DIR/roboplan"
    if [[ "$ROBOPLAN_CONFIGURED_BUILD" == "$build" ]]; then
        return
    fi

    source_ros_integration_environment
    local pinocchio_cmake_dir
    pinocchio_cmake_dir="$(
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
    )" || {
        echo -e "${RED}  FAIL: Pinocchio CMake package is required for the RoboPlan producer closure${NC}"
        return 1
    }
    if [[ -z "$pinocchio_cmake_dir" ]]; then
        echo -e "${RED}  FAIL: Pinocchio CMake package is required for the RoboPlan producer closure${NC}"
        return 1
    fi

    local pinocchio_prefix
    pinocchio_prefix=$(dirname "$(dirname "$(dirname "$pinocchio_cmake_dir")")")
    local boost_root=""
    if [[ -f "${pinocchio_prefix}/include/boost/version.hpp" ]]; then
        boost_root="$pinocchio_prefix"
    fi
    mkdir -p "$build/.cmake/api/v1/query"
    printf '{}\n' > "$build/.cmake/api/v1/query/codemodel-v2"
    printf '{}\n' > "$build/.cmake/api/v1/query/cmakeFiles-v1"
    printf '{}\n' > "$build/.cmake/api/v1/query/toolchains-v1"
    cd "$build"
    if ! CMAKE_PREFIX_PATH="/usr/local:${pinocchio_prefix}:${CMAKE_PREFIX_PATH:-}" \
        BOOST_ROOT="$boost_root" \
        BOOST_INCLUDEDIR="${boost_root:+${boost_root}/include}" \
        BOOST_LIBRARYDIR="${boost_root:+${boost_root}/lib}" \
        cmake "$WORKSPACE/manyforge_roboplan" -G "$BUILD_GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=ON \
        -DMANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=ON \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S"; then
        echo -e "${RED}  FAIL: RoboPlan producer configure failed${NC}"
        return 1
    fi
    ROBOPLAN_CONFIGURED_BUILD="$build"
}

build_headless_roboplan_producer_closure() {
    local build="$BUILD_DIR/roboplan"
    if [[ "$ROBOPLAN_HEADLESS_PRODUCER_BUILD" == "$build" ]]; then
        return
    fi
    if ! MANYFORGE_ROS2_CONTROL_BUILD_TESTING=ON \
        MANYFORGE_ROS2_CONTROL_BUILD_ROOT="$BUILD_DIR/isaac" \
        MANYFORGE_ROBOPLAN_BUILD_DIR="$build" \
        CMAKE_GENERATOR="$BUILD_GENERATOR" \
        MANYFORGE_ROS2_CONTROL_BUILD_KEEP_GOING=ON \
        CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_PARALLEL_LEVEL" \
        bash "$WORKSPACE/scripts/ros/build-ros2-control-runtime.sh" \
            --target manyforge_ros2_control_execution_host \
            --target manyforge_ros2_control_execution_host_pick_place_planning_fixture \
            --target manyforge_ros2_control_execution_host_projection_fixture \
            --target manyforge_roboplan_candidate_worker \
            --target fake_process_candidate_worker \
            --topic-install "$ROS_INTEGRATION_TOPIC_INSTALL" \
            --data-overlay "$ROS_INTEGRATION_DATA_OVERLAY"; then
        echo -e "${RED}  FAIL: ROS producer closure build failed${NC}"
        return 1
    fi
    ROBOPLAN_CONFIGURED_BUILD="$build"
    source_ros_integration_environment
    set +u
    # shellcheck disable=SC1090
    source "$BUILD_DIR/isaac/ros-overlay/install/setup.bash"
    set -u

    local producer_input
    for producer_input in \
        "$build/manyforge_ros2_control_execution_host" \
        "$build/manyforge_ros2_control_execution_host_pick_place_planning_fixture" \
        "$build/ros_projection_host_fixture/manyforge_ros2_control_execution_host" \
        "$build/manyforge_roboplan_candidate_worker"; do
        if [[ ! -f "$producer_input" ]]; then
            echo -e "${RED}  FAIL: RoboPlan producer closure did not create ${producer_input}${NC}"
            return 1
        fi
    done
    ROBOPLAN_HEADLESS_PRODUCER_BUILD="$build"
}

require_headless_runtime_packages() {
    local build="$1"
    local package_metadata
    for package_metadata in \
        "$build/manager-apply-test-fixture/metadata.json" \
        "$build/manager-apply-o1-4g-replacement-fixture/metadata.json" \
        "$build/assistant-native-test-fixture/metadata.json"; do
        if [[ ! -f "$package_metadata" ]]; then
            echo -e "${RED}  FAIL: Headless runtime package was not generated: ${package_metadata}${NC}"
            return 1
        fi
    done
    return 0
}

prepare_ui6i_cell_bootstrap() {
    local build="$BUILD_DIR/deployment"
    configure_manyforge_deployment "$build" ON
    set_build_keep_going_args "$build" || return 1
    if ! cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" \
        --target manyforge_ui6i_cell_bootstrap manyforge_prepare_activation -- "${BUILD_KEEP_GOING_ARGS[@]}"; then
        echo -e "${RED}  FAIL: UI6-I cell bootstrap generation failed${NC}"
        return 1
    fi
    local package="$build/ui6i-dual-scenario-fixture/retained-package"
    local admission="$package/canonical-empty-admission.ui6i_ur5e_lite6_wall_cell.json"
    if [[ ! -d "$package" || ! -f "$admission" ]]; then
        echo -e "${RED}  FAIL: UI6-I cell bootstrap is incomplete${NC}"
        return 1
    fi
}

run_roboplan_tests() {
    section "C++ retained RoboPlan product-consumer tests"
    local build="$BUILD_DIR/roboplan"
    if ! configure_roboplan_build; then
        FAIL=$((FAIL + 1))
        return
    fi
    if ! set_build_keep_going_args "$build"; then
        FAIL=$((FAIL + 1))
        return
    fi
    if ! cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" -- "${BUILD_KEEP_GOING_ARGS[@]}"; then
        echo -e "${RED}  FAIL: RoboPlan producer build failed${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    cd "$build"
    # The execution host loads its adapter families from the build tree's
    # ament prefix (CONTRACTS §6).
    if ! require_ctest_tests "manyforge_roboplan"; then
        FAIL=$((FAIL + 1))
    elif AMENT_PREFIX_PATH="$build/ament_cmake_index:${AMENT_PREFIX_PATH:-}" \
        ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi

}

run_headless_cpp_tests() {
    section "C++ I5/H0 headless owner tests"
    local build="$BUILD_DIR/headless"
    local ros_execution_host="$BUILD_DIR/roboplan/manyforge_ros2_control_execution_host"
    build_headless_roboplan_producer_closure
    build_manyforge_headless "$build" "$ros_execution_host"
    cd "$build"
    if ! require_ctest_tests "manyforge_headless" \
        "ManagerDaemonProcess.CompatibleNativeReplacementUsesFreshEpochAfterCleanStop" \
        "ManagerDaemonProcess.NamedApplyUsesTheRealManagedFenceAndSurvivesReplacement" \
        "ManagerDaemonProcess.PhysicalCompositionStaysClosedAfterUnexpectedChildLossAndManagerReplacement"; then
        FAIL=$((FAIL + 1))
    elif ! require_headless_runtime_packages "$build"; then
        FAIL=$((FAIL + 1))
    elif AMENT_PREFIX_PATH="$BUILD_DIR/roboplan/ament_cmake_index:${AMENT_PREFIX_PATH:-}" \
        ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_headless_python_tests() {
    section "Python I5/H0 API, CLI, and process-boundary tests"
    local build="$BUILD_DIR/headless"
    if ! prepare_ui6i_cell_bootstrap; then
        FAIL=$((FAIL + 1))
        return
    fi
    cd "$WORKSPACE"
    setup_pythonpath
    export PYTHONPATH="$build:$PYTHONPATH"
    if ! python_has_modules \
        fastapi \
        httpx \
        pydantic \
        uvicorn \
        manyforge_headless_py \
        manyforge_program_runner; then
        echo -e "${RED}  FAIL: missing I5/H0 Python or native binding dependencies${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    # The UI6-I cell is a ros2_control mock cell: its process tests need the
    # ROS image and run in the `ros-e2e` suite, never here.
    if AMENT_PREFIX_PATH="$BUILD_DIR/roboplan/ament_cmake_index:${AMENT_PREFIX_PATH:-}" \
        MANYFORGE_MANAGER_DAEMON="$build/manyforge_manager_daemon" \
        MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT="$build/canonical-empty-test-package" \
        MANYFORGE_ASSISTANT_NATIVE_PACKAGE_ROOT="$build/assistant-native-test-fixture/retained-package" \
        MANYFORGE_ASSISTANT_NATIVE_METADATA="$build/assistant-native-test-fixture/metadata.json" \
        MANYFORGE_ROBOTIQ_POSITION_PACKAGE_ROOT="$build/robotiq-position-manager-test-fixture/retained-package" \
        MANYFORGE_ROBOTIQ_POSITION_METADATA="$build/robotiq-position-manager-test-fixture/metadata.json" \
        MANYFORGE_MANAGER_APPLY_PACKAGE_ROOT="$build/manager-apply-test-fixture/retained-package" \
        MANYFORGE_MANAGER_APPLY_METADATA="$build/manager-apply-test-fixture/metadata.json" \
        MANYFORGE_UI6I_CELL_BOOTSTRAP_ROOT="$BUILD_DIR/deployment/ui6i-dual-scenario-fixture/retained-package" \
        MANYFORGE_ACTIVATION_PREPARER="$BUILD_DIR/deployment/manyforge_prepare_activation" \
        MANYFORGE_EXAMPLE_SCENARIO_ROOT="$WORKSPACE/examples" \
        MANYFORGE_ROS2_CONTROL_EXECUTION_HOST="$BUILD_DIR/roboplan/manyforge_ros2_control_execution_host" \
        MANYFORGE_ROBOPLAN_CANDIDATE_WORKER="$BUILD_DIR/roboplan/manyforge_roboplan_candidate_worker" \
        MANYFORGE_ROBOPLAN_FAKE_WORKER="$BUILD_DIR/roboplan/fake_process_candidate_worker" \
        pytest \
            manyforge_headless_api/tests/ \
            manyforge_headless_api/process_tests/ \
            manyforge_program_runner/tests/ \
            "${UI6I_ROS_PROCESS_TESTS[@]/#/--ignore=}" \
            -p no:cacheprovider \
            -p no:launch_testing -p no:launch_ros \
            -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_headless_tests() {
    run_headless_cpp_tests
    run_headless_python_tests
}

run_pybind_tests() {
    section "ROS-free pybind11 compiler bindings"
    local stage_root="${MANYFORGE_BINDINGS_STAGE_ROOT:-${BUILD_DIR}-bindings-stage}"
    local generated_root="${MANYFORGE_BINDINGS_GENERATED_ROOT:-${BUILD_DIR}-bindings-generated}"
    local prepare_only="${MANYFORGE_BINDINGS_PREPARE_ONLY:-0}"
    local arguments=(
        release
        --build-root "$BUILD_DIR"
        --stage-root "$stage_root"
        --generated-output-root "$generated_root"
    )
    if [[ "$prepare_only" == "1" ]]; then
        arguments+=(--prepare-only)
    elif [[ "$prepare_only" != "0" ]]; then
        echo -e "${RED}  FAIL: MANYFORGE_BINDINGS_PREPARE_ONLY must be 0 or 1${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    cd "$WORKSPACE"
    if "$WORKSPACE/scripts/ci/run-core-bindings.sh" "${arguments[@]}"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_python_tests() {
    section "Retained Python behavior and observation tests"
    cd "$WORKSPACE"
    setup_pythonpath
    if ! setup_expression_binding; then
        echo -e "${RED}  FAIL: the expression engine binding (manyforge_headless_py) is unavailable${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    if pytest \
        manyforge_behavior/tests/ \
        manyforge_operator/tests/ \
        scripts/qualification/tests/ \
        tests/composable_cell_i4/ \
        tests/test_benchmark.py \
        tests/test_public_contract_entrypoint_reconciliation.py \
        tests/test_regen.py \
        tests/test_qualification_wrapper_entrypoints.py \
        tests/test_ros_qualification_helpers.py \
        tests/test_grouped_gtest_runner.py \
        tests/test_release_golden_workflow.py \
        tests/test_test_all_script_contract.py \
        -p no:launch_testing -p no:launch_ros \
        -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_observability_client_tests() {
    section "Retained diagnostics and Medkit client tests"
    cd "$WORKSPACE"
    setup_pythonpath
    export PYTHONPATH="$WORKSPACE/manyforge_ros:$WORKSPACE/manyforge_medkit_adapter:$PYTHONPATH"
    if pytest \
        manyforge_ros/tests/test_diagnostics_bridge.py \
        manyforge_medkit_adapter/tests/test_forwarder.py \
        -k 'not statuses_to_msg_builds_diagnostic_array' \
        -v; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_integration_tests() {
    section "Integration tests"
    manyforge_fail_composable_cell_runtime_unavailable
}

run_bridge_tests() {
    section "Assistant H0 read client and retained protocol tests"
    cd "$WORKSPACE"
    setup_pythonpath
    if ! python_has_modules fastapi httpx; then
        echo -e "${RED}  FAIL: missing Python deps for retained assistant tests (need fastapi, httpx) — run in the dev container${NC}"
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
    # The retained shell is intentionally structural. A missing declared
    # dependency is a hard lane failure, never a skipped runtime test.
    if ! python_has_modules fastapi httpx yaml; then
        echo -e "${RED}  FAIL: missing Python deps for Composer structural-shell tests — run in the dev container${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    if ! setup_expression_binding; then
        echo -e "${RED}  FAIL: the expression engine binding (manyforge_headless_py) is unavailable${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    # The launch plugins install a non-propagating logger class, which hides
    # Composer log records from caplog.
    if pytest manyforge_composer/tests/ -p no:launch_testing -p no:launch_ros -v; then
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
    section "Composer structural frontend unit tests"
    local frontend_dir="$WORKSPACE/manyforge_composer/frontend"
    if ! command -v npm >/dev/null 2>&1; then
        echo -e "${RED}  FAIL: npm is required for the retained structural frontend tests${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ ! -f "$frontend_dir/package.json" ]; then
        echo -e "${RED}  FAIL: Composer frontend package.json not found${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    cd "$frontend_dir"
    if npm test; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_qualification_tests() {
    section "Scenario qualification"
    manyforge_fail_composable_cell_runtime_unavailable
}

run_deployment_composition_tests() {
    section "Deployment Stop-plan composition"
    local build="$BUILD_DIR/ua7wl-deployment-qualification"
    cmake -S "$WORKSPACE/verification/ua7wl_deployment" -B "$build" \
        -G "$BUILD_GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -DMANYFORGE_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S"
    cmake --build "$build" \
        --target test_resolved_stop_base_plan_resolver \
        --parallel "$BUILD_PARALLEL_LEVEL"
    cd "$build"
    if ! require_ctest_tests "Deployment composition" \
        "ResolvedStopBasePlanResolverTest"; then
        FAIL=$((FAIL + 1))
    elif ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S" \
        -R 'ResolvedStopBasePlanResolverTest'; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_qualification_soak_tests() {
    section "Soak qualification"
    manyforge_fail_composable_cell_runtime_unavailable
}

# The ROS image's environment the Q9 build helper selects: ROS, the pinned
# stock topic_based_ros2_control install, the xarm_description data overlay and
# the application venv. Re-entering a configured tree needs the same one.
ROS_INTEGRATION_TOPIC_INSTALL=/opt/manyforge/topic_based_ros2_control/install
ROS_INTEGRATION_DATA_OVERLAY=/opt/manyforge/xarm_description/install

source_ros_integration_environment() {
    set +u
    # shellcheck disable=SC1090
    source "/opt/ros/${ROS_DISTRO:-lyrical}/setup.bash"
    # shellcheck disable=SC1091
    source "$ROS_INTEGRATION_TOPIC_INSTALL/setup.bash"
    # shellcheck disable=SC1091
    source "$ROS_INTEGRATION_DATA_OVERLAY/setup.bash"
    if [[ -d "${MANYFORGE_VENV:-/opt/manyforge/venv}" ]]; then
        # shellcheck disable=SC1091
        source "${MANYFORGE_VENV:-/opt/manyforge/venv}/bin/activate"
    fi
    set -u
}

# Q9: one ROS integration graph (Core, deployment, the JTC adapter and gripper,
# the ROS execution host) built by the ROS image's helper, then its CTest.
run_ros_tests() {
    section "ROS integration graph (Core, deployment, JTC adapter, ROS host)"
    local build_root="$BUILD_DIR/ros-integration"
    local build="$build_root/roboplan"
    local helper_status=0
    MANYFORGE_ROS2_CONTROL_BUILD_TESTING=ON \
    MANYFORGE_ROS2_CONTROL_BUILD_ROOT="$build_root" \
    CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_PARALLEL_LEVEL" \
        bash "$WORKSPACE/scripts/ros/build-ros2-control-runtime.sh" \
            --source-dir "$WORKSPACE/scripts/ros/ros-integration" --target all \
            --topic-install "$ROS_INTEGRATION_TOPIC_INSTALL" \
            --data-overlay "$ROS_INTEGRATION_DATA_OVERLAY" || helper_status=$?
    if (( helper_status != 0 )); then
        # Report every failing target of the configured tree, not only the first.
        if [[ -f "$build/CMakeCache.txt" ]] && set_build_keep_going_args "$build"; then
            ( source_ros_integration_environment
              cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" \
                  -- "${BUILD_KEEP_GOING_ARGS[@]}" ) || true
        fi
        echo -e "${RED}  FAIL: ROS integration build failed${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    cd "$build"
    if ! setup_expression_binding; then
        echo -e "${RED}  FAIL: the expression engine binding (manyforge_headless_py) is unavailable${NC}"
        FAIL=$((FAIL + 1))
    elif ! ( source_ros_integration_environment
           require_ctest_tests "ROS integration graph" \
               "JtcExecutionPort" "Ros2GripperCommandBackendTest" \
               "RetainedRos2ControlBackendFactoryTest" \
               "RetainedRos2ControlRuntimeHandleTest" \
               "test_xarm_robot_states_transport" "test_xarm_driver_services" \
               "Ros2ControlKernelStopIntegrationTest" ); then
        FAIL=$((FAIL + 1))
    elif ( source_ros_integration_environment
           ctest --output-on-failure --timeout "$CTEST_TEST_TIMEOUT_S" ); then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

# UI6-I process tests whose cell is the ros2_control mock lane. They need the
# ROS image: Manager beside the real ROS execution host, which starts its
# Manager-owned mock controller launches from the sourced bringup overlay.
UI6I_ROS_PROCESS_TESTS=(
    manyforge_headless_api/process_tests/test_ui6i_grouped_execution.py
    manyforge_headless_api/process_tests/test_operator_alarm_motion_attention.py
    manyforge_headless_api/process_tests/test_interrupt_signal_nodes_process.py
)

run_ros_e2e_tests() {
    section "UI6-I mock cell: Manager, ROS execution host, mock ros2_control"
    local build_root="$BUILD_DIR/ros-e2e"
    local ros_build="$build_root/roboplan"
    local overlay="$build_root/ros-overlay/install/setup.bash"
    local headless="$BUILD_DIR/headless"
    local helper_status=0
    # One ROS graph supplies Manager's host, planner worker and the real ROS
    # fixture hosts. Only the named targets are built.
    MANYFORGE_ROS2_CONTROL_BUILD_TESTING=ON \
    MANYFORGE_ROS2_CONTROL_BUILD_ROOT="$build_root" \
    CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_PARALLEL_LEVEL" \
        bash "$WORKSPACE/scripts/ros/build-ros2-control-runtime.sh" \
            --target manyforge_ros2_control_execution_host \
            --target manyforge_roboplan_candidate_worker \
            --target manyforge_ros2_control_execution_host_pick_place_planning_fixture \
            --target manyforge_ros2_control_execution_host_projection_fixture \
            --topic-install "$ROS_INTEGRATION_TOPIC_INSTALL" \
            --data-overlay "$ROS_INTEGRATION_DATA_OVERLAY" || helper_status=$?
    if (( helper_status != 0 )); then
        echo -e "${RED}  FAIL: ROS execution host build failed${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    # Same configuration as build_manyforge_headless, with its full output
    # kept: this lane's evidence must show any configure or build error.
    local pybind11_cmake_dir
    pybind11_cmake_dir="$(python3 -c 'import pybind11; print(pybind11.get_cmake_dir())')"
    if ! cmake -S "$WORKSPACE/manyforge_headless" -B "$headless" -G "$BUILD_GENERATOR" \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
            -DMANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL=ON \
                -DMANYFORGE_HEADLESS_BUILD_PYTHON=ON -Dpybind11_DIR="$pybind11_cmake_dir" \
            -DMANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST="$ros_build/manyforge_ros2_control_execution_host" \
            -DMANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS="$CTEST_TEST_TIMEOUT_S" \
        || ! cmake --build "$headless" --parallel "$BUILD_PARALLEL_LEVEL"; then
        echo -e "${RED}  FAIL: Headless build failed${NC}"
        FAIL=$((FAIL + 1))
        return
    fi
    if ! prepare_ui6i_cell_bootstrap; then
        FAIL=$((FAIL + 1))
        return
    fi
    # A failing test copies its stack's logs and controller-launch files here.
    # One fresh folder per run; earlier runs' evidence is kept, not deleted.
    local evidence
    evidence="$BUILD_DIR/ros-e2e-evidence/$(date -u +%Y%m%dT%H%M%SZ)-$$"
    mkdir -p "$evidence"
    cd "$WORKSPACE"
    if ( source_ros_integration_environment
         set +u
         # shellcheck disable=SC1090
         source "$overlay"
         set -u
         # Resolve using the actual sourced ament environment inherited by
         # Manager/host/ros2 launch; keep exact input path/hash in this run.
         python3 - "$WORKSPACE" "$evidence/controller-launch-input.json" <<'PY_LAUNCH' || exit 1
import hashlib
import json
import sys
from pathlib import Path
from ament_index_python.packages import get_package_share_directory
share = Path(get_package_share_directory("manyforge_ros_bringup"))
launch = share / "launch" / "ros2_control.launch.py"
source = Path(sys.argv[1]) / "manyforge_ros_bringup" / "launch" / "ros2_control.launch.py"
digest = lambda path: "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
record = {"package_share_directory": str(share), "launch_file": str(launch),
          "resolved_launch_file": str(launch.resolve(strict=True)), "launch_sha256": digest(launch),
          "source_launch_file": str(source.resolve(strict=True)), "source_sha256": digest(source)}
Path(sys.argv[2]).write_text(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
print(json.dumps(record, sort_keys=True))
if record["launch_sha256"] != record["source_sha256"]:
    raise SystemExit("Actual sourced Mock launch differs from the current qualified source")
PY_LAUNCH
         export MANYFORGE_MANAGER_ROS_LAUNCH_EVIDENCE="$evidence/controller-launch-input.json"
         # The execution host loads its adapter families from the build
         # tree's ament prefix (CONTRACTS §6).
         export AMENT_PREFIX_PATH="$ros_build/ament_cmake_index:${AMENT_PREFIX_PATH:-}"
         setup_pythonpath
         export PYTHONPATH="$headless:$PYTHONPATH"
         export MANYFORGE_MANAGER_DAEMON="$headless/manyforge_manager_daemon" \
         MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT="$headless/canonical-empty-test-package" \
         MANYFORGE_UI6I_CELL_BOOTSTRAP_ROOT="$BUILD_DIR/deployment/ui6i-dual-scenario-fixture/retained-package" \
         MANYFORGE_ACTIVATION_PREPARER="$BUILD_DIR/deployment/manyforge_prepare_activation" \
         MANYFORGE_EXAMPLE_SCENARIO_ROOT="$WORKSPACE/examples" \
         MANYFORGE_ROBOPLAN_CANDIDATE_WORKER="$ros_build/manyforge_roboplan_candidate_worker" \
         MANYFORGE_ROS2_CONTROL_EXECUTION_HOST="$ros_build/manyforge_ros2_control_execution_host" \
         MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT="$evidence"
         local manager_apply_status=0
         ctest --test-dir "$headless" --output-on-failure --no-tests=error \
             --timeout "$CTEST_TEST_TIMEOUT_S" \
             -R '^ManagerDaemonProcess\.EmptyRosApplyConfiguresInactiveMockThenExplicitActivation$' \
             > "$evidence/empty-ros-apply-ctest.log" 2>&1 || manager_apply_status=$?
         cat "$evidence/empty-ros-apply-ctest.log"
         printf '%s\n' "$manager_apply_status" > "$evidence/empty-ros-apply-ctest.status"
         # The run gets its own ROS domain (Fast DDS #6599, N16).
         ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST \
             "/opt/ros/${ROS_DISTRO:-lyrical}/lib/rmw_test_fixture_implementation/run_rmw_isolated" \
             pytest "${UI6I_ROS_PROCESS_TESTS[@]}" -p no:cacheprovider -v ); then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
    if [[ -f "$evidence/empty-ros-apply-ctest.status" ]] &&
       [[ "$(< "$evidence/empty-ros-apply-ctest.status")" == "0" ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

case "$FILTER" in
    cpp)
        run_cpp_tests
        run_headless_cpp_tests
        ;;
    headless)
        run_headless_tests
        ;;
    roboplan)
        run_roboplan_tests
        ;;
    bindings)
        run_pybind_tests
        ;;
    python)
        run_python_tests
        run_observability_client_tests
        ;;
    composer)
        run_composer_backend_tests
        run_composer_frontend_build
        run_composer_frontend_tests
        ;;
    bridge)
        run_bridge_tests
        ;;
    deployment-composition)
        run_deployment_composition_tests
        ;;
    qualification)
        run_qualification_tests
        ;;
    qualification-soak)
        run_qualification_soak_tests
        ;;
    ros)
        run_ros_tests
        ;;
    ros-e2e)
        run_ros_e2e_tests
        ;;
    qualification-all)
        run_qualification_tests
        run_qualification_soak_tests
        ;;
    integration)
        run_integration_tests
        ;;
    retained)
        # This name is deliberately narrower than the historical `all`: it
        # covers only the currently retained structural/offline surfaces.
        run_cpp_tests
        run_headless_tests
        run_roboplan_tests
        run_python_tests
        run_observability_client_tests
        run_bridge_tests
        run_composer_backend_tests
        run_composer_frontend_build
        run_composer_frontend_tests
        ;;
    *)
        echo "Usage: $0 [retained|cpp|headless|roboplan|bindings|python|composer|bridge|deployment-composition|qualification|qualification-soak|qualification-all|integration|ros|ros-e2e]"
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
