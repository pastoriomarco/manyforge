#!/usr/bin/env bash
# Run the deterministic fake xArm lifecycle and sole-owner plugin lane.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}

if [[ "${MANYFORGE_SKIP_REAL_LITE6_BUILD:-0}" != "1" ]]; then
    "${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"
fi

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

FIXTURE_ROOT=$(mktemp -d /tmp/manyforge-xarm-binding-fixture.XXXXXX)
trap 'rm -r -- "${FIXTURE_ROOT}"' EXIT
"${WORKSPACE}/scripts/ros/prepare-xarm-production-binding-fixture.sh" \
    "${FIXTURE_ROOT}"
export MANYFORGE_XARM_EXACT_FAKE_URDF="${FIXTURE_ROOT}/model/lite6_fixed_pneumatic.supervised_fake.runtime.urdf"
export MANYFORGE_XARM_EXACT_CONTROLLER_YAML="${FIXTURE_ROOT}/model/ros2_controllers.yaml"

ctest \
    --test-dir "${BUILD_ROOT}/build/manyforge_xarm_control" \
    --output-on-failure

echo "Fake xArm lifecycle ownership lane passed."
