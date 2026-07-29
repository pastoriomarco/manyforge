#!/usr/bin/env bash
# Build the ManyForge-owned ROS packages used by the experimental Lite6 lane.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
ROS_DISTRO=${ROS_DISTRO:-jazzy}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}

set +u
source "/opt/ros/${ROS_DISTRO}/setup.bash"
source "/opt/manyforge/xarm/install/setup.bash"
set -u

# This lane builds ROS-only ownership packages. The full development image also
# exposes a Python/cmeel prefix for Pinocchio and RoboPlan; allowing that prefix
# into ROS dependency discovery can mix its TinyXML2/Boost ABI with Jazzy's
# system libraries. Retain every other prefix and use the ROS dependency set.
MANYFORGE_ROS_CMAKE_PREFIX_PATH=
MANYFORGE_SAVED_IFS=${IFS}
IFS=:
for MANYFORGE_PREFIX_ENTRY in ${CMAKE_PREFIX_PATH:-}; do
    case "${MANYFORGE_PREFIX_ENTRY}" in
        */cmeel.prefix) ;;
        *)
            MANYFORGE_ROS_CMAKE_PREFIX_PATH="${MANYFORGE_ROS_CMAKE_PREFIX_PATH:+${MANYFORGE_ROS_CMAKE_PREFIX_PATH}:}${MANYFORGE_PREFIX_ENTRY}"
            ;;
    esac
done
IFS=${MANYFORGE_SAVED_IFS}
export CMAKE_PREFIX_PATH=${MANYFORGE_ROS_CMAKE_PREFIX_PATH}
unset BOOST_ROOT

MANYFORGE_REAL_LITE6_CMAKE_ARGS=(-DBUILD_TESTING=ON)
case "${MANYFORGE_REAL_LITE6_BUILD_PROFILE:-default}" in
    default) ;;
    asan-ubsan)
        MANYFORGE_REAL_LITE6_CMAKE_ARGS+=(
            -DCMAKE_BUILD_TYPE=Debug
            "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
            -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined
        )
        ;;
    tsan)
        MANYFORGE_REAL_LITE6_CMAKE_ARGS+=(
            -DCMAKE_BUILD_TYPE=Debug
            "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer"
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
            -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread
        )
        ;;
    *)
        echo "Unsupported real-Lite6 build profile: ${MANYFORGE_REAL_LITE6_BUILD_PROFILE}" >&2
        exit 2
        ;;
esac

colcon --log-base "${BUILD_ROOT}/log" build \
    --base-paths \
        "${WORKSPACE}/manyforge_robot_lifecycle_msgs" \
        "${WORKSPACE}/manyforge_robot_lifecycle" \
        "${WORKSPACE}/manyforge_xarm_control" \
        "${WORKSPACE}/manyforge_xarm_adapter" \
    --build-base "${BUILD_ROOT}/build" \
    --install-base "${BUILD_ROOT}/install" \
    --symlink-install \
    --packages-select \
        manyforge_robot_lifecycle_msgs \
        manyforge_robot_lifecycle \
        manyforge_xarm_control \
        manyforge_xarm_adapter \
    --cmake-args "${MANYFORGE_REAL_LITE6_CMAKE_ARGS[@]}"

echo "Real-Lite6 ManyForge ROS packages ready in ${BUILD_ROOT}/install."
