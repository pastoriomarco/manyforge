#!/usr/bin/env bash
# One supported D0 development workbench: explicit start/status/stop, exact
# backend-owned scenario activation, and Vite HMR. It never runs qualification,
# tests, sanitizers, release packaging, or Stop-proof generation.

set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
container_name=${MANYFORGE_DEV_CONTAINER_NAME:-manyforge-development-workbench}
image=${MANYFORGE_DEV_IMAGE:-manyforge-dev:latest}
cache_base=${MANYFORGE_DEV_CACHE_BASE:-"$repo_root/.manyforge-dev"}
project=
started_container=false

frontend_ready() {
  docker exec "$container_name" python3 -c \
    'import socket; socket.create_connection(("127.0.0.1", 3000), timeout=0.5).close()' \
    >/dev/null 2>&1
}

cleanup_failed_start() {
  result=$?
  if [[ $result -ne 0 && "$started_container" == true ]]; then
    docker stop --time 15 "$container_name" >/dev/null 2>&1 || true
    docker rm "$container_name" >/dev/null 2>&1 || true
  fi
  exit "$result"
}
trap cleanup_failed_start EXIT

usage() {
  echo "Usage: $0 start [--project <scenario-project-folder>] | status | stop" >&2
}

command=${1:-}
if [[ -n "$command" ]]; then
  shift
fi
case "$command" in
  start)
    while (($#)); do
      case "$1" in
        --project)
          [[ $# -ge 2 ]] || { usage; exit 64; }
          project=$2
          shift 2
          ;;
        *) usage; exit 64 ;;
      esac
    done
    if docker inspect "$container_name" >/dev/null 2>&1; then
      if [[ $(docker inspect -f '{{.State.Running}}' "$container_name") == true ]]; then
        status_args=(status)
        if [[ -n "$project" ]]; then
          status_args+=(--project "$project")
        fi
        if frontend_ready && current=$(docker exec "$container_name" python3 scripts/dev-workbench.py "${status_args[@]}" 2>/dev/null); then
          echo "$current"
          echo "Development workbench is already running at http://localhost:3000"
          exit 0
        fi
        echo "A different or unhealthy workbench is already running." >&2
        echo "Use '$0 stop' before selecting another scenario." >&2
        exit 69
      fi
      docker rm "$container_name" >/dev/null
    fi
    image_id=$(docker image inspect -f '{{.Id}}' "$image" 2>/dev/null) || {
      echo "Development image is unavailable: $image" >&2
      echo "Prepare it once with: ./scripts/setup-docker.sh && docker compose build dev" >&2
      exit 69
    }
    image_key=${image_id#sha256:}
    cache_root="$cache_base/images/$image_key"
    mkdir -p "$cache_root/build" "$cache_root/node_modules"
    # Installation Composer settings survive stop/start. Only this folder is
    # persistent; all other runtime state still resets with the container.
    composer_state="$cache_base/state/composer"
    mkdir -p "$composer_state"
    serve_args=(serve)
    if [[ -n "$project" ]]; then
      serve_args+=(--project "$project")
    fi
    docker run -d \
      --name "$container_name" \
      --network host \
      --init \
      --log-opt max-size=50m \
      --log-opt max-file=3 \
      -e MANYFORGE_DEV_BUILD_JOBS="${MANYFORGE_DEV_BUILD_JOBS:-}" \
      -e MANYFORGE_STATE_DIR=/var/lib/manyforge \
      -v "$repo_root:/workspace" \
      -v "$cache_root:/runner" \
      -v "$composer_state:/var/lib/manyforge/composer" \
      -v "$cache_root/node_modules:/workspace/manyforge_composer/frontend/node_modules" \
      -w /workspace \
      "$image_id" \
      python3 scripts/dev-workbench.py "${serve_args[@]}" >/dev/null
    started_container=true
    deadline=$((SECONDS + ${MANYFORGE_DEV_START_TIMEOUT_SECONDS:-900}))
    while ((SECONDS < deadline)); do
      if [[ $(docker inspect -f '{{.State.Running}}' "$container_name") != true ]]; then
        docker logs --tail 200 "$container_name" >&2
        exit 69
      fi
      status_args=(status)
      if [[ -n "$project" ]]; then
        status_args+=(--project "$project")
      fi
      if frontend_ready && current=$(docker exec "$container_name" python3 scripts/dev-workbench.py "${status_args[@]}" 2>/dev/null); then
        echo "$current"
        echo "Development workbench: http://localhost:3000"
        started_container=false
        exit 0
      fi
      sleep 1
    done
    docker logs --tail 200 "$container_name" >&2
    echo "Development workbench did not become ready before its bounded startup timeout." >&2
    exit 69
    ;;
  status)
    (($# == 0)) || { usage; exit 64; }
    docker inspect "$container_name" >/dev/null 2>&1 || {
      echo "Development workbench is stopped." >&2
      exit 69
    }
    [[ $(docker inspect -f '{{.State.Running}}' "$container_name") == true ]] || {
      echo "Development workbench container is not running." >&2
      exit 69
    }
    docker exec "$container_name" python3 scripts/dev-workbench.py status
    ;;
  stop)
    (($# == 0)) || { usage; exit 64; }
    if docker inspect "$container_name" >/dev/null 2>&1; then
      docker stop --time 15 "$container_name" >/dev/null || true
      docker rm "$container_name" >/dev/null || true
    fi
    echo "Development workbench is stopped. Build and npm caches were retained."
    ;;
  *) usage; exit 64 ;;
esac
