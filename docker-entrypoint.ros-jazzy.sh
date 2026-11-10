#!/bin/bash
set -e

# OD-59: the strict real-time profile locks the process memory (mlockall);
# it needs an unlimited memlock limit (`docker run --ulimit memlock=-1:-1`,
# compose `ulimits: memlock: -1`). Refuse to start without it.
if [ "${MANYFORGE_RT_RESOURCE_PROFILE:-off}" = "strict" ] && [ "$(ulimit -l)" != "unlimited" ]; then
    echo "manyforge_rt_profile_refused reason=rt_memlock_limit_not_unlimited limit=$(ulimit -l):" \
         "the strict real-time profile needs an unlimited memlock limit" >&2
    exit 1
fi


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

if [ "${MANYFORGE_SOURCE_WORKSPACE_INSTALL:-OFF}" = "ON" ] &&
    [ -f "/workspace/install/setup.bash" ]; then
    # Explicit development escape hatch only. Canonical tests and launchers
    # use container-local or immutable-image-keyed build roots instead.
    # shellcheck disable=SC1091
    source "/workspace/install/setup.bash"
fi

export PYTHONPATH="/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace:${PYTHONPATH:-}"
export MANYFORGE_BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"

if [ -d "$MANYFORGE_BUILD_DIR" ] && [ ! -w "$MANYFORGE_BUILD_DIR" ]; then
    echo "Build root is not writable by $(whoami): $MANYFORGE_BUILD_DIR" >&2
    echo "Recreate the container with a writable build root; it is never deleted automatically." >&2
    exit 1
fi

exec "$@"
