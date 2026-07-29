#!/usr/bin/env bash
# Run XC-6 adapter tests and the offline XC-7A lifecycle-core gate.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}

"${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

ctest \
    --test-dir "${BUILD_ROOT}/build/manyforge_robot_lifecycle" \
    --output-on-failure

ctest \
    --test-dir "${BUILD_ROOT}/build/manyforge_robot_lifecycle_msgs" \
    --output-on-failure

MANYFORGE_SKIP_REAL_LITE6_BUILD=1 \
    "${WORKSPACE}/scripts/ros/test-xarm-lifecycle-fake.sh"

python3 -m pytest \
    -p no:cacheprovider \
    "${WORKSPACE}/manyforge_xarm_adapter/test" \
    "${WORKSPACE}/manyforge_ros/tests/test_execution_admission.py" \
    "${WORKSPACE}/manyforge_ros/tests/test_execution_admission_ros.py" \
    "${WORKSPACE}/manyforge_ros/tests/test_ros2_control_kernel_factory.py" \
    -q

ament_flake8 \
    "${WORKSPACE}/manyforge_supervision" \
    "${WORKSPACE}/manyforge_xarm_adapter/manyforge_xarm_adapter" \
    "${WORKSPACE}/manyforge_xarm_adapter/test" \
    "${WORKSPACE}/manyforge_xarm_adapter/launch/lite6_real_read_only.launch.py" \
    "${WORKSPACE}/manyforge_ros/manyforge_ros/execution_admission.py" \
    "${WORKSPACE}/manyforge_ros/tests/test_execution_admission.py" \
    "${WORKSPACE}/manyforge_ros/tests/test_execution_admission_ros.py"

ament_pep257 \
    "${WORKSPACE}/manyforge_supervision" \
    "${WORKSPACE}/manyforge_xarm_adapter/manyforge_xarm_adapter" \
    "${WORKSPACE}/manyforge_xarm_adapter/launch/lite6_real_read_only.launch.py" \
    "${WORKSPACE}/manyforge_ros/manyforge_ros/execution_admission.py"

ament_copyright \
    "${WORKSPACE}/manyforge_robot_lifecycle" \
    "${WORKSPACE}/manyforge_robot_lifecycle_msgs" \
    "${WORKSPACE}/manyforge_xarm_control"

ament_cpplint \
    "${WORKSPACE}/manyforge_robot_lifecycle/include" \
    "${WORKSPACE}/manyforge_robot_lifecycle/src" \
    "${WORKSPACE}/manyforge_robot_lifecycle/test" \
    "${WORKSPACE}/manyforge_xarm_control/include" \
    "${WORKSPACE}/manyforge_xarm_control/src" \
    "${WORKSPACE}/manyforge_xarm_control/test"

ament_uncrustify \
    "${WORKSPACE}/manyforge_robot_lifecycle/include" \
    "${WORKSPACE}/manyforge_robot_lifecycle/src" \
    "${WORKSPACE}/manyforge_robot_lifecycle/test" \
    "${WORKSPACE}/manyforge_xarm_control/include" \
    "${WORKSPACE}/manyforge_xarm_control/src" \
    "${WORKSPACE}/manyforge_xarm_control/test"

MANYFORGE_FORBIDDEN_XARM_SDK_PATTERN='xarm_sdk_transport_session|#include[[:space:]]*<xarm_api/|find_package\(xarm_api|<depend>xarm_api</depend>'
if command -v rg >/dev/null 2>&1; then
    MANYFORGE_FORBIDDEN_XARM_SDK_SEARCH=(
        rg -n
        "${MANYFORGE_FORBIDDEN_XARM_SDK_PATTERN}"
        "${WORKSPACE}/manyforge_xarm_control"
    )
else
    MANYFORGE_FORBIDDEN_XARM_SDK_SEARCH=(
        grep -R -n -E
        "${MANYFORGE_FORBIDDEN_XARM_SDK_PATTERN}"
        "${WORKSPACE}/manyforge_xarm_control"
    )
fi
if "${MANYFORGE_FORBIDDEN_XARM_SDK_SEARCH[@]}"; then
    echo "ManyForge production control retains a forbidden direct-SDK dependency" >&2
    exit 1
fi

bash -n \
    "${WORKSPACE}/scripts/ros/generate-lite6-fixed-pneumatic-model.sh" \
    "${WORKSPACE}/scripts/ros/prepare-xarm-production-binding-fixture.sh" \
    "${WORKSPACE}/scripts/ros/resolve-lite6-supervised-runtime.sh" \
    "${WORKSPACE}/scripts/ros/run-lite6-supervised-preflight.sh" \
    "${WORKSPACE}/scripts/ros/start-lite6-real-robot-composer.sh" \
    "${WORKSPACE}/scripts/ros/start-lite6-real-robot-owner.sh" \
    "${WORKSPACE}/scripts/ros/test-xarm-controller-manager-stress.sh" \
    "${WORKSPACE}/scripts/ros/test-xarm-lifecycle-fake.sh" \
    "${WORKSPACE}/scripts/ros/test-xarm-lifecycle-sanitizers.sh" \
    "${WORKSPACE}/scripts/ros/test-xarm-lifecycle-thread-sanitizer.sh" \
    "${WORKSPACE}/scripts/xarm/capture-lite6-live-evidence.sh" \
    "${WORKSPACE}/scripts/xarm/capture-lite6-product-lane-state.sh" \
    "${WORKSPACE}/scripts/xarm/capture-lite6-controller-gate.sh" \
    "${WORKSPACE}/scripts/xarm/capture-lite6-supervised-command.sh" \
    "${WORKSPACE}/scripts/xarm/launch-lite6-supervised-owner.sh" \
    "${WORKSPACE}/scripts/xarm/lite6-local-operator.sh" \
    "${WORKSPACE}/scripts/xarm/verify-lite6-supervised-topology.sh"

python3 -m py_compile \
    "${WORKSPACE}/manyforge_supervision/cell_supervisor.py" \
    "${WORKSPACE}/manyforge_supervision/component_health.py" \
    "${WORKSPACE}/manyforge_supervision/manifest.py" \
    "${WORKSPACE}/scripts/xarm/prepare_lite6_commissioning_candidate.py"

echo "Real-Lite6 offline runtime tests passed."
