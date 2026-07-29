#!/usr/bin/env bash
# Start the non-owner Composer service for one resolved real-Lite6 runtime.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}
OUTPUT_ROOT=${MANYFORGE_REAL_LITE6_OUTPUT_ROOT:?MANYFORGE_REAL_LITE6_OUTPUT_ROOT is required}
COMPOSER_PORT=${MANYFORGE_COMPOSER_PORT:-9000}
RUNTIME_ROOT="${OUTPUT_ROOT}/resolved/lite6-a"

for required in \
    "${OUTPUT_ROOT}/owner-ready" \
    "${RUNTIME_ROOT}/runtime-binding.json" \
    "${RUNTIME_ROOT}/runtime-record.json" \
    "${RUNTIME_ROOT}/commissioning.deployment.yaml"; do
    if [[ ! -f "${required}" ]]; then
        echo "real-robot owner did not publish required artifact: ${required}" >&2
        exit 1
    fi
done
"${WORKSPACE}/scripts/ros/build-ros2-control-runtime.sh"

# shellcheck source=../lib/frontend.sh
source "${WORKSPACE}/scripts/lib/frontend.sh"
manyforge_build_frontend_dist \
    "${WORKSPACE}/manyforge_composer/frontend" \
    /tmp/manyforge-build

set +u
source "/opt/ros/${ROS_DISTRO:-jazzy}/setup.bash"
source /opt/manyforge/xarm/install/setup.bash
source "${BUILD_ROOT}/install/setup.bash"
set -u
export PYTHONPATH="/tmp/manyforge-build/roboplan:${WORKSPACE}/manyforge_roboplan/python:${PYTHONPATH:-}"

exec python -m manyforge_composer \
    --catalog-path \
    "${WORKSPACE}/manyforge_behavior/resources/node_catalog.yaml" \
    --host 0.0.0.0 \
    --port "${COMPOSER_PORT}" \
    --static-dir "${MANYFORGE_FRONTEND_STATIC_DIR}" \
    --trace-dir "${OUTPUT_ROOT}/traces"
