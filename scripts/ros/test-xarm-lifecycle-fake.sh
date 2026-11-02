#!/usr/bin/env bash
# Run the xArm adapter's tests (fault catalogue, action sequences, I/O, launch
# contribution) against their fake driver.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}

if [[ "${MANYFORGE_SKIP_REAL_LITE6_BUILD:-0}" != "1" ]]; then
    "${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"
fi

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

ctest \
    --test-dir "${BUILD_ROOT}/build/manyforge_xarm_control" \
    --output-on-failure

echo "xArm adapter tests passed."
