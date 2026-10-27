#!/usr/bin/env bash
# Developer tool: standalone I9 Isaac/Jazzy ros2_control qualification.
# This is separate from the Manager-owned component launch.
#
# Isaac Sim is a separately owned laptop process.  This launcher runs in the
# source-matched Orin container, incrementally builds the retained ROS product,
# attaches the inactive controller infrastructure to the scene already opened
# by the user. It never starts Composer, loads a world or Program,
# or restores DemoSupervisor command ownership.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../.." && pwd)
WORKSPACE="${WORKSPACE:-${REPO_ROOT}}"
ROS_DISTRO="${ROS_DISTRO:-jazzy}"
BUILD_ROOT="${MANYFORGE_ROS2_CONTROL_BUILD_ROOT:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/isaac}"
ROS_OVERLAY="${MANYFORGE_ISAAC_ROS_OVERLAY:-${BUILD_ROOT}/ros-overlay}"

# Generated artifacts are required before any build or ROS initialization.
for artifact_variable in MANYFORGE_ROBOT_DESCRIPTION_FILE MANYFORGE_CONTROLLERS_FILE; do
    artifact_path="${!artifact_variable:-}"
    if [[ -z "${artifact_path}" || ! -f "${artifact_path}" || ! -r "${artifact_path}" ]]; then
        printf '%s must name a readable generated file.\n' "${artifact_variable}" >&2
        exit 2
    fi
done

# shellcheck source=../lib/realtime-resources.sh
source "${WORKSPACE}/scripts/lib/realtime-resources.sh"
# shellcheck source=../../config/ros/isaac_i9.env.sh
source "${WORKSPACE}/config/ros/isaac_i9.env.sh"

if [[ ! "${ROS_DOMAIN_ID}" =~ ^[0-9]+$ ]] || ((10#${ROS_DOMAIN_ID} > 232)); then
    printf 'ROS_DOMAIN_ID must be an integer in [0, 232].\n' >&2
    exit 2
fi
if [[ "${RMW_IMPLEMENTATION}" != rmw_fastrtps_cpp ]]; then
    printf 'I9 requires RMW_IMPLEMENTATION=rmw_fastrtps_cpp.\n' >&2
    exit 2
fi
if [[ "${ROS_AUTOMATIC_DISCOVERY_RANGE}" != LOCALHOST ]]; then
    printf 'I9 requires ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST.\n' >&2
    exit 2
fi
if [[ -z "${ROS_STATIC_PEERS}" || \
      "${ROS_STATIC_PEERS}" == *[[:space:]]* || \
      "${ROS_STATIC_PEERS}" == \;* || \
      "${ROS_STATIC_PEERS}" == *\; || \
      "${ROS_STATIC_PEERS}" == *\;\;* || \
      ${#ROS_STATIC_PEERS} -gt 4096 ]]; then
    printf 'ROS_STATIC_PEERS must be one non-empty explicit peer list.\n' >&2
    exit 2
fi

isaac_profile="${ISAAC_TOPIC_CONTROL_PROFILE:-ur10e}"
case "${isaac_profile}" in
    ur10e)
        default_component_id=isaac_ur10e_light_robot
        ;;
    lite6)
        default_component_id=isaac_lite6_robot
        ;;
    *)
        printf 'ISAAC_TOPIC_CONTROL_PROFILE must be ur10e or lite6.\n' >&2
        exit 2
        ;;
esac
component_id="${MANYFORGE_ISAAC_COMPONENT_ID:-${default_component_id}}"
if [[ ! "${component_id}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,254}$ ]]; then
    printf 'MANYFORGE_ISAAC_COMPONENT_ID must be one bounded ROS name token.\n' >&2
    exit 2
fi
runtime_namespace="/manyforge/${component_id}"

printf 'I9 DDS topology: domain=%s rmw=%s discovery=%s peers=%s runtime=%s\n' \
    "${ROS_DOMAIN_ID}" \
    "${RMW_IMPLEMENTATION}" \
    "${ROS_AUTOMATIC_DISCOVERY_RANGE}" \
    "${ROS_STATIC_PEERS}" \
    "${runtime_namespace}"

"${WORKSPACE}/scripts/ros/build-ros2-control-runtime.sh"

# Source identity only: a matching marker does not qualify runtime behavior.
if [[ ! -f /opt/manyforge/topic_based_ros2_control/install/UPSTREAM_COMMIT ]] || \
    [[ "$(</opt/manyforge/topic_based_ros2_control/install/UPSTREAM_COMMIT)" != "aa9f9ef9525a60f8becea322537dd1d6d1bf9123" ]]; then
    printf 'The selected PickNik topic_based_ros2_control install is missing or has the wrong source pin.\n' >&2
    exit 2
fi

set +u
# shellcheck disable=SC1090
source "/opt/ros/${ROS_DISTRO}/setup.bash"
# shellcheck disable=SC1091
source "/opt/manyforge/topic_based_ros2_control/install/setup.bash"
# shellcheck disable=SC1090
source "${ROS_OVERLAY}/install/setup.bash"
set -u

# Bind the selected package to the image install whose source marker was checked.
topic_based_prefix=$(ros2 pkg prefix topic_based_ros2_control)
if [[ "${topic_based_prefix}" != /opt/manyforge/topic_based_ros2_control/install ]]; then
    printf 'Isaac requires the selected PickNik image install; actual prefix=%s\n' "${topic_based_prefix}" >&2
    exit 2
fi

export PYTHONPATH="${WORKSPACE}/manyforge_ros:${WORKSPACE}/manyforge_behavior:${WORKSPACE}:${PYTHONPATH:-}"

launch_arguments=(
    "robot_description_file:=${MANYFORGE_ROBOT_DESCRIPTION_FILE}"
    "controllers_file:=${MANYFORGE_CONTROLLERS_FILE}"
    "isaac_profile:=${isaac_profile}"
    "runtime_namespace:=${runtime_namespace}"
    "controller_manager_lock_memory:=${MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY:-false}"
    "controller_manager_thread_priority:=${MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY:-50}"
    "gripper_state_freshness_timeout_s:=${ISAAC_GRIPPER_STATE_FRESHNESS_TIMEOUT_S:-1.0}"
)
if [[ -n "${ISAAC_JOINT_COMMANDS_TOPIC:-}" ]]; then
    launch_arguments+=("joint_commands_topic:=${ISAAC_JOINT_COMMANDS_TOPIC}")
fi
if [[ -n "${MANYFORGE_FRAME_PREFIX:-}" ]]; then
    launch_arguments+=("frame_prefix:=${MANYFORGE_FRAME_PREFIX}")
fi
if [[ -n "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY:-}" ]]; then
    launch_arguments+=(
        "controller_manager_cpu_affinity:=${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}"
    )
fi
if [[ -n "${ISAAC_POINTCLOUD_TOPIC:-}" ]]; then
    if [[ -z "${MANYFORGE_ISAAC_CELL_ID:-}" ]]; then
        printf 'MANYFORGE_ISAAC_CELL_ID is required with ISAAC_POINTCLOUD_TOPIC.\n' >&2
        exit 2
    fi
    launch_arguments+=(
        "pointcloud_topic:=${ISAAC_POINTCLOUD_TOPIC}"
        "pointcloud_source_cell_frame_id:=${ISAAC_POINTCLOUD_CELL_FRAME_ID:-universe}"
        "pointcloud_cell_id:=${MANYFORGE_ISAAC_CELL_ID}"
        "pointcloud_composer_base_url:=${MANYFORGE_COMPOSER_BASE_URL:-http://127.0.0.1:8000}"
        "pointcloud_owner_id:=${MANYFORGE_POINTCLOUD_OWNER_ID:-manyforge.isaac.pointcloud}"
        "pointcloud_max_points:=${MANYFORGE_POINTCLOUD_MAX_POINTS:-200000}"
        "pointcloud_minimum_period_s:=${MANYFORGE_POINTCLOUD_MINIMUM_PERIOD_S:-0.1}"
        "pointcloud_demand_poll_period_s:=${MANYFORGE_POINTCLOUD_DEMAND_POLL_PERIOD_S:-0.5}"
        "pointcloud_http_timeout_s:=${MANYFORGE_POINTCLOUD_HTTP_TIMEOUT_S:-1.0}"
    )
fi

manyforge_exec_with_nrt_affinity \
    ros2 launch manyforge_ros_bringup isaac_topic_based_control.launch.py \
    "${launch_arguments[@]}" \
    "$@"
