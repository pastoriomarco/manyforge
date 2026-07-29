# shellcheck shell=bash
# Real-robot launcher support. Vendor-specific work stays behind a resolver id.

if [[ -n "${_MANYFORGE_LIB_REAL_ROBOT:-}" ]]; then return 0; fi
_MANYFORGE_LIB_REAL_ROBOT=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"

REAL_ROBOT_OWNER_CONTAINER="${MANYFORGE_REAL_ROBOT_OWNER_CONTAINER:-manyforge-real-robot-owner}"
REAL_ROBOT_COMPOSER_CONTAINER="${MANYFORGE_REAL_ROBOT_CONTAINER:-manyforge-real-robot-composer}"
REAL_ROBOT_IMAGE="${MANYFORGE_REAL_LITE6_IMAGE:-manyforge-ros-jazzy-real-lite6:experimental}"
REAL_ROBOT_OWNER_LABEL="org.manyforge.xarm-transport-owner"
REAL_ROBOT_OWNER_ID="lite6-a:sole-owner"
REAL_ROBOT_SUPERVISED_CONTRACT="1"
REAL_ROBOT_CELL_ID="${MANYFORGE_REAL_ROBOT_CELL_ID:-lite6-a}"
REAL_ROBOT_RESOLVED_DEPLOYMENT=""
REAL_ROBOT_RUNTIME_HOST_ROOT=""
REAL_ROBOT_SUPERVISOR_CONTROL_ROOT=""
REAL_ROBOT_SUPERVISOR_PID=""
REAL_ROBOT_ACTIVE_RUN_FILE="${MANYFORGE_ROOT}/.manyforge-var/real-lite6/active-run"

real_robot_supervisor_pid_matches() {
    local pid="${1:-}" runtime_root="${2:-}" token
    [[ "${pid}" =~ ^[1-9][0-9]*$ ]] || return 1
    [[ -n "${runtime_root}" && -r "/proc/${pid}/cmdline" ]] || return 1
    local saw_module=0 saw_runtime=0
    while IFS= read -r -d '' token; do
        [[ "${token}" == "manyforge_supervision.cell_supervisor" ]] &&
            saw_module=1
        [[ "${token}" == "${runtime_root}" ]] && saw_runtime=1
    done <"/proc/${pid}/cmdline"
    [[ "${saw_module}" == "1" && "${saw_runtime}" == "1" ]]
}

real_robot_preflight() {
    if [[ "${REAL_ROBOT_RESOLVER:-}" != "xarm.lite6_supervised.v1" ]]; then
        fail "Unsupported real-robot resolver: ${REAL_ROBOT_RESOLVER:-unset}"
        return 1
    fi
    if [[ "${ASSISTANT:-off}" != "off" ]]; then
        fail "The initial real-robot commissioning lane does not enable the assistant."
        return 1
    fi
    if [[ -z "${MANYFORGE_LITE6_ROBOT_IP:-}" ]]; then
        fail "Set MANYFORGE_LITE6_ROBOT_IP to the attended Lite6 address."
        return 1
    fi
    if [[ ! "${MANYFORGE_LITE6_ROBOT_IP}" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        fail "MANYFORGE_LITE6_ROBOT_IP must be one literal IPv4 address."
        return 1
    fi
    if [[ -n "$(git -C "${MANYFORGE_ROOT}" status --porcelain=v1 --untracked-files=all)" ]]; then
        fail "ManyForge must be clean before connecting the physical lane."
        return 1
    fi
    if [[ "$(docker image inspect "${REAL_ROBOT_IMAGE}" --format '{{index .Config.Labels "org.manyforge.experimental.real-lite6"}}' 2>/dev/null || true)" != "true" ]]; then
        fail "Image is not a ManyForge experimental real-Lite6 image: ${REAL_ROBOT_IMAGE}"
        return 1
    fi
    if [[ "$(docker image inspect "${REAL_ROBOT_IMAGE}" --format '{{index .Config.Labels "org.manyforge.xarm.supervised-contract"}}' 2>/dev/null || true)" != "${REAL_ROBOT_SUPERVISED_CONTRACT}" ]]; then
        fail "Image does not provide xArm supervised contract v${REAL_ROBOT_SUPERVISED_CONTRACT}: ${REAL_ROBOT_IMAGE}"
        fail "Rebuild it with scripts/xarm/build-real-lite6-image.sh."
        return 1
    fi
    local container
    for container in \
        "${REAL_ROBOT_OWNER_CONTAINER}" \
        "${REAL_ROBOT_COMPOSER_CONTAINER}"; do
        if docker inspect "${container}" >/dev/null 2>&1; then
            fail "Container name already exists: ${container}"
            return 1
        fi
    done
    local existing_owner
    existing_owner="$(
        docker ps \
            --filter "label=${REAL_ROBOT_OWNER_LABEL}=${REAL_ROBOT_OWNER_ID}" \
            --format '{{.Names}}'
    )"
    if [[ -n "${existing_owner}" ]]; then
        fail "A Lite6 transport owner is already running: ${existing_owner}"
        return 1
    fi
}

start_real_robot_lane() {
    local provider_uuid session_id container_runtime_root owner_timeout_s
    local supervisor_epoch heartbeat_path supervisor_deadline
    read -r provider_uuid </proc/sys/kernel/random/uuid
    session_id="$(date -u +%Y%m%dT%H%M%SZ)-${provider_uuid}"
    REAL_ROBOT_RUNTIME_HOST_ROOT="${MANYFORGE_ROOT}/.manyforge-var/real-lite6/runs/${session_id}"
    REAL_ROBOT_SUPERVISOR_CONTROL_ROOT="${MANYFORGE_ROOT}/.manyforge-var/real-lite6/supervisor-control/${session_id}"
    container_runtime_root="/workspace/.manyforge-var/real-lite6/runs/${session_id}"
    mkdir -p \
        "${REAL_ROBOT_RUNTIME_HOST_ROOT}" \
        "${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}"
    chmod 700 "${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}"
    REAL_ROBOT_RESOLVED_DEPLOYMENT="${container_runtime_root}/resolved/lite6-a/commissioning.deployment.yaml"
    DEPLOYMENT_PATH="${REAL_ROBOT_RESOLVED_DEPLOYMENT}"

    info "Starting external fail-closed cell supervisor"
    PYTHONPATH="${MANYFORGE_ROOT}" nohup python3 \
        -m manyforge_supervision.cell_supervisor \
        --runtime-root "${REAL_ROBOT_RUNTIME_HOST_ROOT}" \
        --control-root "${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}" \
        --cell-id "${REAL_ROBOT_CELL_ID}" \
        --robot-id lite6-a \
        --composer-container "${REAL_ROBOT_COMPOSER_CONTAINER}" \
        --owner-container "${REAL_ROBOT_OWNER_CONTAINER}" \
        --composer-health-url \
        "http://127.0.0.1:${COMPOSER_PORT:-9000}/api/runtime/status" \
        --health-startup-grace-ms \
        "${MANYFORGE_REAL_ROBOT_HEALTH_STARTUP_GRACE_MS:-600000}" \
        --health-progress-max-age-ms \
        "${MANYFORGE_REAL_ROBOT_HEALTH_PROGRESS_MAX_AGE_MS:-3000}" \
        --health-unhealthy-grace-ms \
        "${MANYFORGE_REAL_ROBOT_HEALTH_UNHEALTHY_GRACE_MS:-2000}" \
        --health-request-timeout-ms \
        "${MANYFORGE_REAL_ROBOT_HEALTH_REQUEST_TIMEOUT_MS:-200}" \
        >"${REAL_ROBOT_RUNTIME_HOST_ROOT}/cell-supervisor.log" 2>&1 &
    REAL_ROBOT_SUPERVISOR_PID=$!
    printf '%s\n%s\n%s\n' \
        "${REAL_ROBOT_RUNTIME_HOST_ROOT}" \
        "${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}" \
        "${REAL_ROBOT_SUPERVISOR_PID}" \
        >"${REAL_ROBOT_ACTIVE_RUN_FILE}"
    chmod 600 "${REAL_ROBOT_ACTIVE_RUN_FILE}"
    heartbeat_path="${REAL_ROBOT_RUNTIME_HOST_ROOT}/cell-supervisor-heartbeat.json"
    supervisor_deadline=$((SECONDS + 10))
    while [[ ! -s "${heartbeat_path}" ]]; do
        if ! kill -0 "${REAL_ROBOT_SUPERVISOR_PID}" 2>/dev/null; then
            fail "The external cell supervisor exited during startup"
            tail -n 80 "${REAL_ROBOT_RUNTIME_HOST_ROOT}/cell-supervisor.log" >&2 || true
            return 1
        fi
        if ((SECONDS >= supervisor_deadline)); then
            fail "Timed out waiting for the external cell supervisor"
            stop_real_robot_lane
            return 1
        fi
        sleep 0.1
    done
    supervisor_epoch="$(
        python3 -c \
            'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["componentEpoch"])' \
            "${heartbeat_path}" 2>/dev/null || true
    )"
    if [[ ! "${supervisor_epoch}" =~ ^cell-supervisor-[A-Za-z0-9-]+$ ]]; then
        fail "The external cell supervisor returned an invalid component epoch"
        stop_real_robot_lane
        return 1
    fi

    owner_timeout_s="${MANYFORGE_REAL_ROBOT_OWNER_READY_TIMEOUT_S:-600}"
    if [[ ! "${owner_timeout_s}" =~ ^[1-9][0-9]*$ ]]; then
        fail "MANYFORGE_REAL_ROBOT_OWNER_READY_TIMEOUT_S must be a positive integer"
        stop_real_robot_lane
        return 1
    fi

    info "Starting observation-only real-robot control owner"
    info "  retained runtime evidence: ${REAL_ROBOT_RUNTIME_HOST_ROOT}"
    info "  robot state: preserved; ManyForge hardware/JTC start inactive"
    if ! docker run -d --rm --init \
        --name "${REAL_ROBOT_OWNER_CONTAINER}" \
        --label "${REAL_ROBOT_OWNER_LABEL}=${REAL_ROBOT_OWNER_ID}" \
        --network host \
        --ipc host \
        --cap-add SYS_NICE \
        --ulimit rtprio=99:99 \
        --ulimit memlock=-1:-1 \
        -e "ROS_DOMAIN_ID=${MANYFORGE_LITE6_ROS_DOMAIN_ID:-${ROS_DOMAIN_ID:-99}}" \
        -e ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST \
        -e MANYFORGE_LANE=real-robot \
        -e "MANYFORGE_COMPOSER_PORT=${COMPOSER_PORT:-9000}" \
        -e "MANYFORGE_LITE6_ROBOT_IP=${MANYFORGE_LITE6_ROBOT_IP}" \
        -e MANYFORGE_LITE6_RUNTIME_BINDING_CONFIRM=RESOLVE_SUPERVISED_RUNTIME \
        -e MANYFORGE_LITE6_ROBOT_INSTANCE_ID=lite6-a \
        -e "MANYFORGE_LITE6_TRANSPORT_OWNER_ID=${REAL_ROBOT_OWNER_ID}" \
        -e "MANYFORGE_LITE6_LIFECYCLE_PROVIDER_INSTANCE_ID=lite6-a:provider:${provider_uuid}" \
        -e MANYFORGE_LITE6_AUTHORITY_PROFILE=local_operator \
        -e "MANYFORGE_REAL_ROBOT_CELL_ID=${REAL_ROBOT_CELL_ID}" \
        -e "MANYFORGE_CELL_SUPERVISOR_COMPONENT_EPOCH=${supervisor_epoch}" \
        -e "MANYFORGE_CELL_SUPERVISOR_HEARTBEAT_PATH=${container_runtime_root}/cell-supervisor-heartbeat.json" \
        -e "MANYFORGE_REAL_LITE6_CELL_PATH=${REAL_ROBOT_CELL_PATH}" \
        -e "MANYFORGE_REAL_LITE6_DEPLOYMENT_TEMPLATE=${REAL_ROBOT_DEPLOYMENT_TEMPLATE}" \
        -e "MANYFORGE_REAL_LITE6_PROGRAM_PATH=${REAL_ROBOT_PROGRAM_PATH}" \
        -e "MANYFORGE_REAL_LITE6_OUTPUT_ROOT=${container_runtime_root}" \
        -e MANYFORGE_FRONTEND_SOURCE_MODE=git-head \
        -v "${MANYFORGE_ROOT}:/workspace:ro" \
        -v "${REAL_ROBOT_RUNTIME_HOST_ROOT}:${container_runtime_root}:rw" \
        -v manyforge_build-cache:/tmp/manyforge-build \
        -w /workspace \
        "${REAL_ROBOT_IMAGE}" \
        ./scripts/ros/start-lite6-real-robot-owner.sh \
        >/dev/null; then
        stop_real_robot_lane
        return 1
    fi

    local deadline=$((SECONDS + owner_timeout_s))
    while [[ ! -f "${REAL_ROBOT_RUNTIME_HOST_ROOT}/owner-ready" ]]; do
        if [[ "$(docker inspect "${REAL_ROBOT_OWNER_CONTAINER}" --format '{{.State.Running}}' 2>/dev/null || true)" != "true" ]]; then
            fail "The real-robot control owner exited during startup"
            docker logs --tail 80 "${REAL_ROBOT_OWNER_CONTAINER}" >&2 2>/dev/null || true
            stop_real_robot_lane
            return 1
        fi
        if ((SECONDS >= deadline)); then
            fail "Timed out waiting for the real-robot control owner"
            docker logs --tail 80 "${REAL_ROBOT_OWNER_CONTAINER}" >&2 2>/dev/null || true
            stop_real_robot_lane
            return 1
        fi
        sleep 0.25
    done

    info "Starting non-owner Composer service"
    if ! docker run -d --rm --init \
        --name "${REAL_ROBOT_COMPOSER_CONTAINER}" \
        --network host \
        --ipc host \
        -e "ROS_DOMAIN_ID=${MANYFORGE_LITE6_ROS_DOMAIN_ID:-${ROS_DOMAIN_ID:-99}}" \
        -e ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST \
        -e MANYFORGE_LANE=real-robot \
        -e "MANYFORGE_COMPOSER_PORT=${COMPOSER_PORT:-9000}" \
        -e "MANYFORGE_REAL_LITE6_OUTPUT_ROOT=${container_runtime_root}" \
        -e MANYFORGE_FRONTEND_SOURCE_MODE=git-head \
        -v "${MANYFORGE_ROOT}:/workspace:ro" \
        -v "${REAL_ROBOT_RUNTIME_HOST_ROOT}:${container_runtime_root}:rw" \
        -v manyforge_build-cache:/tmp/manyforge-build \
        -w /workspace \
        "${REAL_ROBOT_IMAGE}" \
        ./scripts/ros/start-lite6-real-robot-composer.sh \
        >/dev/null; then
        stop_real_robot_lane
        return 1
    fi

    if [[ "$(docker inspect "${REAL_ROBOT_COMPOSER_CONTAINER}" --format '{{.State.Running}}' 2>/dev/null || true)" != "true" ]]; then
        fail "The real-robot Composer exited during startup"
        stop_real_robot_lane
        return 1
    fi
    : >"${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}/cell-supervisor-arm"
    supervisor_deadline=$((SECONDS + 10))
    while true; do
        local supervisor_state
        supervisor_state="$(
            python3 -c \
                'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8")).get("desiredState", ""))' \
                "${heartbeat_path}" 2>/dev/null || true
        )"
        if [[ "${supervisor_state}" == "running" ]]; then
            break
        fi
        if ! kill -0 "${REAL_ROBOT_SUPERVISOR_PID}" 2>/dev/null; then
            fail "The external cell supervisor rejected the runtime domain"
            tail -n 80 "${REAL_ROBOT_RUNTIME_HOST_ROOT}/cell-supervisor.log" >&2 || true
            stop_real_robot_lane
            return 1
        fi
        if ((SECONDS >= supervisor_deadline)); then
            fail "Timed out arming the external cell supervisor"
            stop_real_robot_lane
            return 1
        fi
        sleep 0.1
    done
}

stop_real_robot_lane() {
    # The external supervisor owns normal dependency-ordered teardown. This
    # fallback is retained for a missing or failed supervisor and is never
    # classified as a verified physical stop.
    local supervisor_pid="${REAL_ROBOT_SUPERVISOR_PID:-}" supervisor_deadline
    local -a active_run=()
    if [[ -f "${REAL_ROBOT_ACTIVE_RUN_FILE}" ]]; then
        mapfile -t active_run <"${REAL_ROBOT_ACTIVE_RUN_FILE}" || true
        if [[ -z "${REAL_ROBOT_RUNTIME_HOST_ROOT}" ]]; then
            REAL_ROBOT_RUNTIME_HOST_ROOT="${active_run[0]:-}"
        fi
        if [[ -z "${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}" ]]; then
            REAL_ROBOT_SUPERVISOR_CONTROL_ROOT="${active_run[1]:-}"
        fi
        if [[ -z "${supervisor_pid}" ]]; then
            supervisor_pid="${active_run[2]:-}"
        fi
        case "${REAL_ROBOT_RUNTIME_HOST_ROOT}" in
            "${MANYFORGE_ROOT}/.manyforge-var/real-lite6/runs/"*) ;;
            *) REAL_ROBOT_RUNTIME_HOST_ROOT="" ;;
        esac
        case "${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}" in
            "${MANYFORGE_ROOT}/.manyforge-var/real-lite6/supervisor-control/"*) ;;
            *) REAL_ROBOT_SUPERVISOR_CONTROL_ROOT="" ;;
        esac
    fi
    if [[ -n "${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}" ]] &&
       real_robot_supervisor_pid_matches \
           "${supervisor_pid}" "${REAL_ROBOT_RUNTIME_HOST_ROOT}"; then
        : >"${REAL_ROBOT_SUPERVISOR_CONTROL_ROOT}/cell-supervisor-shutdown"
        supervisor_deadline=$((SECONDS + 35))
        while real_robot_supervisor_pid_matches \
                  "${supervisor_pid}" "${REAL_ROBOT_RUNTIME_HOST_ROOT}" &&
              ((SECONDS < supervisor_deadline)); do
            sleep 0.2
        done
    fi
    docker stop --time 10 "${REAL_ROBOT_COMPOSER_CONTAINER}" \
        >/dev/null 2>&1 || true
    docker stop --time 20 "${REAL_ROBOT_OWNER_CONTAINER}" \
        >/dev/null 2>&1 || true
    docker rm -f "${REAL_ROBOT_COMPOSER_CONTAINER}" >/dev/null 2>&1 || true
    docker rm -f "${REAL_ROBOT_OWNER_CONTAINER}" >/dev/null 2>&1 || true
    if real_robot_supervisor_pid_matches \
        "${supervisor_pid}" "${REAL_ROBOT_RUNTIME_HOST_ROOT}"; then
        kill "${supervisor_pid}" 2>/dev/null || true
    fi
    rm -f "${REAL_ROBOT_ACTIVE_RUN_FILE}"
}

real_robot_logs() {
    echo "real-robot control owner:"
    docker logs --tail 20 "${REAL_ROBOT_OWNER_CONTAINER}" 2>&1 || true
    echo "real-robot Composer:"
    docker logs --tail 20 "${REAL_ROBOT_COMPOSER_CONTAINER}" 2>&1 || true
    if [[ -n "${REAL_ROBOT_RUNTIME_HOST_ROOT}" ]]; then
        echo "external cell supervisor:"
        tail -n 20 "${REAL_ROBOT_RUNTIME_HOST_ROOT}/cell-supervisor.log" 2>&1 || true
    fi
}
