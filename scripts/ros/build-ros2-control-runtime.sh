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
CMAKE_SOURCE="${WORKSPACE}/manyforge_roboplan"
TOPIC_INSTALL=/opt/manyforge/topic_based_ros2_control/install
DATA_OVERLAY=
BUILD_TARGETS=(manyforge_ros2_control_execution_host manyforge_roboplan_candidate_worker)
targets_selected=false

usage() {
    cat <<'EOF'
Usage: build-ros2-control-runtime.sh [OPTIONS]
  --source-dir DIR     CMake entrypoint (default: WORKSPACE/manyforge_roboplan)
  --target TARGET      Build target; repeat for several (default: host + worker)
  --topic-install DIR  Pinned stock PickNik install (default: image install)
  --data-overlay DIR   Verified isolated data install root containing xarm_description
  --help              Show this help without sourcing ROS or configuring

BUILD_TESTING is selected by MANYFORGE_ROS2_CONTROL_BUILD_TESTING (ON/OFF).
The caller owns Docker/image selection, input verification and the global build lock.
This helper neither starts ROS processes nor runs tests.
EOF
}
while (( $# )); do
    case "$1" in
        --help) usage; exit 0 ;;
        --source-dir|--target|--topic-install|--data-overlay)
            if (( $# < 2 )) || [[ -z "$2" || "$2" == --* ]]; then
                printf 'Missing value for %s\n' "$1" >&2
                exit 2
            fi
            case "$1" in
                --source-dir) CMAKE_SOURCE=$2 ;;
                --topic-install) TOPIC_INSTALL=$2 ;;
                --data-overlay) DATA_OVERLAY=$2 ;;
                --target)
                    if [[ ! "$2" =~ ^[a-zA-Z0-9_][a-zA-Z0-9_.+-]*$ ]]; then
                        printf 'Invalid build target: %s\n' "$2" >&2
                        exit 2
                    fi
                    if [[ "$targets_selected" == false ]]; then BUILD_TARGETS=(); fi
                    targets_selected=true
                    BUILD_TARGETS+=("$2")
                    ;;
            esac
            shift 2
            ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
case "${MANYFORGE_ROS2_CONTROL_BUILD_TYPE:-Release}" in
    Release|RelWithDebInfo|Debug) ;;
    *) printf 'MANYFORGE_ROS2_CONTROL_BUILD_TYPE must be Release, RelWithDebInfo, or Debug.\n' >&2; exit 2 ;;
esac
case "${MANYFORGE_ROS2_CONTROL_BUILD_TESTING:-OFF}" in
    ON|OFF) ;;
    *) printf 'MANYFORGE_ROS2_CONTROL_BUILD_TESTING must be ON or OFF.\n' >&2; exit 2 ;;
esac
if [[ ! "$BUILD_PARALLEL_LEVEL" =~ ^[1-9][0-9]*$ ]]; then
    printf 'CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.\n' >&2
    exit 2
fi

for required in \
    "${CMAKE_SOURCE}/CMakeLists.txt" \
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

# Source identity only: a matching marker does not qualify runtime behavior.
if [[ ! -f "${TOPIC_INSTALL}/UPSTREAM_COMMIT" ]] || \
    [[ "$(<"${TOPIC_INSTALL}/UPSTREAM_COMMIT")" != "aa9f9ef9525a60f8becea322537dd1d6d1bf9123" ]]; then
    printf 'The selected PickNik topic_based_ros2_control install is missing or has the wrong source pin.\n' >&2
    exit 2
fi
if [[ -n "$DATA_OVERLAY" && ! -f "${DATA_OVERLAY}/setup.bash" ]]; then
    printf 'Data overlay setup is missing: %s\n' "$DATA_OVERLAY" >&2
    exit 2
fi

set +u
# shellcheck disable=SC1090
source "/opt/ros/${ROS_DISTRO}/setup.bash"
# shellcheck disable=SC1091
source "${TOPIC_INSTALL}/setup.bash"
if [[ -n "$DATA_OVERLAY" ]]; then
    # The caller supplies a previously verified data-only install, never vendor source.
    # shellcheck disable=SC1090
    source "${DATA_OVERLAY}/setup.bash"
fi
if [[ -d "${MANYFORGE_VENV}" ]]; then
    # shellcheck disable=SC1091
    source "${MANYFORGE_VENV}/bin/activate"
fi
set -u

# Bind the selected package to the image install whose source marker was checked.
topic_based_prefix=$(ros2 pkg prefix topic_based_ros2_control)
if [[ "${topic_based_prefix}" != "$TOPIC_INSTALL" ]]; then
    printf 'Isaac requires the selected PickNik image install; actual prefix=%s\n' "${topic_based_prefix}" >&2
    exit 2
fi
if [[ -n "$DATA_OVERLAY" ]]; then
    data_prefix=$(ros2 pkg prefix xarm_description)
    if [[ "$data_prefix" != "${DATA_OVERLAY}/xarm_description" ]]; then
        printf 'Selected xarm_description does not match the verified data overlay: %s\n' "$data_prefix" >&2
        exit 2
    fi
fi

PINOCCHIO_CMAKE_DIR=$(
    find "${MANYFORGE_VENV}" /usr -name pinocchioConfig.cmake \
        -printf '%h\n' 2>/dev/null | head -1 || true
)
if [[ -z "${PINOCCHIO_CMAKE_DIR}" ]]; then
    printf 'pinocchioConfig.cmake not found; cannot build the I9 runtime.\n' >&2
    exit 2
fi
PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "${PINOCCHIO_CMAKE_DIR}")")")
export CMAKE_PREFIX_PATH="${TOPIC_INSTALL}:/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}"

mkdir -p "${ROBOPLAN_BUILD}" "${ROS_OVERLAY}"
cmake -S "${CMAKE_SOURCE}" -B "${ROBOPLAN_BUILD}" \
    -DCMAKE_BUILD_TYPE="${MANYFORGE_ROS2_CONTROL_BUILD_TYPE:-Release}" \
    -DBUILD_TESTING="${MANYFORGE_ROS2_CONTROL_BUILD_TESTING:-OFF}" \
    -DMANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=ON
cmake --build "${ROBOPLAN_BUILD}" \
    --target "${BUILD_TARGETS[@]}" \
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

for target in "${BUILD_TARGETS[@]}"; do
    case "$target" in
        all)
            test -x "${ROBOPLAN_BUILD}/manyforge_ros2_control_execution_host"
            test -x "${ROBOPLAN_BUILD}/manyforge_roboplan_candidate_worker"
            ;;
        manyforge_ros2_control_execution_host|manyforge_roboplan_candidate_worker)
            test -x "${ROBOPLAN_BUILD}/${target}"
            ;;
    esac
done
test -f "${ROS_OVERLAY}/install/setup.bash"

printf 'ROS build complete: graph=%s targets=%s overlay=%s\n' \
    "${ROBOPLAN_BUILD}" "${BUILD_TARGETS[*]}" \
    "${ROS_OVERLAY}/install"
