#!/bin/bash
set -e

ROS_DISTRO="${ROS_DISTRO:-jazzy}"

if [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
    # shellcheck disable=SC1090
    source "/opt/ros/${ROS_DISTRO}/setup.bash"
fi

if [ -f "/workspace/install/setup.bash" ]; then
    # shellcheck disable=SC1091
    source "/workspace/install/setup.bash"
fi

export PYTHONPATH="/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace:${PYTHONPATH:-}"
export MANYFORGE_BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"

if [ -d "$MANYFORGE_BUILD_DIR" ] && [ ! -w "$MANYFORGE_BUILD_DIR" ]; then
    echo "Build cache not writable by $(whoami). Run: docker compose down -v" >&2
    exit 1
fi

exec "$@"
