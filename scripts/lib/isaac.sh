# shellcheck shell=bash
# scripts/lib/isaac.sh — Isaac/ROS readiness probes for the isaac-sim lane.
#
# These functions assume `compose_set_lane isaac-sim` has populated
# COMPOSE_CMD with the ros-full profile. ros_shell runs a one-off
# ros-jazzy-full container that has the same ROS env as the live
# composer-ros / isaac-topic-control services, so action-list / topic-echo
# probes see the right DDS namespace.

if [[ -n "${_MANYFORGE_LIB_ISAAC:-}" ]]; then return 0; fi
_MANYFORGE_LIB_ISAAC=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"
source "$(dirname "${BASH_SOURCE[0]}")/compose.sh"
source "$(dirname "${BASH_SOURCE[0]}")/wait.sh"

ROS_DISCOVERY_TIMEOUT_S="${ROS_DISCOVERY_TIMEOUT_S:-45}"
ARM_ACTION="${ARM_ACTION:-/isaac_arm_controller/follow_joint_trajectory}"
GRIPPER_ACTION="${GRIPPER_ACTION:-/robotiq_gripper_controller/gripper_cmd}"
ISAAC_JOINT_STATE_TOPIC="${ISAAC_JOINT_STATE_TOPIC:-/isaac_joint_states}"
ARM_JOINT_STATE_TOPIC="${ARM_JOINT_STATE_TOPIC:-/joint_states}"
ARM_CONTROLLER_STATE_TOPIC="${ARM_CONTROLLER_STATE_TOPIC:-/isaac_arm_controller/controller_state}"
TOPIC_BASED_PKG="${TOPIC_BASED_PKG:-topic_based_ros2_control}"

ros_shell() {
    "${COMPOSE_CMD[@]}" run --rm --no-deps ros-jazzy-full bash -lc "$*"
}

build_ros2_control_runtime() {
    step "Building ROS-enabled ManyForge native runtime (one-time per image)"
    ros_shell "./scripts/ros/build-ros2-control-runtime.sh"
}

# Shared log tail for all isaac smoke probes — isaac-topic-control is the
# compose service that hosts the ROS bridge and isaac_demo_supervisor.
isaac_topic_control_logs() {
    if declare -p COMPOSE_CMD >/dev/null 2>&1; then
        "${COMPOSE_CMD[@]}" logs --tail 20 isaac-topic-control 2>&1 || true
    fi
}

# Probe functions — return 0 when ready, non-zero when not. Called every
# 1 s by wait_with_progress; each invocation runs a fresh ros_shell.
isaac_joint_states_ready() {
    ros_shell "source /opt/ros/jazzy/setup.bash; timeout 3 ros2 topic echo --once ${ISAAC_JOINT_STATE_TOPIC} >/dev/null 2>&1"
}

arm_action_ready() {
    ros_shell "source /opt/ros/jazzy/setup.bash; ros2 action list | grep -Fx ${ARM_ACTION} >/dev/null"
}

gripper_action_ready() {
    ros_shell "source /opt/ros/jazzy/setup.bash; ros2 action list | grep -Fx ${GRIPPER_ACTION} >/dev/null"
}

arm_controller_state_ready() {
    ros_shell "source /opt/ros/jazzy/setup.bash; timeout 3 ros2 topic echo --once ${ARM_CONTROLLER_STATE_TOPIC} >/dev/null 2>&1"
}

# wait_for_* — called by `smoke` only (not `start`). Normal start delegates
# external-readiness reporting to isaac_demo_supervisor, which writes rows
# to /api/runtime/external/isaac_demo_supervisor as Isaac Sim publishes.
wait_for_isaac_joint_states() {
    wait_with_progress \
        "${ISAAC_JOINT_STATE_TOPIC}" \
        "${ROS_DISCOVERY_TIMEOUT_S}" \
        "Isaac Sim must be running with simulation playback active and ROS publishing enabled." \
        isaac_topic_control_logs \
        isaac_joint_states_ready
}

wait_for_arm_action() {
    wait_with_progress \
        "${ARM_ACTION}" \
        "${ROS_DISCOVERY_TIMEOUT_S}" \
        "If this stalls, check isaac-topic-control logs: ${COMPOSE_CMD[*]:-docker compose --profile ros-full} logs isaac-topic-control" \
        isaac_topic_control_logs \
        arm_action_ready
}

wait_for_gripper_action() {
    wait_with_progress \
        "${GRIPPER_ACTION}" \
        "${ROS_DISCOVERY_TIMEOUT_S}" \
        "Gripper action server is part of isaac-topic-control. Tail its logs if this stalls." \
        isaac_topic_control_logs \
        gripper_action_ready
}

wait_for_arm_controller_state() {
    wait_with_progress \
        "${ARM_CONTROLLER_STATE_TOPIC}" \
        "${ROS_DISCOVERY_TIMEOUT_S}" \
        "Controller state is published once Isaac is running and topic_based_ros2_control is loaded." \
        isaac_topic_control_logs \
        arm_controller_state_ready
}

# Does the ros-jazzy-full image exist locally? Derives the canonical name
# from `docker compose config --images` so this honors COMPOSE_PROJECT_NAME
# overrides instead of hard-coding `manyforge-ros-jazzy-full`.
#
# Used as an early gate in maybe_build_isaac_image. WITHOUT this gate, the
# `isaac_image_runtime_ready` probe below silently triggers an implicit
# `docker compose run` build on a missing image — and the probe redirects
# stdout/stderr to /dev/null, so the operator stares at a black hole for
# 10+ minutes instead of seeing compose's actual build progress.
isaac_image_present() {
    local image_name
    image_name=$("${COMPOSE_CMD[@]}" config --images 2>/dev/null \
        | awk '/ros-jazzy-full(:|$)/ {print; exit}')
    [[ -n "${image_name}" ]] && docker image inspect "${image_name}" >/dev/null 2>&1
}

# image_has_isaac_runtime adapter — preflight.sh uses ros_shell via a callable;
# we wire it here so the call site reads naturally. Assumes the image already
# exists; callers must gate on `isaac_image_present` first to avoid the
# silent-build pitfall documented above.
isaac_image_runtime_ready() {
    local probe='source /opt/ros/jazzy/setup.bash; source /opt/manyforge/topic_based_ros2_control/install/setup.bash 2>/dev/null || true; ros2 pkg prefix '"${TOPIC_BASED_PKG}"' >/dev/null 2>&1; python3 -c "import rclpy; from control_msgs.action import GripperCommand; from sensor_msgs.msg import JointState"'
    ros_shell "${probe}" >/dev/null 2>&1
}

maybe_build_isaac_image() {
    local rebuild="${REBUILD:-auto}"
    case "${rebuild}" in
        true)
            step "Building ROS-capable image (REBUILD=true)"
            "${COMPOSE_CMD[@]}" build ros-jazzy-full composer-ros isaac-topic-control
            ;;
        false)
            info "Skipping image build (REBUILD=false)"
            ;;
        auto)
            if ! isaac_image_present; then
                # Cold path: image is not on disk. Go straight to the
                # visible-output build path. Running the runtime-readiness
                # probe here would trigger an implicit build under the
                # probe's /dev/null redirect — exactly the silent black-
                # hole behavior this gate exists to prevent.
                step "ROS image not present; building"
                "${COMPOSE_CMD[@]}" build ros-jazzy-full composer-ros isaac-topic-control
            elif isaac_image_runtime_ready; then
                pass "ROS image already contains ${TOPIC_BASED_PKG} and gripper-action Python deps"
            else
                step "ROS image missing Isaac ros2_control runtime deps; rebuilding"
                "${COMPOSE_CMD[@]}" build ros-jazzy-full composer-ros isaac-topic-control
            fi
            ;;
        *)
            fail "Invalid REBUILD=${rebuild}; use true|false|auto"
            return 2
            ;;
    esac
}

start_isaac_services() {
    step "Starting Isaac ros2_control bridge and ROS-capable Composer"
    # SUPERVISOR_AUTO_LOAD lets the isaac_demo_supervisor inside the
    # isaac-topic-control container POST /api/program/load itself once
    # Composer is healthy. PROGRAM_PATH and DEPLOYMENT_PATH MUST be
    # exported here — docker-compose.yaml declares them as env-var
    # parameters on the isaac-topic-control service (with fallback
    # defaults to the non-assistant deployment). Without this export,
    # the wizard's --scenario isaac-assistant selection would silently
    # be replaced by the compose default at load time.
    #
    # LOAD_ARTIFACTS is the operator-facing escape hatch (inherited from
    # the old demo-isaac-ros2-control.sh): setting LOAD_ARTIFACTS=false
    # brings up the compose services without the supervisor auto-loading
    # the program/deployment, so an operator can drive the load step
    # manually through the Composer UI or curl. Default is true (the
    # original behavior).
    export COMPOSER_URL
    export SUPERVISOR_AUTO_LOAD="${LOAD_ARTIFACTS:-true}"
    export PROGRAM_PATH
    export DEPLOYMENT_PATH
    "${COMPOSE_CMD[@]}" up -d isaac-topic-control composer-ros
}

stop_isaac_services() {
    step "Stopping Isaac ros2_control preview services"
    "${COMPOSE_CMD[@]}" stop composer-ros isaac-topic-control >/dev/null 2>&1 || true
}
