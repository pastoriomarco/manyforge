#!/usr/bin/env bash
# demo-isaac-ros2-control.sh — bring up ManyForge Composer against Isaac Sim.
#
# This is the Isaac execution-lane companion to demo-assistant-known-good.sh.
# Isaac Sim may be started before or after this script. A ROS supervisor loads
# the Composer program/deployment once Composer is healthy, keeps the command
# gate closed until Isaac/controller readiness is observed, then asks Composer
# to reset so the backend commands the simulator home pose.
#
# ManyForge starts a topic_based_ros2_control bridge that exposes the Isaac arm
# as /isaac_arm_controller/follow_joint_trajectory, then starts ROS-capable
# Composer. A supervisor loads the saved Isaac light scene program/deployment
# once Composer is healthy so the backend-authored scene and Viser HMI can be
# shown before Isaac starts. This demo uses the simulator-commanded profile:
# ManyForge's backend/authored state is the source of truth, and Isaac follows
# the streamed controller command. Raw Isaac/controller feedback remains
# diagnostic/tracking telemetry. Robotiq
# commands use the standard GripperCommand action; the Isaac-only bridge
# translates that action into the scene's raw /isaac_gripper_state JointState
# command topic.
#
# Usage:
#   ./scripts/demo-isaac-ros2-control.sh start
#   ./scripts/demo-isaac-ros2-control.sh restart
#   ./scripts/demo-isaac-ros2-control.sh status
#   ./scripts/demo-isaac-ros2-control.sh smoke
#   ./scripts/demo-isaac-ros2-control.sh stop
#
# Useful overrides:
#   REBUILD=true|false|auto      default: auto
#   LOAD_ARTIFACTS=true|false   default: true (supervisor auto-load)
#   STOP_KNOWN_CONFLICTS=true|false  default: true
#   COMPOSER_URL=http://...     default: http://127.0.0.1:9000
#   HMI_PORT=8081               default: 8081

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANYFORGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
COMPOSE=(docker compose --profile ros-full)

COMPOSER_URL="${COMPOSER_URL:-http://127.0.0.1:9000}"
HMI_PORT="${HMI_PORT:-8081}"
HMI_URL="${HMI_URL:-http://127.0.0.1:${HMI_PORT}}"
PROGRAM_PATH="${PROGRAM_PATH:-/workspace/examples/isaac_manipulator_scene_light.program.yaml}"
DEPLOYMENT_PATH="${DEPLOYMENT_PATH:-/workspace/examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml}"
REBUILD="${REBUILD:-auto}"
LOAD_ARTIFACTS="${LOAD_ARTIFACTS:-true}"
STOP_KNOWN_CONFLICTS="${STOP_KNOWN_CONFLICTS:-true}"
HEALTH_TIMEOUT_S="${HEALTH_TIMEOUT_S:-60}"
HMI_STARTUP_WAIT_S="${HMI_STARTUP_WAIT_S:-2}"
ROS_DISCOVERY_TIMEOUT_S="${ROS_DISCOVERY_TIMEOUT_S:-45}"
TOPIC_BASED_PKG="${TOPIC_BASED_PKG:-topic_based_ros2_control}"
ARM_ACTION="${ARM_ACTION:-/isaac_arm_controller/follow_joint_trajectory}"
GRIPPER_ACTION="${GRIPPER_ACTION:-/robotiq_gripper_controller/gripper_cmd}"
ISAAC_JOINT_STATE_TOPIC="${ISAAC_JOINT_STATE_TOPIC:-/isaac_joint_states}"
ARM_JOINT_STATE_TOPIC="${ARM_JOINT_STATE_TOPIC:-/joint_states}"
ARM_CONTROLLER_STATE_TOPIC="${ARM_CONTROLLER_STATE_TOPIC:-/isaac_arm_controller/controller_state}"

usage() {
    cat <<EOF
Usage: $0 {start|restart|status|smoke|load|recover|stop}

Environment:
  REBUILD=${REBUILD}                  # true | false | auto
  LOAD_ARTIFACTS=${LOAD_ARTIFACTS}
  STOP_KNOWN_CONFLICTS=${STOP_KNOWN_CONFLICTS}
  COMPOSER_URL=${COMPOSER_URL}
  HMI_PORT=${HMI_PORT}
  PROGRAM_PATH=${PROGRAM_PATH}
  DEPLOYMENT_PATH=${DEPLOYMENT_PATH}
  ARM_ACTION=${ARM_ACTION}
  GRIPPER_ACTION=${GRIPPER_ACTION}
  ISAAC_JOINT_STATE_TOPIC=${ISAAC_JOINT_STATE_TOPIC}
  ARM_JOINT_STATE_TOPIC=${ARM_JOINT_STATE_TOPIC}
  ARM_CONTROLLER_STATE_TOPIC=${ARM_CONTROLLER_STATE_TOPIC}

This script starts the ManyForge side. Isaac Sim can be started before or after
it, as long as the manyforge collider service and ROS streaming are eventually
enabled:
  - isaac-topic-control: topic_based_ros2_control bridge
  - composer-ros: Composer using the ROS-capable runtime image

The current preview controls the UR arm through ROS2/JTC and commands the
Robotiq gripper through a standard control_msgs/GripperCommand action. The
Isaac bridge launched by this script owns the raw /isaac_gripper_state topic.
EOF
}

step() {
    printf '\n==> %s\n' "$*"
}

cd "${MANYFORGE_ROOT}"

port_listeners() {
    local port="$1"
    ss -ltnp 2>/dev/null | grep -E ":${port}\\b" || true
}

stop_known_conflicts() {
    if [[ "${STOP_KNOWN_CONFLICTS}" != "true" ]]; then
        return 0
    fi
    # The assistant known-good lane uses a fixed container name and the same
    # Composer/HMI ports. The Isaac lane is mutually exclusive with it because
    # Composer must run from the ROS-capable image.
    docker rm -f manyforge-e2e-composer >/dev/null 2>&1 || true
}

preflight_ports() {
    local busy=0
    local port
    for port in 9000 "${HMI_PORT}"; do
        if [[ -n "$(port_listeners "${port}")" ]]; then
            echo "Port ${port} is already in use; cannot start composer-ros cleanly." >&2
            port_listeners "${port}" >&2
            busy=1
        fi
    done
    if (( busy != 0 )); then
        cat >&2 <<EOF

Stop the existing Composer/HMI process, or rerun with STOP_KNOWN_CONFLICTS=true
if it is the standard manyforge-e2e-composer container.
EOF
        exit 1
    fi
}

ros_shell() {
    "${COMPOSE[@]}" run --rm --no-deps ros-jazzy-full bash -lc "$*"
}

build_ros2_control_runtime() {
    step "Building ROS-enabled ManyForge native runtime"
    ros_shell "./scripts/ros/build-ros2-control-runtime.sh"
}

image_has_isaac_control_runtime() {
    ros_shell "source /opt/ros/jazzy/setup.bash; source /opt/manyforge/topic_based_ros2_control/install/setup.bash 2>/dev/null || true; ros2 pkg prefix ${TOPIC_BASED_PKG} >/dev/null 2>&1; python3 -c 'import rclpy; from control_msgs.action import GripperCommand; from sensor_msgs.msg import JointState'" >/dev/null 2>&1
}

maybe_build_images() {
    case "${REBUILD}" in
        true)
            step "Building ROS-capable image"
            "${COMPOSE[@]}" build ros-jazzy-full composer-ros isaac-topic-control
            ;;
        false)
            step "Skipping image build (REBUILD=false)"
            ;;
        auto)
            if image_has_isaac_control_runtime; then
                step "ROS image already contains ${TOPIC_BASED_PKG} and gripper-action Python deps"
            else
                step "ROS image missing Isaac ros2_control runtime deps; rebuilding"
                "${COMPOSE[@]}" build ros-jazzy-full composer-ros isaac-topic-control
            fi
            ;;
        *)
            echo "Invalid REBUILD=${REBUILD}; use true, false, or auto." >&2
            exit 2
            ;;
    esac
}

wait_for_composer() {
    step "Waiting for Composer at ${COMPOSER_URL}"
    local deadline=$((SECONDS + HEALTH_TIMEOUT_S))
    until curl -fsS "${COMPOSER_URL}/api/health" >/dev/null 2>&1; do
        if (( SECONDS >= deadline )); then
            echo "Composer did not become healthy within ${HEALTH_TIMEOUT_S}s." >&2
            return 1
        fi
        sleep 1
    done
}

wait_for_hmi() {
    if [[ "${LOAD_ARTIFACTS}" != "true" ]]; then
        step "Skipping Viser HMI readiness check (LOAD_ARTIFACTS=false)"
        return 0
    fi
    step "Checking Viser HMI on ${HMI_URL}"
    local deadline=$((SECONDS + HMI_STARTUP_WAIT_S))
    until [[ -n "$(port_listeners "${HMI_PORT}")" ]]; do
        if (( SECONDS >= deadline )); then
            cat <<EOF
Viser HMI is not listening yet. This usually means the supervisor has not loaded
the program/deployment yet. Composer can load the scene before Isaac starts;
execution remains gated until Isaac/controller readiness is observed.
EOF
            return 0
        fi
        sleep 1
    done
}

wait_for_isaac_joint_states() {
    step "Waiting for ${ISAAC_JOINT_STATE_TOPIC}"
    local deadline=$((SECONDS + ROS_DISCOVERY_TIMEOUT_S))
    until ros_shell "source /opt/ros/jazzy/setup.bash; timeout 3 ros2 topic echo --once ${ISAAC_JOINT_STATE_TOPIC} >/dev/null 2>&1"; do
        if (( SECONDS >= deadline )); then
            cat >&2 <<EOF
Timed out waiting for ${ISAAC_JOINT_STATE_TOPIC}.

Check that Isaac Sim is running, simulation playback is active, and ROS
publishing is enabled for the manipulator scene.
EOF
            return 1
        fi
        sleep 1
    done
}

wait_for_arm_action() {
    step "Waiting for ${ARM_ACTION}"
    local deadline=$((SECONDS + ROS_DISCOVERY_TIMEOUT_S))
    until ros_shell "source /opt/ros/jazzy/setup.bash; ros2 action list | grep -Fx ${ARM_ACTION} >/dev/null"; do
        if (( SECONDS >= deadline )); then
            cat >&2 <<EOF
Timed out waiting for ${ARM_ACTION}.

Check the isaac-topic-control logs:
  docker compose --profile ros-full logs isaac-topic-control
EOF
            return 1
        fi
        sleep 1
    done
}

wait_for_gripper_action() {
    step "Waiting for ${GRIPPER_ACTION}"
    local deadline=$((SECONDS + ROS_DISCOVERY_TIMEOUT_S))
    until ros_shell "source /opt/ros/jazzy/setup.bash; ros2 action list | grep -Fx ${GRIPPER_ACTION} >/dev/null"; do
        if (( SECONDS >= deadline )); then
            cat >&2 <<EOF
Timed out waiting for ${GRIPPER_ACTION}.

Check the isaac-topic-control logs:
  docker compose --profile ros-full logs isaac-topic-control
EOF
            return 1
        fi
        sleep 1
    done
}

wait_for_arm_controller_state() {
    step "Waiting for ${ARM_CONTROLLER_STATE_TOPIC}"
    local deadline=$((SECONDS + ROS_DISCOVERY_TIMEOUT_S))
    until ros_shell "source /opt/ros/jazzy/setup.bash; timeout 3 ros2 topic echo --once ${ARM_CONTROLLER_STATE_TOPIC} >/dev/null 2>&1"; do
        if (( SECONDS >= deadline )); then
            cat >&2 <<EOF
Timed out waiting for ${ARM_CONTROLLER_STATE_TOPIC}.

The arm action may exist, but ManyForge expects controller-owned state for
execution freshness. Check the isaac-topic-control logs and controller config.
EOF
            return 1
        fi
        sleep 1
    done
}

load_artifacts() {
    if [[ "${LOAD_ARTIFACTS}" != "true" ]]; then
        step "Skipping Composer artifact load (LOAD_ARTIFACTS=false)"
        return 0
    fi
    step "Loading Isaac light scene program and ROS deployment"
    local payload
    local response_file
    local status
    payload="$(PROGRAM_PATH="${PROGRAM_PATH}" DEPLOYMENT_PATH="${DEPLOYMENT_PATH}" python3 - <<'PY'
import json
import os

print(json.dumps({
    "path": os.environ["PROGRAM_PATH"],
    "deploymentPath": os.environ["DEPLOYMENT_PATH"],
    "forceDiscardOverrides": True,
}))
PY
)"
    response_file="$(mktemp)"
    status="$(curl -sS -o "${response_file}" -w "%{http_code}" \
        -X POST "${COMPOSER_URL}/api/program/load" \
        -H "Content-Type: application/json" \
        --data-binary "${payload}" || true)"
    if [[ "${status}" -lt 200 || "${status}" -ge 300 ]]; then
        echo "Composer artifact load failed with HTTP ${status}:" >&2
        cat "${response_file}" >&2
        echo >&2
        rm -f "${response_file}"
        return 1
    fi
    rm -f "${response_file}"
}

start() {
    stop_known_conflicts
    "${COMPOSE[@]}" stop composer-ros isaac-topic-control >/dev/null 2>&1 || true
    preflight_ports
    maybe_build_images
    build_ros2_control_runtime
    step "Starting Isaac ros2_control bridge and ROS-capable Composer"
    export COMPOSER_URL PROGRAM_PATH DEPLOYMENT_PATH
    export SUPERVISOR_AUTO_LOAD="${LOAD_ARTIFACTS}"
    "${COMPOSE[@]}" up -d isaac-topic-control composer-ros
    wait_for_composer
    wait_for_hmi
    status
    cat <<EOF

ManyForge Isaac ros2_control preview services are running.
Composer UI: ${COMPOSER_URL}
Viser HMI:   ${HMI_URL}

If Isaac is already publishing ROS, the supervisor will load the program and
reset the simulator to the backend-authored home pose. If Isaac is not running
yet, start Isaac playback and the supervisor will recover without restarting
ManyForge.

Run a non-destructive action smoke with:
  $0 smoke
EOF
}

stop() {
    step "Stopping Isaac ros2_control preview services"
    "${COMPOSE[@]}" stop composer-ros isaac-topic-control >/dev/null
}

status() {
    step "Service status"
    "${COMPOSE[@]}" ps composer-ros isaac-topic-control
    printf '\nComposer health: '
    if curl -fsS "${COMPOSER_URL}/api/health" >/dev/null 2>&1; then
        echo "ok"
    else
        echo "unreachable"
    fi
    printf 'Viser HMI:       '
    if [[ -n "$(port_listeners "${HMI_PORT}")" ]]; then
        echo "${HMI_URL}"
    else
        echo "not listening"
    fi
    printf 'Arm action:      '
    if ros_shell "source /opt/ros/jazzy/setup.bash; ros2 action list | grep -Fx ${ARM_ACTION} >/dev/null" >/dev/null 2>&1; then
        echo "${ARM_ACTION}"
    else
        echo "not discovered"
    fi
    printf 'Gripper action:  '
    if ros_shell "source /opt/ros/jazzy/setup.bash; ros2 action list | grep -Fx ${GRIPPER_ACTION} >/dev/null" >/dev/null 2>&1; then
        echo "${GRIPPER_ACTION}"
    else
        echo "not discovered"
    fi
    printf 'Controller state: '
    if ros_shell "source /opt/ros/jazzy/setup.bash; timeout 2 ros2 topic echo --once ${ARM_CONTROLLER_STATE_TOPIC} >/dev/null 2>&1" >/dev/null 2>&1; then
        echo "${ARM_CONTROLLER_STATE_TOPIC}"
    else
        echo "not discovered"
    fi
    printf 'Isaac state:      '
    if ros_shell "source /opt/ros/jazzy/setup.bash; timeout 2 ros2 topic echo --once ${ISAAC_JOINT_STATE_TOPIC} >/dev/null 2>&1" >/dev/null 2>&1; then
        echo "${ISAAC_JOINT_STATE_TOPIC}"
    else
        echo "not discovered"
    fi
    printf 'Supervisor:       '
    local supervisor_status
    supervisor_status="$(ros_shell "source /opt/ros/jazzy/setup.bash; timeout 5 ros2 topic echo --once /manyforge/isaac_demo_supervisor/status std_msgs/msg/String --field data 2>/dev/null" 2>/dev/null | sed '/^---$/d' | tail -n 1 || true)"
    if [[ -n "${supervisor_status}" ]]; then
        echo "${supervisor_status}"
    else
        echo "not discovered"
    fi
}

smoke() {
    wait_for_isaac_joint_states
    wait_for_arm_action
    wait_for_gripper_action
    wait_for_arm_controller_state
    step "Sending a tiny reversible arm trajectory to ${ARM_ACTION}"
    ros_shell "source /opt/ros/jazzy/setup.bash; python3 /workspace/scripts/ros/smoke_isaac_arm_action.py --action-name ${ARM_ACTION} --joint-state-topic ${ARM_JOINT_STATE_TOPIC}"
}

case "${1:-}" in
    start) start ;;
    restart) stop; start ;;
    status) status ;;
    smoke) smoke ;;
    load|recover)
        wait_for_isaac_joint_states
        wait_for_arm_action
        wait_for_gripper_action
        wait_for_arm_controller_state
        load_artifacts
        ;;
    stop) stop ;;
    -h|--help|help|"") usage ;;
    *)
        usage >&2
        exit 2
        ;;
esac
