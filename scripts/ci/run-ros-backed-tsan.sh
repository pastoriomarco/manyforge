#!/usr/bin/env bash
# OD-25: four complete ROS-backed binaries, one process per binary.
# Run only inside the repository's full ROS image; no TSan suppressions.
set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
ROS_DISTRO="${ROS_DISTRO:-lyrical}"
BUILD_DIR="${MANYFORGE_ROS_TSAN_BUILD_DIR:-/tmp/manyforge-ros-tsan}"
RESULTS_DIR="${MANYFORGE_ROS_TSAN_RESULTS_DIR:-/tmp/manyforge-ros-tsan-results}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
TIMEOUT_SECONDS="${MANYFORGE_ROS_TSAN_TIMEOUT_SECONDS:-1800}"
[[ "$PARALLEL" =~ ^[1-9][0-9]*$ && "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || {
  echo 'TSan parallelism and timeout must be positive integers' >&2
  exit 2
}

# Same image overlays used by the existing ROS integration graph.
set +u
source "/opt/ros/${ROS_DISTRO}/setup.bash"
source /opt/manyforge/topic_based_ros2_control/install/setup.bash
source /opt/manyforge/xarm_description/install/setup.bash
source /opt/manyforge/xarm_ws/install/local_setup.bash
# The apt ROS base only; the source-built base has no overlays.
for overlay in /opt/manyforge/control_overlay /opt/manyforge/rmw_fastrtps_overlay; do
    if [ -f "${overlay}/local_setup.bash" ]; then source "${overlay}/local_setup.bash"; fi
done
source /opt/manyforge/venv/bin/activate
set -u

# Never inherit a user's filter, sharding, or suppressions. Discovery is
# deferred so the build itself does not execute instrumented test programs.
unset TSAN_OPTIONS GTEST_FILTER GTEST_TOTAL_SHARDS GTEST_SHARD_INDEX GTEST_REPEAT
if [[ -e "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "TSan build directory already contains a CMake graph: $BUILD_DIR" >&2
  exit 2
fi
mkdir -p "$BUILD_DIR" "$RESULTS_DIR"
cmake -S "$WORKSPACE/scripts/ros/ros-integration" -B "$BUILD_DIR" -G Ninja \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=thread'
cmake --build "$BUILD_DIR" --parallel "$PARALLEL" --target \
  test_ros2_control_execution_backend \
  test_ros2_gripper_command_backend \
  test_ros2_control_kernel_stop_integration \
  test_retained_ros2_control_runtime_handle

isolator="/opt/ros/${ROS_DISTRO}/lib/rmw_test_fixture_implementation/run_rmw_isolated"
test -x "$isolator"
binaries=(
  adapter/test_ros2_control_execution_backend
  adapter/test_ros2_gripper_command_backend
  roboplan/test_ros2_control_kernel_stop_integration
  roboplan/test_retained_ros2_control_runtime_handle
)
status=0
for relative in "${binaries[@]}"; do
  binary="$BUILD_DIR/$relative"
  name="${relative##*/}"
  result="$RESULTS_DIR/$name"
  if [[ -e "$result" ]]; then
    echo "TSan result directory already exists: $result" >&2
    exit 2
  fi
  mkdir "$result"
  test -x "$binary"
  # Preserve every unsuppressed report, including reports from subprocesses.
  # GTest XML and command status independently detect test failures;
  # exitcode=0 lets the OD-47 parser classify SDK-only reports.
  export TSAN_OPTIONS="halt_on_error=0:exitcode=0:second_deadlock_stack=1:history_size=7:log_path=$result/tsan"
  set +e
  ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST timeout --signal=TERM --kill-after=10 \
    "$TIMEOUT_SECONDS" "$isolator" "$binary" --gtest_output="xml:$result/gtest.xml" \
    >"$result/console.log" 2>&1
  run_status=$?
  set -e
  cat "$result/console.log"
  if (( run_status != 0 )); then
    printf '%s failed with status %s\n' "$name" "$run_status" >&2
    status=1
  fi
  if ! python3 - "$result/gtest.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET
try:
    root = ET.parse(sys.argv[1]).getroot()
    tests = int(root.attrib.get("tests", "0"))
    failures = int(root.attrib.get("failures", "0"))
    errors = int(root.attrib.get("errors", "0"))
    disabled = int(root.attrib.get("disabled", "0"))
    skipped = int(root.attrib.get("skipped", "0"))
    cases = list(root.iter("testcase"))
    executed = sum(case.attrib.get("status", "run") == "run" and
                   case.find("skipped") is None for case in cases)
    print(f"GTest: tests={tests} executed={executed} disabled={disabled} "
          f"skipped={skipped} failures={failures} errors={errors}")
    if tests == 0 or executed == 0 or failures or errors:
        raise ValueError("no executed tests or a test failure")
except (OSError, ET.ParseError, ValueError) as error:
    print(f"GTest XML did not prove a successful nonempty run: {error}", file=sys.stderr)
    sys.exit(1)
PY
  then status=1; fi
  logs=("$result/console.log")
  shopt -s nullglob
  logs+=("$result"/tsan.*)
  shopt -u nullglob
  set +e
  python3 "$WORKSPACE/scripts/ci/classify-ros-backed-tsan.py" "${logs[@]}" \
    | tee "$result/classification.txt"
  classification_status=("${PIPESTATUS[@]}")
  set -e
  if (( classification_status[0] != 0 || classification_status[1] != 0 )); then
    status=1
  fi
done
exit "$status"
