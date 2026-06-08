#!/usr/bin/env bash
# Build the ROS-enabled native module + frontend bundle and start Composer.
#
# This is the entrypoint for the `composer-ros` compose service used by the
# isaac-sim lane. The manyforge-only lane has its equivalent build path in
# `scripts/lib/compose.sh` (start_composer_dev_container) which already
# rebuilds the frontend before launching Composer. Without the frontend
# rebuild here, `launch.sh restart` on the isaac-sim lane would silently
# keep serving a stale `frontend/dist/` bundle even though Python source
# was picked up by the container restart — so viewer changes never reach the
# live browser. Mirror the manyforge-only build flow so the two lanes
# behave identically under restart.

set -euo pipefail

./scripts/ros/build-ros2-control-runtime.sh

source /workspace/scripts/lib/frontend.sh
manyforge_build_frontend_dist \
    /workspace/manyforge_composer/frontend \
    "${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"

BUILD_DIR="${MANYFORGE_ROBOPLAN_BUILD_DIR:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/roboplan}"
export PYTHONPATH="${BUILD_DIR}:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"
COMPOSER_PORT="${MANYFORGE_COMPOSER_PORT:-${COMPOSER_PORT:-9000}}"
HMI_PORT="${MANYFORGE_HMI_PORT:-${HMI_PORT:-8081}}"
ENABLE_LEGACY_HMI="${MANYFORGE_ENABLE_LEGACY_VISER_HMI:-${ENABLE_LEGACY_VISER_HMI:-false}}"

hmi_args=()
case "${ENABLE_LEGACY_HMI,,}" in
    1|true|yes|on)
        hmi_args+=(--hmi-port "${HMI_PORT}")
        ;;
esac

exec python -m manyforge_composer \
    --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
    --host 0.0.0.0 \
    --port "${COMPOSER_PORT}" \
    "${hmi_args[@]}" \
    --static-dir "${MANYFORGE_FRONTEND_STATIC_DIR}"
