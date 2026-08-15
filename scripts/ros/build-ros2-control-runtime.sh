#!/usr/bin/env bash
# Build the I9 ros2_control execution product and its mechanical ROS overlay.
#
# This is deliberately narrower than a cell launcher: it neither starts a
# Manager nor touches Isaac.  The build roots are persistent so an Orin image
# can incrementally rebuild one frozen source batch and reuse the exact output
# for controller/runtime replacement tests.

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
ROS_DISTRO="${ROS_DISTRO:-jazzy}"
MANYFORGE_VENV="${MANYFORGE_VENV:-/opt/manyforge/venv}"
BUILD_ROOT="${MANYFORGE_ROS2_CONTROL_BUILD_ROOT:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/isaac}"
ROBOPLAN_BUILD="${MANYFORGE_ROBOPLAN_BUILD_DIR:-${BUILD_ROOT}/roboplan}"
ROS_OVERLAY="${MANYFORGE_ISAAC_ROS_OVERLAY:-${BUILD_ROOT}/ros-overlay}"
BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"

for required in \
    "${WORKSPACE}/manyforge_roboplan/CMakeLists.txt" \
    "${WORKSPACE}/manyforge_ros2_control_execution/CMakeLists.txt" \
    "${WORKSPACE}/manyforge_cclc_msgs/package.xml" \
    "${WORKSPACE}/manyforge_ros_bringup/package.xml"; do
    if [[ ! -f "${required}" ]]; then
        printf 'I9 ros2_control source is incomplete: %s\n' "${required}" >&2
        exit 2
    fi
done
if [[ ! -w "$(dirname "${BUILD_ROOT}")" && ! -w "${BUILD_ROOT}" ]]; then
    printf 'I9 ros2_control build root is not writable: %s\n' "${BUILD_ROOT}" >&2
    exit 2
fi

set +u
# shellcheck disable=SC1090
source "/opt/ros/${ROS_DISTRO}/setup.bash"
# shellcheck disable=SC1091
source "/opt/manyforge/topic_based_ros2_control/install/setup.bash"
if [[ -d "${MANYFORGE_VENV}" ]]; then
    # shellcheck disable=SC1091
    source "${MANYFORGE_VENV}/bin/activate"
fi
set -u

PINOCCHIO_CMAKE_DIR=$(
    find "${MANYFORGE_VENV}" /usr -name pinocchioConfig.cmake \
        -printf '%h\n' 2>/dev/null | head -1 || true
)
if [[ -z "${PINOCCHIO_CMAKE_DIR}" ]]; then
    printf 'pinocchioConfig.cmake not found; cannot build the I9 runtime.\n' >&2
    exit 2
fi
PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "${PINOCCHIO_CMAKE_DIR}")")")
export CMAKE_PREFIX_PATH="/opt/manyforge/topic_based_ros2_control/install:/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}"

case "${MANYFORGE_ROS2_CONTROL_BUILD_TYPE:-Release}" in
    Release|RelWithDebInfo|Debug) ;;
    *)
        printf 'MANYFORGE_ROS2_CONTROL_BUILD_TYPE must be Release, RelWithDebInfo, or Debug.\n' >&2
        exit 2
        ;;
esac

mkdir -p "${ROBOPLAN_BUILD}" "${ROS_OVERLAY}"
cmake -S "${WORKSPACE}/manyforge_roboplan" -B "${ROBOPLAN_BUILD}" \
    -DCMAKE_BUILD_TYPE="${MANYFORGE_ROS2_CONTROL_BUILD_TYPE:-Release}" \
    -DBUILD_TESTING="${MANYFORGE_ROS2_CONTROL_BUILD_TESTING:-OFF}" \
    -DMANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=ON
cmake --build "${ROBOPLAN_BUILD}" \
    --target \
        manyforge_ros2_control_execution_host \
        manyforge_roboplan_candidate_worker \
    --parallel "${BUILD_PARALLEL_LEVEL}"

# Keep cmeel's Boost/TinyXML stack out of the ament-only overlay.  The C++
# runtime above consumes the qualified RoboPlan prefix; the ROS overlay below
# must resolve Jazzy's ABI-compatible dependency set.
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
unset BOOST_ROOT BOOST_INCLUDEDIR BOOST_LIBRARYDIR

colcon --log-base "${ROS_OVERLAY}/log" build \
    --base-paths \
        "${WORKSPACE}/manyforge_cclc_msgs" \
        "${WORKSPACE}/manyforge_ros_bringup" \
    --build-base "${ROS_OVERLAY}/build" \
    --install-base "${ROS_OVERLAY}/install" \
    --symlink-install \
    --packages-select manyforge_cclc_msgs manyforge_ros_bringup \
    --cmake-args \
        -DBUILD_TESTING="${MANYFORGE_ROS2_CONTROL_BUILD_TESTING:-OFF}"

test -x "${ROBOPLAN_BUILD}/manyforge_ros2_control_execution_host"
test -x "${ROBOPLAN_BUILD}/manyforge_roboplan_candidate_worker"
test -f "${ROS_OVERLAY}/install/setup.bash"

printf 'I9 ros2_control runtime ready: host=%s overlay=%s\n' \
    "${ROBOPLAN_BUILD}/manyforge_ros2_control_execution_host" \
    "${ROS_OVERLAY}/install"
