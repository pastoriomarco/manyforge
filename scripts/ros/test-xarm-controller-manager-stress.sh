#!/usr/bin/env bash
# Repeat the real controller_manager/JTC fake lane in fresh OS processes.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}
ITERATIONS=${MANYFORGE_XARM_CM_STRESS_ITERATIONS:-20}

if [[ ! "${ITERATIONS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MANYFORGE_XARM_CM_STRESS_ITERATIONS must be a positive integer" >&2
    exit 2
fi

if [[ "${MANYFORGE_SKIP_REAL_LITE6_BUILD:-0}" != "1" ]]; then
    "${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"
fi

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

FIXTURE_ROOT=$(mktemp -d /tmp/manyforge-xarm-binding-fixture.XXXXXX)
LOG_ROOT=$(mktemp -d /tmp/manyforge-xarm-cm-stress.XXXXXX)
trap 'rm -r -- "${FIXTURE_ROOT}" "${LOG_ROOT}"' EXIT
"${WORKSPACE}/scripts/ros/prepare-xarm-production-binding-fixture.sh" \
    "${FIXTURE_ROOT}"
export MANYFORGE_XARM_EXACT_FAKE_URDF="${FIXTURE_ROOT}/model/lite6_fixed_pneumatic.supervised_fake.runtime.urdf"
export MANYFORGE_XARM_EXACT_CONTROLLER_YAML="${FIXTURE_ROOT}/model/ros2_controllers.yaml"

TEST_EXECUTABLE="${BUILD_ROOT}/build/manyforge_xarm_control/test_controller_manager_integration"
if [[ ! -x "${TEST_EXECUTABLE}" ]]; then
    echo "Controller-manager integration executable is missing" >&2
    exit 2
fi

export RCUTILS_LOGGING_SEVERITY_THRESHOLD=${RCUTILS_LOGGING_SEVERITY_THRESHOLD:-ERROR}

for ((iteration = 1; iteration <= ITERATIONS; ++iteration)); do
    ITERATION_LOG="${LOG_ROOT}/iteration-${iteration}.log"
    if ! "${TEST_EXECUTABLE}" --gtest_brief=1 >"${ITERATION_LOG}" 2>&1; then
        cat "${ITERATION_LOG}" >&2
        echo "controller-manager/JTC process iteration ${iteration}/${ITERATIONS} failed" >&2
        exit 1
    fi
    echo "controller-manager/JTC process iteration ${iteration}/${ITERATIONS} passed"
done

MANYFORGE_TEST_INSTANCE_SUFFIX=parallel_a \
    "${TEST_EXECUTABLE}" \
    --gtest_filter='ControllerManagerIntegrationTest.RealJtcObeysAuthorityLossAndRejectsStaleCommandResumption' \
    --gtest_brief=1 >"${LOG_ROOT}/parallel-a.log" 2>&1 &
FIRST_PID=$!
MANYFORGE_TEST_INSTANCE_SUFFIX=parallel_b \
    "${TEST_EXECUTABLE}" \
    --gtest_filter='ControllerManagerIntegrationTest.RealJtcObeysAuthorityLossAndRejectsStaleCommandResumption' \
    --gtest_brief=1 >"${LOG_ROOT}/parallel-b.log" 2>&1 &
SECOND_PID=$!

set +e
wait "${FIRST_PID}"
FIRST_STATUS=$?
wait "${SECOND_PID}"
SECOND_STATUS=$?
set -e
if ((FIRST_STATUS != 0 || SECOND_STATUS != 0)); then
    cat "${LOG_ROOT}/parallel-a.log" >&2
    cat "${LOG_ROOT}/parallel-b.log" >&2
    echo "Concurrent controller-manager/JTC process isolation failed" >&2
    exit 1
fi

echo "Concurrent controller-manager/JTC process isolation passed (2/2)."
echo "Controller-manager/JTC process stress lane passed (${ITERATIONS}/${ITERATIONS})."
