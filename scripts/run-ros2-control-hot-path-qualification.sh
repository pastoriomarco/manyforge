#!/usr/bin/env bash
# Report-only ROS hot-path measurement. Run after cutover.
# Reports whole fake-FJT feedback delivery (SDK/executor/polling included) and
# repeated idle observer updates through the public runtime handle. There are
# no allocation/update budgets; the maintainer decides from measured evidence.
# Does not start a Manager, Isaac, a real robot, or a controller process.
#
# Use an existing qualified ROS-full image through diagnostic-build.sh:
#   MANYFORGE_DIAGNOSTIC_IMAGE=sha256:... ./scripts/run-ros2-control-hot-path-qualification.sh
# Inside that toolchain: ./scripts/run-ros2-control-hot-path-qualification.sh --in-container
# Caller owns the global build lock and any resource-priority window.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN_CONTAINER=false
case "${1:-}" in
    --in-container) IN_CONTAINER=true; shift ;;
    --help)
        printf '%s\n' 'Usage: run-ros2-control-hot-path-qualification.sh [--in-container]' \
          'MANYFORGE_ROS2_CONTROL_HOT_PATH_ITERATIONS: delivered feedback messages (default 10000)' \
          'MANYFORGE_ROS2_CONTROL_IDLE_WINDOW_MS: unchanged-sample window (default 1000)' \
          'MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT: optional archival JSON file' \
          'MANYFORGE_DIAGNOSTIC_IMAGE: required existing qualified ROS-full image' \
          'MANYFORGE_ROS2_CONTROL_HOT_PATH_VARIANT: diagnostic graph variant' \
          'MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_DIR: in-container graph directory' \
          'MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_TESTING: ON/OFF matching the graph (default ON)'
        exit 0 ;;
    '') ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
esac
if (( $# )); then printf 'Unexpected arguments\n' >&2; exit 2; fi

run_inside_container() {
    local workspace="${WORKSPACE:-/workspace}"
    local build_dir="${MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_DIR:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/ros-integration}"
    local ros_distro="${ROS_DISTRO:?Selected ROS-full image must set ROS_DISTRO}"
    local build_testing="${MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_TESTING:-ON}"
    local venv="${MANYFORGE_VENV:-/opt/manyforge/venv}"
    local pinocchio_dir pinocchio_prefix
    case "${build_testing}" in
        ON|OFF) ;;
        *) printf 'Build testing must be ON or OFF\n' >&2; return 2 ;;
    esac
    set +u
    # shellcheck disable=SC1090
    source "/opt/ros/${ros_distro}/setup.bash"
    if [[ -f "${venv}/bin/activate" ]]; then
        # shellcheck disable=SC1091
        source "${venv}/bin/activate"
    fi
    set -u
    pinocchio_dir=$(find "${venv}" /usr -name pinocchioConfig.cmake -printf '%h\n' 2>/dev/null | head -1 || true)
    if [[ -z "${pinocchio_dir}" ]]; then
        printf 'pinocchioConfig.cmake missing from selected ROS-full toolchain\n' >&2
        return 2
    fi
    pinocchio_prefix=$(dirname "$(dirname "$(dirname "${pinocchio_dir}")")")
    export CMAKE_PREFIX_PATH="/usr/local:${pinocchio_prefix}:${CMAKE_PREFIX_PATH:-}"
    cmake -S "${workspace}/scripts/ros/ros-integration" -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="${build_testing}" >&2
    cmake --build "${build_dir}" --target perf_ros2_control_hot_paths \
        --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" >&2
    "${build_dir}/perf_ros2_control_hot_paths"
}

if [[ "${IN_CONTAINER}" == true ]]; then
    run_inside_container
    exit $?
fi
if [[ -z "${MANYFORGE_DIAGNOSTIC_IMAGE:-}" ]]; then
    printf 'Set MANYFORGE_DIAGNOSTIC_IMAGE to an existing qualified ROS-full image\n' >&2
    exit 2
fi
# The existing launcher owns source/image identity, compiler cache and tree lock.
# It never silently selects another running container or builds an image.
# JSON is written on the host after extraction from the evidence line.
set +e
result=$("${REPO_ROOT}/scripts/diagnostic-build.sh" \
    "${MANYFORGE_ROS2_CONTROL_HOT_PATH_VARIANT:-ros-hot-path-report-release}" -- env \
    "MANYFORGE_ROS2_CONTROL_HOT_PATH_ITERATIONS=${MANYFORGE_ROS2_CONTROL_HOT_PATH_ITERATIONS:-10000}" \
    "MANYFORGE_ROS2_CONTROL_IDLE_WINDOW_MS=${MANYFORGE_ROS2_CONTROL_IDLE_WINDOW_MS:-1000}" \
    "MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_TESTING=${MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_TESTING:-ON}" \
    "MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_DIR=${MANYFORGE_ROS2_CONTROL_HOT_PATH_BUILD_DIR:-/runner/build/ros-integration}" \
    MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT= \
    bash /workspace/scripts/run-ros2-control-hot-path-qualification.sh --in-container)
status=$?
set -e
printf '%s\n' "${result}"
if [[ "$status" == 0 && -n "${MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT:-}" ]]; then
    printf '%s\n' "${result}" | sed -n 's/^MANYFORGE_QUALIFICATION_EVIDENCE //p' \
        > "${MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT}"
fi
exit "${status}"
