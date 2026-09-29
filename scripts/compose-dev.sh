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
  echo "Usage: $0 start [--project <scenario-project-folder>] | status | stop [--keep-evidence]" >&2
}

stop_with_evidence() {
  if ! docker stop --time 15 "$container_name" >/dev/null; then
    echo "Failed to stop the development workbench; no evidence was copied." >&2
    return 69
  fi

  local running
  if ! running=$(docker inspect -f '{{.State.Running}}' "$container_name"); then
    echo "Could not verify that the development workbench stopped; no evidence was copied." >&2
    return 69
  fi
  if [[ "$running" != false ]]; then
    echo "Development workbench is still running; no evidence was copied." >&2
    return 69
  fi

  local timestamp evidence_root evidence_path partial_path
  timestamp=$(date -u +%Y%m%dT%H%M%SZ)
  evidence_root="$cache_base/evidence"
  evidence_path="$evidence_root/$timestamp"
  partial_path="$evidence_path.partial"
  if ! mkdir -p "$evidence_root" ||
      [[ -e "$evidence_path" || -e "$partial_path" ]] ||
      ! mkdir "$partial_path"; then
    echo "Could not create partial evidence folder: $partial_path" >&2
    echo "The stopped container was retained: $container_name" >&2
    return 69
  fi

  # Skip what is reproducible from the image and the Scenario at this commit:
  # installed binaries, content-addressed model/mesh copies, the prepared
  # activation inputs that embed them and the native host's unpacked planning
  # robot packages. The Manager journal keeps their exact digests, so the
  # failed Apply stays identifiable.
  if ! docker cp "$container_name:/tmp/manyforge-state" - |
      tar --extract --no-same-owner --directory "$partial_path" \
        --exclude='manyforge-state/installed-targets' \
        --exclude='manyforge-state/artifacts/raw-artifacts' \
        --exclude='manyforge-state/artifacts/prepared-activations' \
        --exclude='manyforge-state/execution-run/manyforge-retained-planning-*'; then
    echo "Could not copy runtime evidence. Partial evidence: $partial_path" >&2
    echo "The stopped container was retained: $container_name" >&2
    return 69
  fi
  if ! docker logs "$container_name" >"$partial_path/docker.log" 2>&1; then
    echo "Could not copy container logs. Partial evidence: $partial_path" >&2
    echo "The stopped container was retained: $container_name" >&2
    return 69
  fi
  local inspect_format
  inspect_format='{"image":{{json .Image}},"mounts":{{json .Mounts}},'
  inspect_format+='"state":{"status":{{json .State.Status}},'
  inspect_format+='"running":{{json .State.Running}},'
  inspect_format+='"exit_code":{{json .State.ExitCode}},'
  inspect_format+='"finished_at":{{json .State.FinishedAt}}}}'
  if ! docker inspect --format "$inspect_format" "$container_name" \
      >"$partial_path/docker-inspect.json"; then
    echo "Could not inspect the stopped container. Partial evidence: $partial_path" >&2
    echo "The stopped container was retained: $container_name" >&2
    return 69
  fi
  if ! mv --no-clobber --no-target-directory "$partial_path" "$evidence_path" ||
      [[ -e "$partial_path" ]]; then
    echo "Could not finalize evidence folder. Partial evidence: $partial_path" >&2
    echo "The stopped container was retained: $container_name" >&2
    return 69
  fi
  if ! docker rm "$container_name" >/dev/null; then
    echo "Evidence was preserved at $evidence_path, but the stopped container could not be removed." >&2
    return 69
  fi

  echo "Evidence preserved at $evidence_path"
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
    keep_evidence=false
    if [[ ${1:-} == --keep-evidence ]]; then
      keep_evidence=true
      shift
    fi
    (($# == 0)) || { usage; exit 64; }
    if [[ "$keep_evidence" == true ]]; then
      docker inspect "$container_name" >/dev/null 2>&1 || {
        echo "Cannot preserve evidence: development workbench container does not exist." >&2
        exit 69
      }
      stop_with_evidence
      echo "Development workbench is stopped. Build and npm caches were retained."
      exit 0
    fi
    if docker inspect "$container_name" >/dev/null 2>&1; then
      docker stop --time 15 "$container_name" >/dev/null || true
      docker rm "$container_name" >/dev/null || true
    fi
    echo "Development workbench is stopped. Build and npm caches were retained."
    ;;
  *) usage; exit 64 ;;
esac
