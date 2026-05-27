#!/usr/bin/env bash
# Build the ROS-enabled native module + frontend bundle and start Composer.
#
# This is the entrypoint for the `composer-ros` compose service used by the
# isaac-sim lane. The manyforge-only lane has its equivalent build path in
# `scripts/lib/compose.sh` (start_composer_dev_container) which already
# rebuilds the frontend before launching Composer. Without the frontend
# rebuild here, `launch.sh restart` on the isaac-sim lane would silently
# keep serving a stale `frontend/dist/` bundle even though Python source
# was picked up by the container restart — so HMI changes never reach the
# live browser. Mirror the manyforge-only build flow so the two lanes
# behave identically under restart.

set -euo pipefail

./scripts/ros/build-ros2-control-runtime.sh

# Frontend rebuild. Match the manyforge-only lane semantics:
# - `npm ci` only when node_modules is missing or stale relative to
#   package-lock.json (fast no-op otherwise).
# - `npm run build` unconditionally; vite caches and finishes in ~0.5 s
#   when sources haven't changed, but the operator MUST get fresh
#   bundles when they have.
# Build output streams live to docker logs so the launcher's wait helper
# shows real progress.
echo "Building frontend (manyforge_composer/frontend)..."
(
    cd /workspace/manyforge_composer/frontend
    if [[ ! -d node_modules ]] || [[ package-lock.json -nt node_modules ]]; then
        echo "  (node_modules missing or stale; running npm ci)"
        npm ci --no-audit --no-fund 2>&1
    fi
    npm run build 2>&1
) || echo "  (frontend build had warnings — continuing with whatever dist exists)"

BUILD_DIR="${MANYFORGE_ROBOPLAN_BUILD_DIR:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/roboplan}"
export PYTHONPATH="${BUILD_DIR}:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"
COMPOSER_PORT="${MANYFORGE_COMPOSER_PORT:-${COMPOSER_PORT:-9000}}"
HMI_PORT="${MANYFORGE_HMI_PORT:-${HMI_PORT:-8081}}"

exec python -m manyforge_composer \
    --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
    --host 0.0.0.0 \
    --port "${COMPOSER_PORT}" \
    --hmi-port "${HMI_PORT}"
