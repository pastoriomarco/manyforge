#!/usr/bin/env bash
# Start the I9 mechanical Isaac/Jazzy ros2_control product.
#
# Isaac Sim is a separately owned laptop process.  This launcher runs in the
# source-matched Orin container, incrementally builds the retained ROS product,
# proves the exact world through simulation_interfaces, and only then exposes
# the JTC and command-gate adapters.  It never starts Composer, loads a Program,
# or restores DemoSupervisor command ownership.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../.." && pwd)
WORKSPACE="${WORKSPACE:-${REPO_ROOT}}"
ROS_DISTRO="${ROS_DISTRO:-jazzy}"
BUILD_ROOT="${MANYFORGE_ROS2_CONTROL_BUILD_ROOT:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/isaac}"
ROS_OVERLAY="${MANYFORGE_ISAAC_ROS_OVERLAY:-${BUILD_ROOT}/ros-overlay}"

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

set +u
# shellcheck disable=SC1090
source "/opt/ros/${ROS_DISTRO}/setup.bash"
# shellcheck disable=SC1091
source "/opt/manyforge/topic_based_ros2_control/install/setup.bash"
# shellcheck disable=SC1090
source "${ROS_OVERLAY}/install/setup.bash"
set -u

export PYTHONPATH="${WORKSPACE}/manyforge_ros:${WORKSPACE}/manyforge_behavior:${WORKSPACE}:${PYTHONPATH:-}"

launch_arguments=(
    "isaac_profile:=${isaac_profile}"
    "runtime_namespace:=${runtime_namespace}"
    "command_gate_owner_namespace:=${runtime_namespace}"
    "world_uri:=${ISAAC_WORLD_URI:-file:/home/tndlux/workspaces/isaac_ros-dev/src/isaac_sim_custom_examples/Scenario/IsaacManipulator/UniversalRobots/isaac_manipulator_scene_light.usda}"
    "expected_world_name:=${ISAAC_EXPECTED_WORLD_NAME:-isaac_manipulator_scene_light}"
    "scene_startup_timeout_s:=${ISAAC_SCENE_STARTUP_TIMEOUT_S:-180.0}"
    "scene_service_timeout_s:=${ISAAC_SCENE_SERVICE_TIMEOUT_S:-30.0}"
    "controller_manager_lock_memory:=${MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY:-false}"
    "controller_manager_thread_priority:=${MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY:-50}"
)
if [[ -n "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY:-}" ]]; then
    launch_arguments+=(
        "controller_manager_cpu_affinity:=${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}"
    )
fi
if [[ -n "${ISAAC_ADDITIONAL_REQUIRED_TOPICS:-}" ]]; then
    launch_arguments+=(
        "additional_required_topics:=${ISAAC_ADDITIONAL_REQUIRED_TOPICS}"
    )
fi
if [[ -n "${MANYFORGE_MANAGER_STATE_ROOT:-}" || \
      -n "${ISAAC_POSE_OBSERVER_SITE_CONFIG:-}" ]]; then
    if [[ -z "${MANYFORGE_MANAGER_STATE_ROOT:-}" || \
          -z "${ISAAC_POSE_OBSERVER_SITE_CONFIG:-}" ]]; then
        printf 'MANYFORGE_MANAGER_STATE_ROOT and ISAAC_POSE_OBSERVER_SITE_CONFIG must be set together.\n' >&2
        exit 2
    fi
    launch_arguments+=(
        "manager_state_root:=${MANYFORGE_MANAGER_STATE_ROOT}"
        "pose_observer_site_config:=${ISAAC_POSE_OBSERVER_SITE_CONFIG}"
    )
fi

manyforge_exec_with_nrt_affinity \
    ros2 launch manyforge_ros_bringup isaac_topic_based_control.launch.py \
    "${launch_arguments[@]}" \
    "$@"
