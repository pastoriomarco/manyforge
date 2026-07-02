#!/usr/bin/env bash
# Start the standard Isaac Sim ros2_control execution lane.
#
# This script assumes Isaac Sim is already running and exposing:
#   - /isaac_joint_states
#   - /isaac_joint_commands
#
# It starts controller_manager with topic_based_ros2_control, spawns
# /isaac_arm_controller/follow_joint_trajectory for ManyForge Composer, starts
# the fail-closed arm command gate, and starts the Isaac-only bridge that
# exposes a standard Robotiq /robotiq_gripper_controller/gripper_cmd action.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../.." && pwd)
ROS_DISTRO="${ROS_DISTRO:-jazzy}"
WORKSPACE="${WORKSPACE:-${REPO_ROOT}}"
ISAAC_ROS_WS="${ISAAC_ROS_WS:-/home/tndlux/workspaces/isaac_ros-dev}"
BUILD_IF_MISSING="${BUILD_IF_MISSING:-1}"
MANYFORGE_ROS_OVERLAY="${MANYFORGE_ROS_OVERLAY:-/tmp/manyforge-ros2-control-overlay}"

source_if_exists() {
    local path="$1"
    if [ -f "$path" ]; then
        # shellcheck disable=SC1090
        set +u
        source "$path"
        set -u
    fi
}

source_if_exists "/opt/ros/${ROS_DISTRO}/setup.bash"
source_if_exists "/opt/manyforge/topic_based_ros2_control/install/setup.bash"
source_if_exists "${ISAAC_ROS_WS}/install/setup.bash"
source_if_exists "${WORKSPACE}/install/setup.bash"

if ! ros2 pkg prefix topic_based_ros2_control >/dev/null 2>&1; then
    if [ "${BUILD_IF_MISSING}" = "1" ] && [ -d "${ISAAC_ROS_WS}/src/topic_based_ros2_control" ]; then
        echo "Building topic_based_ros2_control and manyforge_ros_bringup in ${MANYFORGE_ROS_OVERLAY}/install ..." >&2
        rm -rf "${MANYFORGE_ROS_OVERLAY}"
        mkdir -p "${MANYFORGE_ROS_OVERLAY}"
        colcon build \
            --base-paths "${ISAAC_ROS_WS}/src/topic_based_ros2_control" "${WORKSPACE}/manyforge_ros_bringup" \
            --build-base "${MANYFORGE_ROS_OVERLAY}/build" \
            --install-base "${MANYFORGE_ROS_OVERLAY}/install" \
            --packages-select topic_based_ros2_control manyforge_ros_bringup \
            --cmake-args -DBUILD_TESTING=OFF
        source_if_exists "${MANYFORGE_ROS_OVERLAY}/install/setup.bash"
    fi
fi

if ! ros2 pkg prefix topic_based_ros2_control >/dev/null 2>&1; then
    cat >&2 <<EOF
topic_based_ros2_control is not available.

Either:
  - set ISAAC_ROS_WS to a workspace whose install/setup.bash contains it, or
  - rebuild the manyforge-ros-jazzy-full image, which vendors it under /opt/manyforge.
  - keep BUILD_IF_MISSING=1 and make sure ISAAC_ROS_WS/src/topic_based_ros2_control exists.
EOF
    exit 2
fi

if ! ros2 pkg prefix manyforge_ros_bringup >/dev/null 2>&1; then
    if [ "${BUILD_IF_MISSING}" != "1" ]; then
        echo "manyforge_ros_bringup is not built. Run: colcon build --packages-select manyforge_ros_bringup" >&2
        exit 2
    fi
    echo "Building manyforge_ros_bringup in ${WORKSPACE}/install ..." >&2
    (
        cd "${WORKSPACE}"
        colcon build --packages-select manyforge_ros_bringup
    )
    source_if_exists "${WORKSPACE}/install/setup.bash"
fi

bringup_executables="$(ros2 pkg executables manyforge_ros_bringup 2>/dev/null || true)"
if ! grep -q "isaac_topic_command_gate" <<<"${bringup_executables}" || \
   ! grep -q "isaac_demo_supervisor" <<<"${bringup_executables}" || \
   ! grep -q "pose_source_snapshot_publisher" <<<"${bringup_executables}"; then
    if [ "${BUILD_IF_MISSING}" != "1" ]; then
        cat >&2 <<EOF
manyforge_ros_bringup is built, but it is stale: the
isaac_topic_command_gate, isaac_demo_supervisor, or
pose_source_snapshot_publisher executable is missing.

Rebuild with:
  colcon build --packages-select manyforge_ros_bringup
EOF
        exit 2
    fi
    echo "Rebuilding stale manyforge_ros_bringup in ${WORKSPACE}/install ..." >&2
    (
        cd "${WORKSPACE}"
        colcon build --packages-select manyforge_ros_bringup
    )
    source_if_exists "${WORKSPACE}/install/setup.bash"
fi

bringup_prefix="$(ros2 pkg prefix manyforge_ros_bringup 2>/dev/null || true)"
bringup_gate_exe="${bringup_prefix}/lib/manyforge_ros_bringup/isaac_topic_command_gate"
if [ -n "${bringup_prefix}" ] && [ -f "${bringup_gate_exe}" ] && \
   [ -n "$(find "${WORKSPACE}/manyforge_ros_bringup" -type f -newer "${bringup_gate_exe}" -print -quit 2>/dev/null)" ]; then
    if [ "${BUILD_IF_MISSING}" != "1" ]; then
        cat >&2 <<EOF
manyforge_ros_bringup source is newer than the installed Isaac launch helpers.

Rebuild with:
  colcon build --packages-select manyforge_ros_bringup
EOF
        exit 2
    fi
    echo "Rebuilding updated manyforge_ros_bringup in ${WORKSPACE}/install ..." >&2
    (
        cd "${WORKSPACE}"
        colcon build --packages-select manyforge_ros_bringup
    )
    source_if_exists "${WORKSPACE}/install/setup.bash"
fi

isaac_profile="${ISAAC_TOPIC_CONTROL_PROFILE:-}"
if [ -z "${isaac_profile}" ]; then
    case "${DEPLOYMENT_PATH:-}" in
        *lite6*) isaac_profile="lite6" ;;
        *) isaac_profile="ur10e" ;;
    esac
fi

exec ros2 launch manyforge_ros_bringup isaac_topic_based_control.launch.py \
    isaac_profile:="${isaac_profile}" \
    composer_url:="${COMPOSER_URL:-http://127.0.0.1:9000}" \
    program_path:="${PROGRAM_PATH:-/workspace/examples/isaac_manipulator_scene_light.program.yaml}" \
    deployment_path:="${DEPLOYMENT_PATH:-/workspace/examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml}" \
    supervisor_auto_load:="${SUPERVISOR_AUTO_LOAD:-true}" \
    "$@"
