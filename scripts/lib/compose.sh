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

# launch.sh exports MANYFORGE_ROOT; fall back to deriving it from this file's
# location so compose targeting works even if this lib is sourced standalone.
: "${MANYFORGE_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# Result variable for compose_set_lane.
declare -a COMPOSE_CMD

compose_set_lane() {
    local lane="$1"
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
    case "${lane}" in
        manyforge-only)
            COMPOSE_CMD=("${base[@]}")
            ;;
        isaac-sim)
            COMPOSE_CMD=("${base[@]}" --profile ros-full)
            ;;
        *)
            fail "compose_set_lane: unknown lane '${lane}'"
            return 1
            ;;
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
    docker rm -f manyforge-e2e-composer >/dev/null 2>&1 || true
    docker rm -f manyforge-e2e-vllm >/dev/null 2>&1 || true
}

# start_composer_dev_container — for the manyforge-only lane.
# Detached docker run that builds C++ backends on entry then execs the
# Composer process. Matches the inline pattern in scripts/demo.sh
# but in detached form so the launcher can manage it via name.
#
# Ports are env-overridable:
#   COMPOSER_PORT (default 9000) — Composer API + UI
#   HMI_PORT      (default 8081) — legacy Viser HMI, only when
#                  MANYFORGE_ENABLE_LEGACY_VISER_HMI=true
#
# Note: the container runs with --network host, so -p HOST:CONT mappings
# are no-ops. The override path is to pass the chosen ports as env vars
# into the container and have the inline Python command read them. The
# host process binds directly on the chosen ports.
#
# Optional env (forwarded to Composer): MANYFORGE_ASSISTANT_PROVIDER,
# MANYFORGE_ASSISTANT_ENDPOINT_URL, MANYFORGE_ASSISTANT_TIMEOUT_S.
start_composer_dev_container() {
    local container_name="${COMPOSER_CONTAINER:-manyforge-e2e-composer}"
    local composer_port="${COMPOSER_PORT:-9000}"
    local hmi_port="${HMI_PORT:-8081}"
    local legacy_hmi_raw="${MANYFORGE_ENABLE_LEGACY_VISER_HMI:-${ENABLE_LEGACY_VISER_HMI:-false}}"
    local legacy_hmi_enabled=false
    case "${legacy_hmi_raw,,}" in
        1|true|yes|on) legacy_hmi_enabled=true ;;
    esac
    if container_running "${container_name}"; then
        info "Reusing existing ${container_name}"
        return 0
    fi
    docker rm -f "${container_name}" >/dev/null 2>&1 || true
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
    local hmi_env=(-e "MANYFORGE_ENABLE_LEGACY_VISER_HMI=${legacy_hmi_enabled}")
    if [[ "${legacy_hmi_enabled}" == "true" ]]; then
        hmi_env+=(-e "MANYFORGE_HMI_PORT=${hmi_port}")
        info "Starting ${container_name} (detached dev container) on Composer ${composer_port}, legacy Viser HMI ${hmi_port}"
    else
        info "Starting ${container_name} (detached dev container) on Composer ${composer_port}; native viewer is default"
    fi
    docker run -d --rm \
        --name "${container_name}" \
        --network host \
        -v "${MANYFORGE_ROOT:?MANYFORGE_ROOT not set}:/workspace" \
        -v manyforge_build-cache:/tmp/manyforge-build \
        -w /workspace \
        -e MANYFORGE_COMPOSER_PORT="${composer_port}" \
        "${hmi_env[@]}" \
        -e CTEST_TEST_TIMEOUT_S="${CTEST_TEST_TIMEOUT_S:-60}" \
        "${assistant_env[@]}" \
        manyforge-dev:latest \
        bash -lc '
            set -euo pipefail
            hmi_args=()
            if [[ "${MANYFORGE_ENABLE_LEGACY_VISER_HMI:-false}" == "true" ]]; then
                hmi_args+=(--hmi-port "${MANYFORGE_HMI_PORT:-8081}")
            fi
            # Build output streams live to docker logs so the launcher
            # wait helper (docker logs --tail 20, then last 3 in the
            # render) can show real progress — cmake compile lines,
            # npm/vite output, roboplan progress — instead of just the
            # "Building..." echo. The previous `| tail -3` buffered
            # everything until each invocation exited.
            echo "Building C++ backends..."
            ./scripts/test-all.sh cpp 2>&1 || echo "  (core build had warnings — continuing)"
            echo "Building roboplan..."
            ./scripts/test-all.sh roboplan 2>&1 || echo "  (roboplan build failed — will use MockKernelClient)"
            source /workspace/scripts/lib/frontend.sh
            manyforge_build_frontend_dist /workspace/manyforge_composer/frontend /tmp/manyforge-build
            export PYTHONPATH="/tmp/manyforge-build/roboplan:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"
            exec python -m manyforge_composer \
                --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
                --host 0.0.0.0 \
                --port "${MANYFORGE_COMPOSER_PORT:-9000}" \
                "${hmi_args[@]}" \
                --static-dir "${MANYFORGE_FRONTEND_STATIC_DIR}"
        ' >/dev/null
}

stop_composer_dev_container() {
    local container_name="${COMPOSER_CONTAINER:-manyforge-e2e-composer}"
    docker rm -f "${container_name}" >/dev/null 2>&1 || true
}
