#!/usr/bin/env bash
# Generate and validate the exact Lite6 production binding for fake-HIL tests.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
FIXTURE_ROOT=${1:-}

if [[ -z "${FIXTURE_ROOT}" ]]; then
    echo "usage: $0 <fixture-root>" >&2
    exit 2
fi

MANYFORGE_SKIP_REAL_LITE6_BUILD=1 \
MANYFORGE_REAL_LITE6_OUTPUT_ROOT="${FIXTURE_ROOT}" \
    "${WORKSPACE}/scripts/ros/generate-lite6-fixed-pneumatic-model.sh"

test -s "${FIXTURE_ROOT}/model/lite6_fixed_pneumatic.supervised.runtime.urdf"
test -s "${FIXTURE_ROOT}/model/lite6_fixed_pneumatic.supervised_fake.runtime.urdf"
test -s "${FIXTURE_ROOT}/model/ros2_controllers.yaml"
test -s "${FIXTURE_ROOT}/binding-manifest.json"

echo "Exact Lite6 production binding fixture ready in ${FIXTURE_ROOT}."
