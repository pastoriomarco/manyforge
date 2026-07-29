# shellcheck shell=bash
# scripts/lib/preflight.sh — port + docker + image checks.
#
# Lifted (with consistent naming) from demo-isaac-ros2-control.sh and
# demo-assistant-known-good.sh. The check functions return 0/1 so callers
# can chain or branch; the preflight_* helpers print operator-facing
# messages but still return a status code.

if [[ -n "${_MANYFORGE_LIB_PREFLIGHT:-}" ]]; then return 0; fi
_MANYFORGE_LIB_PREFLIGHT=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"

port_listeners() {
    local port="$1"
    ss -ltnp 2>/dev/null | grep -E ":${port}\\b" || true
}

port_is_listening() {
    local port="$1"
    [[ -n "$(port_listeners "${port}")" ]]
}

docker_running() {
    docker info >/dev/null 2>&1
}

container_running() {
    local name="$1"
    docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "${name}"
}

pid_file_alive() {
    local path="$1"
    [[ -f "${path}" ]] || return 1
    local pid
    pid="$(cat "${path}" 2>/dev/null || true)"
    [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1
}

preflight_managed_components() {
    local found=0
    local log_dir="${LOG_DIR:-/tmp/manyforge-assistant-e2e}"
    local names

    # Anchor the regex to compose's actual naming pattern:
    #   <optional-project-prefix-with-trailing-dash><service-name><optional-scale-suffix>
    # where the scale suffix is `-N` (N a positive integer) and the
    # service-name is one of our compose services. End-anchored so
    # operator containers like `my-composer-ros-test` or
    # `random-composer-ros-suffix` are NOT claimed as ours: the trailing
    # `-test` / `-suffix` would have to match `-[0-9]+`, which they don't.
    names="$(docker ps --format '{{.Names}}' 2>/dev/null \
        | grep -E '^(manyforge-e2e-composer|manyforge-e2e-vllm|manyforge-real-robot-owner|manyforge-real-robot-composer)$|^(.*-)?(composer-ros|isaac-topic-control)(-[0-9]+)?$' \
        || true)"
    if [[ -n "${names}" ]]; then
        warn "Launcher-managed Docker components are already running:"
        printf '%s\n' "${names}" >&2
        found=1
    fi

    local pid_file
    for pid_file in \
        "${log_dir}/vllm-proxy.pid" \
        "${log_dir}/known-good-bridge.pid" \
        "${log_dir}/assistant-supervisor.pid"; do
        if pid_file_alive "${pid_file}"; then
            warn "Launcher-managed process is already running from ${pid_file} (pid $(cat "${pid_file}" 2>/dev/null))"
            found=1
        fi
    done

    if (( found != 0 )); then
        fail "Launcher-managed components are already running. Use 'scripts/launch.sh stop' or 'scripts/launch.sh restart' before starting again."
        return 1
    fi
}

# expected_port_owner PORT  — returns 0 if the busy port is owned by a
# launcher-managed service. The launcher treats that as a conflict for
# `start`; use `restart` or `stop` first for a clean lifecycle. Caller
# must have populated LANE,
# ASSISTANT, ASSISTANT_PROVIDER, COMPOSE_CMD, VLLM_CONTAINER as appropriate.
#
# Owners by port (Composer is env-overridable via COMPOSER_PORT; resolved values
# are compared, not hardcoded literals):
#   ${COMPOSER_PORT:-9000}  →  manyforge-only: manyforge-e2e-composer container
#                              isaac-sim:      composer-ros compose service
#   8000                    →  assistant=on:   vllm-proxy.pid (if START_VLLM_PROXY=true), else vLLM container
#   8050                    →  assistant=on:   vLLM container (when proxy is enabled)
#   8100                    →  assistant=on, provider=nemoclaw: bridge PID file
#   8200                    →  assistant=on, provider=openclaw: bridge PID file
expected_port_owner() {
    local port="$1"
    local container_name="${COMPOSER_CONTAINER:-manyforge-e2e-composer}"
    local composer_port="${COMPOSER_PORT:-9000}"

    # Composer port match (env-overridable). Match against the resolved value,
    # then fall through to the rest of the case for static-port owners.
    if [[ "${port}" == "${composer_port}" ]]; then
        case "${LANE:-}" in
            manyforge-only)
                container_running "${container_name}"
                return $?
                ;;
            isaac-sim)
                # compose services are named <project>-<service>-<n>;
                # match composer-ros or isaac-topic-control by suffix.
                docker ps --format '{{.Names}}' 2>/dev/null \
                    | grep -qE '(composer-ros|isaac-topic-control)'
                return $?
                ;;
            real-robot)
                container_running "${MANYFORGE_REAL_ROBOT_CONTAINER:-manyforge-real-robot-composer}"
                return $?
                ;;
            *) return 1 ;;
        esac
    fi

    case "${port}" in
        8000)
            if [[ "${ASSISTANT:-off}" != "on" ]]; then return 1; fi
            if [[ "${START_VLLM_PROXY:-true}" == "true" ]]; then
                # proxy.pid file owns 8000 when proxy is active
                [[ -f "${LOG_DIR:-/tmp/manyforge-assistant-e2e}/vllm-proxy.pid" ]] && \
                    kill -0 "$(cat "${LOG_DIR:-/tmp/manyforge-assistant-e2e}/vllm-proxy.pid")" 2>/dev/null
            else
                container_running "${VLLM_CONTAINER:-manyforge-e2e-vllm}"
            fi
            ;;
        8050)
            [[ "${ASSISTANT:-off}" == "on" ]] && \
                container_running "${VLLM_CONTAINER:-manyforge-e2e-vllm}"
            ;;
        8100)
            [[ "${ASSISTANT:-off}" == "on" && "${ASSISTANT_PROVIDER:-openclaw}" == "nemoclaw" ]] && \
                [[ -f "${BRIDGE_PID_FILE:-}" ]] && \
                kill -0 "$(cat "${BRIDGE_PID_FILE}")" 2>/dev/null
            ;;
        8200)
            [[ "${ASSISTANT:-off}" == "on" && "${ASSISTANT_PROVIDER:-openclaw}" == "openclaw" ]] && \
                [[ -f "${BRIDGE_PID_FILE:-}" ]] && \
                kill -0 "$(cat "${BRIDGE_PID_FILE}")" 2>/dev/null
            ;;
        *) return 1 ;;
    esac
}

# preflight_ports PORT1 [PORT2 …]
# Reports any port already listening. Returns 1 if any are busy. Recognised
# launcher-managed owners are called out explicitly so operators know whether
# to use `stop`/`restart` or inspect an unrelated process.
preflight_ports() {
    local ports=("$@")
    local busy=0
    local port lines
    for port in "${ports[@]}"; do
        if ! port_is_listening "${port}"; then
            continue
        fi
        if expected_port_owner "${port}"; then
            warn "Port ${port} is held by a launcher-managed service:"
        else
            warn "Port ${port} is in use by an unmanaged listener:"
        fi
        lines="$(port_listeners "${port}")"
        [[ -n "${lines}" ]] && printf '%s\n' "${lines}" >&2
        busy=1
    done
    if (( busy != 0 )); then
        return 1
    fi
    pass "No preflight port conflicts on: ${ports[*]}"
}

# image_has_isaac_runtime — does the ros-jazzy-full image carry the
# topic_based_ros2_control runtime + gripper Python deps?
# Caller supplies the compose runner so we don't import compose.sh here.
image_has_isaac_runtime() {
    local ros_shell_cmd="$1"
    local probe='source /opt/ros/jazzy/setup.bash; source /opt/manyforge/topic_based_ros2_control/install/setup.bash 2>/dev/null || true; ros2 pkg prefix topic_based_ros2_control >/dev/null 2>&1; python3 -c "import rclpy; from control_msgs.action import GripperCommand; from sensor_msgs.msg import JointState"'
    eval "${ros_shell_cmd}" "'${probe}'" >/dev/null 2>&1
}

# image_present IMAGE_REF  — true if the image exists locally.
image_present() {
    docker image inspect "$1" >/dev/null 2>&1
}

# image_has_assistant_runtime — the dev image with python + manyforge_composer.
# The dev image is shared across lanes, so this is a basic sanity check.
image_has_assistant_runtime() {
    image_present manyforge-dev:latest
}

require_docker() {
    if ! docker_running; then
        fail "Docker daemon not reachable. Start it (or check group membership) and retry."
        return 1
    fi
    pass "Docker daemon reachable."
}

# require_image IMAGE_REF [build_hint]  — fail fast if image is missing.
# build_hint is shown verbatim so the operator knows how to recover.
require_image() {
    local image="$1"
    local build_hint="${2:-}"
    if image_present "${image}"; then
        pass "Image ${image} present"
        return 0
    fi
    fail "Required image not found locally: ${image}"
    if [[ -n "${build_hint}" ]]; then
        info "Build it with: ${build_hint}"
    fi
    return 1
}
