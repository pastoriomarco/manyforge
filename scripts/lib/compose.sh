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

# Result variable for compose_set_lane.
declare -a COMPOSE_CMD

compose_set_lane() {
    local lane="$1"
    case "${lane}" in
        manyforge-only)
            COMPOSE_CMD=(docker compose)
            ;;
        isaac-sim)
            COMPOSE_CMD=(docker compose --profile ros-full)
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
# Composer/HMI process. Matches the inline pattern in scripts/demo.sh
# but in detached form so the launcher can manage it via name.
#
# Ports are env-overridable:
#   COMPOSER_PORT (default 9000) — Composer API + UI
#   HMI_PORT      (default 8081) — Viser HMI
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
    info "Starting ${container_name} (detached dev container) on ports ${composer_port}/${hmi_port}"
    docker run -d --rm \
        --name "${container_name}" \
        --network host \
        -v "${MANYFORGE_ROOT:?MANYFORGE_ROOT not set}:/workspace" \
        -v manyforge_build-cache:/tmp/manyforge-build \
        -w /workspace \
        -e MANYFORGE_COMPOSER_PORT="${composer_port}" \
        -e MANYFORGE_HMI_PORT="${hmi_port}" \
        -e CTEST_TEST_TIMEOUT_S="${CTEST_TEST_TIMEOUT_S:-60}" \
        "${assistant_env[@]}" \
        manyforge-dev:latest \
        bash -lc '
            set -euo pipefail
            # Remove the image-baked frontend so Composer falls back to
            # the bind-mounted manyforge_composer/frontend/dist/ (which
            # we rebuild below). Without the rebuild step, source edits
            # to Layout.tsx etc. silently never reach the running UI.
            rm -rf /opt/manyforge-frontend/dist 2>/dev/null || true
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
            echo "Building frontend (manyforge_composer/frontend)..."
            (
                cd /workspace/manyforge_composer/frontend
                # `npm ci` (not `npm install`) so a stale node_modules
                # against an edited package-lock.json fails fast with a
                # clear error instead of silently producing a half-updated
                # tree that vite then builds against. ci is the canonical
                # containerized-build pattern: idempotent, reproducible,
                # bails on lock mismatch. Skipped when node_modules is
                # already in sync (ci itself detects this and is a no-op
                # if present + matching).
                if [[ ! -d node_modules ]] || [[ package-lock.json -nt node_modules ]]; then
                    echo "  (node_modules missing or stale; running npm ci)"
                    npm ci --no-audit --no-fund 2>&1
                fi
                # `vite build` is ~350 ms when sources unchanged — cheap
                # enough to run unconditionally and ensures the dist
                # bundle in the bind mount tracks the live source.
                npm run build 2>&1
            ) || echo "  (frontend build had warnings — continuing with whatever dist exists)"
            export PYTHONPATH="/tmp/manyforge-build/roboplan:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"
            exec python -m manyforge_composer \
                --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
                --host 0.0.0.0 \
                --port "${MANYFORGE_COMPOSER_PORT:-9000}" \
                --hmi-port "${MANYFORGE_HMI_PORT:-8081}"
        ' >/dev/null
}

stop_composer_dev_container() {
    local container_name="${COMPOSER_CONTAINER:-manyforge-e2e-composer}"
    docker rm -f "${container_name}" >/dev/null 2>&1 || true
}
