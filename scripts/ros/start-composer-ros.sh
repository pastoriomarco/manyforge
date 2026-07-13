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
qualification_probe="${BUILD_DIR}/libroboplan_collision_counter_probe.so"
if [[ "${MANYFORGE_ENABLE_QUALIFICATION_PROBES:-OFF}" == "ON" ]]; then
    requested_probe="${MANYFORGE_QUALIFICATION_LD_PRELOAD:-${qualification_probe}}"
    if [[ ! -f "${requested_probe}" ]]; then
        echo "Qualification preload library is missing: ${requested_probe}" >&2
        exit 1
    fi
    if [[ "$(readlink -f "${requested_probe}")" != "$(readlink -f "${qualification_probe}")" ]]; then
        echo "Qualification preload must be the probe built with this runtime: ${qualification_probe}" >&2
        echo "Refusing stale or cross-build probe: ${requested_probe}" >&2
        exit 1
    fi
    export LD_PRELOAD="${qualification_probe}${LD_PRELOAD:+:${LD_PRELOAD}}"
elif [[ -n "${MANYFORGE_QUALIFICATION_LD_PRELOAD:-}" ]]; then
    echo "Qualification preload requires MANYFORGE_ENABLE_QUALIFICATION_PROBES=ON." >&2
    exit 1
fi
COMPOSER_PORT="${MANYFORGE_COMPOSER_PORT:-${COMPOSER_PORT:-9000}}"

exec python -m manyforge_composer \
    --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
    --host 0.0.0.0 \
    --port "${COMPOSER_PORT}" \
    --static-dir "${MANYFORGE_FRONTEND_STATIC_DIR}"
