#!/usr/bin/env bash
# Build and test the fake xArm lifecycle boundary with ASan and UBSan.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_SANITIZER_BUILD_ROOT:-/tmp/manyforge-build/real-lite6-asan-ubsan}
export MANYFORGE_REAL_LITE6_BUILD_ROOT="${BUILD_ROOT}"

MANYFORGE_REAL_LITE6_BUILD_PROFILE=asan-ubsan \
    "${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

FIXTURE_ROOT=$(mktemp -d /tmp/manyforge-xarm-binding-fixture.XXXXXX)
trap 'rm -r -- "${FIXTURE_ROOT}"' EXIT
"${WORKSPACE}/scripts/ros/prepare-xarm-production-binding-fixture.sh" \
    "${FIXTURE_ROOT}"
export MANYFORGE_XARM_EXACT_FAKE_URDF="${FIXTURE_ROOT}/model/lite6_fixed_pneumatic.supervised_fake.runtime.urdf"
export MANYFORGE_XARM_EXACT_CONTROLLER_YAML="${FIXTURE_ROOT}/model/ros2_controllers.yaml"

ctest \
    --test-dir "${BUILD_ROOT}/build/manyforge_robot_lifecycle" \
    --output-on-failure
ctest \
    --test-dir "${BUILD_ROOT}/build/manyforge_xarm_control" \
    --exclude-regex test_controller_manager_integration \
    --output-on-failure

INTEGRATION_TEST="${BUILD_ROOT}/build/manyforge_xarm_control/test_controller_manager_integration"
"${INTEGRATION_TEST}" \
    --gtest_filter='-ControllerManagerIntegrationTest.UpdateLoopTimingRemainsBounded' \
    --gtest_brief=1

echo "Fake xArm lifecycle ASan/UBSan lane passed."
