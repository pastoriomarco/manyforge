#!/usr/bin/env bash
# Build the ROS-enabled native module and start Composer from the ROS image.

set -euo pipefail

./scripts/ros/build-ros2-control-runtime.sh

BUILD_DIR="${MANYFORGE_ROBOPLAN_BUILD_DIR:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/roboplan}"
export PYTHONPATH="${BUILD_DIR}:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"
COMPOSER_PORT="${MANYFORGE_COMPOSER_PORT:-${COMPOSER_PORT:-9000}}"
HMI_PORT="${MANYFORGE_HMI_PORT:-${HMI_PORT:-8081}}"

exec python -m manyforge_composer \
    --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
    --host 0.0.0.0 \
    --port "${COMPOSER_PORT}" \
    --hmi-port "${HMI_PORT}"
