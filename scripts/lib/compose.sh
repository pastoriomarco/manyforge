# shellcheck shell=bash
# scripts/lib/compose.sh — docker compose wrapper that picks the profile.
#
# Lane → compose profile:
#   manyforge-only → no profile (default services)
#   isaac-sim      → ros-full   (composer-ros + isaac-topic-control)
#
# Helpers:
#   compose_cmd      — emits the array of docker-compose argv pieces.
#                      Use as: "${COMPOSE_CMD[@]}" up …
#   compose_set_lane LANE  — populates COMPOSE_CMD according to lane.
#   stop_known_conflicts   — removes stale demo containers by name.

if [[ -n "${_MANYFORGE_LIB_COMPOSE:-}" ]]; then return 0; fi
_MANYFORGE_LIB_COMPOSE=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"
source "$(dirname "${BASH_SOURCE[0]}")/preflight.sh"
source "$(dirname "${BASH_SOURCE[0]}")/realtime-resources.sh"

# launch.sh exports MANYFORGE_ROOT; fall back to deriving it from this file's
# location so compose targeting works even if this lib is sourced standalone.
: "${MANYFORGE_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# Result variable for compose_set_lane.
declare -a COMPOSE_CMD
declare -a NATIVE_COMPOSER_RT_DOCKER_ARGS
NATIVE_COMPOSER_IMAGE_REF="manyforge-dev:latest"
NATIVE_COMPOSER_IMAGE_ID=""
NATIVE_COMPOSER_BUILD_CACHE_VOLUME=""

native_composer_build_cache_volume_for_image_id() {
    local image_id="${1:-}"
    [[ "${image_id}" =~ ^sha256:[0-9a-f]{64}$ ]] || return 1
    printf 'manyforge_native-composer-build-cache-%s\n' "${image_id#sha256:}"
}

resolve_native_composer_image() {
    local image_id
    image_id="$(
        docker image inspect "${NATIVE_COMPOSER_IMAGE_REF}" \
            --format '{{.Id}}' 2>/dev/null || true
    )"
    if ! NATIVE_COMPOSER_BUILD_CACHE_VOLUME="$(
        native_composer_build_cache_volume_for_image_id "${image_id}"
    )"; then
        fail "Could not resolve immutable image identity for ${NATIVE_COMPOSER_IMAGE_REF}"
        return 1
    fi
    NATIVE_COMPOSER_IMAGE_ID="${image_id}"
}

prepare_native_composer_rt_docker_args() {
    NATIVE_COMPOSER_RT_DOCKER_ARGS=()
    if ! manyforge_rt_profile_enabled; then
        return 0
    fi
    export MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT=false
    if ! manyforge_validate_rt_resource_profile; then
        return 1
    fi
    NATIVE_COMPOSER_RT_DOCKER_ARGS=(
        --cap-add SYS_NICE
        --ulimit rtprio=99:99
        -e "MANYFORGE_RT_RESOURCE_PROFILE=${MANYFORGE_RT_RESOURCE_PROFILE}"
        -e "MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT=${MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT}"
        -e "MANYFORGE_NRT_CPU_AFFINITY=${MANYFORGE_NRT_CPU_AFFINITY}"
        -e "MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY=${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}"
        -e "MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY=${MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY}"
        -e "MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY=${MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY}"
        -e "MANYFORGE_RT_SCHED_POLICY=${MANYFORGE_RT_SCHED_POLICY}"
        -e "MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY=${MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY}"
        -e "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY=${MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY}"
        -e "MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY=${MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY}"
        -e "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY=${MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY}"
        -e "MANYFORGE_RT_STOP_CONTROL_PRIORITY=${MANYFORGE_RT_STOP_CONTROL_PRIORITY}"
        -e "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY=${MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY}"
    )
}

compose_set_lane() {
    local lane="$1"
    case "${lane}" in
        manyforge-only) export MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT=false ;;
        isaac-sim) export MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT=true ;;
        *)
            fail "compose_set_lane: unknown lane '${lane}'"
            return 1
            ;;
    esac
    # Pin --project-directory + -f so compose resolves the same project
    # ("manyforge") and services regardless of the caller's CWD. Without this,
    # invoking launch.sh from a directory with no compose file (e.g. the
    # workspace root) makes up/stop/build silently target nothing — the stop
    # path then no-ops while still printing "Stop complete", leaving orphaned
    # containers that the next preflight rejects.
    local base=(
        docker compose
        --project-directory "${MANYFORGE_ROOT}"
        -f "${MANYFORGE_ROOT}/docker-compose.yaml"
    )
    if manyforge_rt_profile_enabled; then
        manyforge_validate_rt_resource_profile
        base+=(
            -f "${MANYFORGE_ROOT}/docker-compose.realtime.yaml"
        )
    fi
    case "${lane}" in
        manyforge-only)
            COMPOSE_CMD=("${base[@]}")
            ;;
        isaac-sim)
            COMPOSE_CMD=("${base[@]}" --profile ros-full)
            ;;
        *) return 1 ;;
    esac
}

# Lane-specific service names. Centralised so the rest of the launcher
# doesn't need to remember the docker-compose service map.
compose_services_for_lane() {
    local lane="$1"
    case "${lane}" in
        manyforge-only)
            # Bring up nothing; the Composer process is launched manually by
            # composer.sh as a one-shot `dev` container (matches the existing
            # demo.sh pattern). Reason: the `composer` compose service uses a
            # built frontend image, but the demo lanes rely on the live mount
            # from the dev volume.
            printf '' ;;
        isaac-sim)
            printf 'isaac-topic-control composer-ros' ;;
        *)
            return 1 ;;
    esac
}

stop_known_conflicts() {
    # Remove stale demo containers across all lanes. Idempotent.
    stop_vllm_status_watcher
    docker rm -f manyforge-e2e-composer >/dev/null 2>&1 || true
    docker rm -f manyforge-e2e-vllm >/dev/null 2>&1 || true
}

# vLLM status watcher — out-of-band crash monitor (lib/vllm-status-watch.sh).
# Host-side sidecar, lifecycle-tied to the Composer (the reference interface) and
# decoupled from the launcher (it observes the container by name, so it survives
# a launcher swap). Writes a status file the Composer backend reads to tell
# "loading" from "crashed". Idempotent: the watcher's own pidfile no-ops repeats.
start_vllm_status_watcher() {
    local watcher; watcher="$(dirname "${BASH_SOURCE[0]}")/vllm-status-watch.sh"
    local container="${VLLM_CONTAINER:-manyforge-e2e-vllm}"
    local status_dir="${MANYFORGE_VLLM_STATUS_DIR:-/tmp/manyforge-assistant-e2e}"
    if [[ ! -x "${watcher}" ]]; then
        info "vLLM status watcher missing at ${watcher}; crash monitor disabled"
        return 0
    fi
    mkdir -p "${status_dir}" 2>/dev/null || true
    nohup "${watcher}" "${container}" "${status_dir}/vllm-status.json" "${status_dir}/vllm-intended-stop" \
        >"${status_dir}/vllm-status-watch.log" 2>&1 &
    disown 2>/dev/null || true
    info "vLLM crash monitor watching ${container} -> ${status_dir}/vllm-status.json"
}

stop_vllm_status_watcher() {
    local status_dir="${MANYFORGE_VLLM_STATUS_DIR:-/tmp/manyforge-assistant-e2e}"
    local pidfile="${status_dir}/vllm-status-watch.pid"
    if [[ -f "${pidfile}" ]]; then
        local pid; pid="$(cat "${pidfile}" 2>/dev/null || true)"
        if [[ "${pid}" =~ ^[0-9]+$ ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
        fi
    fi
    rm -f "${status_dir}/vllm-status.json" 2>/dev/null || true
}

# start_composer_dev_container — for the manyforge-only lane.
# A foreground one-shot container first builds and qualifies source-mounted
# products. Only a successful build starts the detached Composer process. This
# keeps build failures synchronous and visible instead of deleting the failed
# container and waiting for an API that can never become ready.
#
# Ports are env-overridable:
#   COMPOSER_PORT (default 9000) — Composer API + UI
# Note: the container runs with --network host, so -p HOST:CONT mappings
# are no-ops. The override path is to pass the chosen ports as env vars
# into the container and have the inline Python command read them. The
# host process binds directly on the chosen ports.
#
# Optional env (forwarded to Composer): assistant settings plus
# MANYFORGE_NODE_ROOTS, MANYFORGE_NODE_STRICT, and MANYFORGE_SCENARIO_ROOTS.
start_composer_dev_container() {
    local container_name="${COMPOSER_CONTAINER:-manyforge-e2e-composer}"
    local composer_port="${COMPOSER_PORT:-9000}"
    if ! prepare_native_composer_rt_docker_args; then
        fail "Native Composer real-time resource profile is invalid"
        return 1
    fi
    start_vllm_status_watcher  # idempotent; covers both reuse and fresh-start paths
    if container_running "${container_name}"; then
        if manyforge_rt_profile_enabled; then
            # docker run has no compose reconciliation. Recreate instead of
            # silently reusing a container whose capabilities, env, or NRT
            # affinity may belong to an older/off profile.
            info "Recreating ${container_name} to admit the strict RT profile"
            docker rm -f "${container_name}" >/dev/null 2>&1 || true
        else
            info "Reusing existing ${container_name}"
            return 0
        fi
    fi
    docker rm -f "${container_name}" >/dev/null 2>&1 || true
    if ! resolve_native_composer_image; then
        stop_vllm_status_watcher
        return 1
    fi
    local assistant_env=()
    if [[ -n "${MANYFORGE_ASSISTANT_PROVIDER:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_ASSISTANT_PROVIDER=${MANYFORGE_ASSISTANT_PROVIDER}")
    fi
    if [[ -n "${MANYFORGE_ASSISTANT_ENDPOINT_URL:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_ASSISTANT_ENDPOINT_URL=${MANYFORGE_ASSISTANT_ENDPOINT_URL}")
    fi
    if [[ -n "${MANYFORGE_ASSISTANT_TIMEOUT_S:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_ASSISTANT_TIMEOUT_S=${MANYFORGE_ASSISTANT_TIMEOUT_S}")
    fi
    if [[ -n "${MANYFORGE_NODE_ROOTS:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_NODE_ROOTS=${MANYFORGE_NODE_ROOTS}")
    fi
    if [[ -n "${MANYFORGE_NODE_STRICT:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_NODE_STRICT=${MANYFORGE_NODE_STRICT}")
    fi
    if [[ -n "${MANYFORGE_SCENARIO_ROOTS:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_SCENARIO_ROOTS=${MANYFORGE_SCENARIO_ROOTS}")
    fi
    # Frontend dev-build knobs must cross the docker boundary or
    # `MANYFORGE_FRONTEND_SOURCE_MODE=worktree launch.sh restart` silently
    # builds from committed HEAD inside the container.
    if [[ -n "${MANYFORGE_FRONTEND_SOURCE_MODE:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_FRONTEND_SOURCE_MODE=${MANYFORGE_FRONTEND_SOURCE_MODE}")
    fi
    if [[ -n "${MANYFORGE_FRONTEND_FORCE_REBUILD:-}" ]]; then
        assistant_env+=(-e "MANYFORGE_FRONTEND_FORCE_REBUILD=${MANYFORGE_FRONTEND_FORCE_REBUILD}")
    fi
    local vllm_status_dir="${MANYFORGE_VLLM_STATUS_DIR:-/tmp/manyforge-assistant-e2e}"
    mkdir -p "${vllm_status_dir}" 2>/dev/null || true
    info "Building and qualifying native Composer products"
    if ! docker run --rm \
        --network host \
        -v "${MANYFORGE_ROOT:?MANYFORGE_ROOT not set}:/workspace" \
        -v "${NATIVE_COMPOSER_BUILD_CACHE_VOLUME}:/tmp/manyforge-build" \
        -w /workspace \
        -e MANYFORGE_BUILD_DIR=/tmp/manyforge-build \
        -e CTEST_TEST_TIMEOUT_S="${CTEST_TEST_TIMEOUT_S:-60}" \
        "${assistant_env[@]}" \
        "${NATIVE_COMPOSER_IMAGE_ID}" \
        bash -lc '
            set -euo pipefail
            # Build output streams live to docker logs so the launcher
            # wait helper (docker logs --tail 20, then last 3 in the
            # render) can show real progress — cmake compile lines,
            # npm/vite output, roboplan progress — instead of just the
            # "Building..." echo. The previous `| tail -3` buffered
            # everything until each invocation exited.
            echo "Building C++ backends..."
            ./scripts/test-all.sh cpp 2>&1
            echo "Building roboplan..."
            ./scripts/test-all.sh roboplan 2>&1
            source /workspace/scripts/lib/frontend.sh
            manyforge_build_frontend_dist /workspace/manyforge_composer/frontend /tmp/manyforge-build
        '; then
        fail "Native Composer product build or qualification failed"
        stop_vllm_status_watcher
        return 1
    fi

    info "Starting ${container_name} (detached dev container) on Composer ${composer_port}; native viewer is default"
    if ! docker run -d --rm \
        --name "${container_name}" \
        --network host \
        -v "${MANYFORGE_ROOT:?MANYFORGE_ROOT not set}:/workspace" \
        -v "${NATIVE_COMPOSER_BUILD_CACHE_VOLUME}:/tmp/manyforge-build" \
        -v "${vllm_status_dir}:/tmp/manyforge-assistant-e2e:ro" \
        -w /workspace \
        -e MANYFORGE_BUILD_DIR=/tmp/manyforge-build \
        -e MANYFORGE_COMPOSER_PORT="${composer_port}" \
        -e MANYFORGE_TRACE_DIR="${MANYFORGE_TRACE_DIR:-/workspace/.manyforge-var/traces}" \
        "${NATIVE_COMPOSER_RT_DOCKER_ARGS[@]}" \
        "${assistant_env[@]}" \
        "${NATIVE_COMPOSER_IMAGE_ID}" \
        bash -lc '
            set -euo pipefail
            export PYTHONPATH="/tmp/manyforge-build/roboplan:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"
            source /workspace/scripts/lib/realtime-resources.sh
            manyforge_exec_with_nrt_affinity python -m manyforge_composer \
                --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
                --host 0.0.0.0 \
                --port "${MANYFORGE_COMPOSER_PORT:-9000}" \
                --static-dir /tmp/manyforge-build/frontend-cache/workspace/dist
        ' >/dev/null; then
        fail "Native Composer container failed to start"
        stop_vllm_status_watcher
        return 1
    fi
}

stop_composer_dev_container() {
    local container_name="${COMPOSER_CONTAINER:-manyforge-e2e-composer}"
    stop_vllm_status_watcher
    docker rm -f "${container_name}" >/dev/null 2>&1 || true
}
