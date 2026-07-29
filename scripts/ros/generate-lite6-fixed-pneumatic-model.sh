#!/usr/bin/env bash
# Generate the fixed-tool model and immutable binding under ignored runtime state.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}
OUTPUT_ROOT=${MANYFORGE_REAL_LITE6_OUTPUT_ROOT:-${WORKSPACE}/.manyforge-var/real-lite6}
PROFILE=${MANYFORGE_LITE6_PROFILE:-${WORKSPACE}/manyforge_xarm_adapter/config/lite6_fixed_pneumatic.profile.yaml}
SOURCE_MANIFEST=${MANYFORGE_XARM_SOURCE_MANIFEST:-/opt/manyforge/xarm/source-manifest.json}

if [[ "${MANYFORGE_SKIP_REAL_LITE6_BUILD:-0}" != "1" ]]; then
    "${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"
fi

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

ros2 run manyforge_xarm_adapter manyforge-xarm-generate-fixed-model \
    --profile "${PROFILE}" \
    --source-manifest "${SOURCE_MANIFEST}" \
    --output "${OUTPUT_ROOT}/model"

ros2 run manyforge_xarm_adapter manyforge-xarm-binding-manifest \
    --profile "${PROFILE}" \
    --model-manifest "${OUTPUT_ROOT}/model/MODEL_MANIFEST.json" \
    --source-manifest "${SOURCE_MANIFEST}" \
    --output "${OUTPUT_ROOT}/binding-manifest.json"

ros2 run manyforge_xarm_adapter manyforge-xarm-validate-controller-binding \
    --binding-manifest "${OUTPUT_ROOT}/binding-manifest.json" \
    --model-root "${OUTPUT_ROOT}/model" \
    --output "${OUTPUT_ROOT}/controller-binding-validation.json"

echo "Generated model and binding under ${OUTPUT_ROOT}."
