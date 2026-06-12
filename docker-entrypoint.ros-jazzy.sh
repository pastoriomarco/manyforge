#!/bin/bash
set -e


# Keep bytecode caches out of the bind-mounted workspace: container
# processes run as root and root-owned __pycache__ dirs break host-side
# tooling. The prefix keeps the bytecode-cache speedup on a container path.
export PYTHONPYCACHEPREFIX="/tmp/manyforge-pycache"

ROS_DISTRO="${ROS_DISTRO:-jazzy}"

if [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
    # shellcheck disable=SC1090
    source "/opt/ros/${ROS_DISTRO}/setup.bash"
fi

if [ -f "/opt/manyforge/topic_based_ros2_control/install/setup.bash" ]; then
    # shellcheck disable=SC1091
    source "/opt/manyforge/topic_based_ros2_control/install/setup.bash"
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
