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

# Add Python packages to PYTHONPATH
# manyforge_behavior lives at /workspace/manyforge_behavior/manyforge_behavior/
# manyforge_ros lives at /workspace/manyforge_ros/manyforge_ros/
# manyforge_composer lives at /workspace/manyforge_composer/ (flat layout)
export PYTHONPATH="/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace:${PYTHONPATH:-}"

# Use a container path unless a launcher deliberately selects another build
# root. Canonical tests run in an ephemeral dev container; launchers that keep a
# persistent cache bind it to the immutable runtime-image identity.
export MANYFORGE_BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"

# Keep bytecode caches out of the bind-mounted workspace: container
# processes run as root and root-owned __pycache__ dirs break host-side
# tooling. The prefix keeps the bytecode-cache speedup on a container path.
export PYTHONPYCACHEPREFIX="/tmp/manyforge-pycache"


# Fix ownership if the build-cache volume was created by an older root-based
# image.  The Dockerfile now runs as uid 1000 (manyforge), but Docker named
# volumes preserve the ownership from their first creation.
if [ -d "$MANYFORGE_BUILD_DIR" ] && [ ! -w "$MANYFORGE_BUILD_DIR" ]; then
    echo "Build root is not writable by $(whoami): $MANYFORGE_BUILD_DIR" >&2
    echo "Recreate the container with a writable build root; it is never deleted automatically." >&2
    exit 1
fi

# Make cmeel-installed CMake packages discoverable for standalone native builds.
# Pinocchio/coal/eigenpy install under a Python-managed prefix rather than a
# standard system prefix, so plain `cmake -S manyforge_roboplan` needs help.
PINOCCHIO_CMAKE_DIR=$(find /usr -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1 || true)
if [ -n "${PINOCCHIO_CMAKE_DIR:-}" ]; then
    PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "$PINOCCHIO_CMAKE_DIR")")")
    export CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}"
    if [ -f "${PINOCCHIO_PREFIX}/include/boost/version.hpp" ]; then
        export BOOST_ROOT="${PINOCCHIO_PREFIX}"
        export BOOST_INCLUDEDIR="${PINOCCHIO_PREFIX}/include"
        export BOOST_LIBRARYDIR="${PINOCCHIO_PREFIX}/lib"
    fi
fi

PYBIND_BUILD="$MANYFORGE_BUILD_DIR/pybind"

# Add pybind11 module to Python path
if [ -d "$PYBIND_BUILD" ]; then
    export PYTHONPATH="$PYBIND_BUILD:${PYTHONPATH}"
fi

exec "$@"
