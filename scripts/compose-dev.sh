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
bootstrap_project=
ros2_control=false
started_container=false
operator_instance=${MANYFORGE_DEV_LAUNCH_INSTANCE_ID:-}
operator_digest=${MANYFORGE_DEV_LAUNCH_INPUT_DIGEST:-}
operator_mode=false
container_ref=${MANYFORGE_DEV_EXPECTED_CONTAINER_ID:-$container_name}
if [[ -n "$operator_instance" ]]; then
  operator_mode=true
  [[ "$operator_instance" != . && "$operator_instance" != .. && "$operator_instance" != */* && "$operator_instance" != *\\* ]] || { echo "Invalid operator instance path" >&2; exit 64; }
  [[ "$operator_digest" =~ ^[0-9a-f]{64}$ ]] || { echo "Invalid operator input digest" >&2; exit 64; }
  [[ "$image" =~ ^sha256:[0-9a-f]{64}$ ]] || { echo "Operator requires an explicit full image ID" >&2; exit 64; }
fi

assert_owned_container() {
  [[ "$operator_mode" == true ]] || return 0
  docker inspect --format '{{json .}}' "$container_ref" | python3 -c '
import json,sys
row=json.load(sys.stdin); labels=row["Config"].get("Labels") or {}
expected_id, image, instance, digest=sys.argv[1:]
if (expected_id and row["Id"] != expected_id) or row["Image"] != image or labels.get("manyforge.launch-instance") != instance or labels.get("manyforge.launch-input-digest") != digest:
    raise SystemExit("Refusing foreign or changed operator container")
' "${MANYFORGE_DEV_EXPECTED_CONTAINER_ID:-}" "$image" "$operator_instance" "$operator_digest"
}

operator_status() {
  local require_ready=${1:-false}
  assert_owned_container || return $?
  local inspect current code
  inspect=$(docker inspect --format '{{json .}}' "$container_ref") || return $?
  current=null
  code=69
  if [[ $(docker inspect -f '{{.State.Running}}' "$container_ref") == true ]]; then
    if current=$(docker exec "$container_ref" python3 scripts/dev-workbench.py status --operator-runtime --composer-port 9000); then
      code=0
    else
      code=$?
      [[ -n "$current" ]] || current=null
    fi
  fi
  python3 -c '
import json,sys
row=json.loads(sys.argv[1]);workbench=json.loads(sys.argv[2])
print(json.dumps({"container_id":row["Id"],"image_id":row["Image"],"labels":row["Config"].get("Labels") or {},"running":row["State"]["Running"],"workbench":workbench,"log_reference":"docker logs "+row["Id"]},sort_keys=True,separators=(",",":")))
' "$inspect" "$current" || return $?
  if [[ "$require_ready" == true ]]; then
    return "$code"
  fi
  return 0
}

frontend_ready() {
  docker exec "$container_ref" python3 -c \
    'import socket; socket.create_connection(("127.0.0.1", 3000), timeout=0.5).close()' \
    >/dev/null 2>&1
}

cleanup_failed_start() {
  result=$?
  if [[ $result -ne 0 && "$started_container" == true && "$operator_mode" == true ]]; then
    # G owns the Sim. Keep this exact ROS client available for its cleanup;
    # G then calls the same evidence-preserving stop path below.
    operator_status || true
  elif [[ $result -ne 0 && "$started_container" == true ]]; then
    docker stop --time 15 "$container_ref" >/dev/null 2>&1 || true
    docker rm "$container_ref" >/dev/null 2>&1 || true
  fi
  exit "$result"
}
trap cleanup_failed_start EXIT

usage() {
  echo "Usage: $0 start [--project <scenario-project-folder> | --bootstrap-project <first-ros-scenario-folder>] [--ros2-control] | status | stop [--keep-evidence]" >&2
}

stop_with_evidence() {
  assert_owned_container || return $?
  if ! docker stop --time 15 "$container_ref" >/dev/null; then
    echo "Failed to stop the development workbench; no evidence was copied." >&2
    return 69
  fi

  local running
  if ! running=$(docker inspect -f '{{.State.Running}}' "$container_ref"); then
    echo "Could not verify that the development workbench stopped; no evidence was copied." >&2
    return 69
  fi
  if [[ "$running" != false ]]; then
    echo "Development workbench is still running; no evidence was copied." >&2
    return 69
  fi

  local timestamp evidence_root evidence_path partial_path
  timestamp=$(date -u +%Y%m%dT%H%M%SZ)
  evidence_root=${MANYFORGE_DEV_EVIDENCE_ROOT:-"$cache_base/evidence"}
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
  if ! docker cp "$container_ref:/tmp/manyforge-state" - |
      tar --extract --no-same-owner --directory "$partial_path" \
        --exclude='manyforge-state/installed-targets' \
        --exclude='manyforge-state/artifacts/raw-artifacts' \
        --exclude='manyforge-state/artifacts/prepared-activations' \
        --exclude='manyforge-state/execution-run/manyforge-retained-planning-*'; then
    echo "Could not copy runtime evidence. Partial evidence: $partial_path" >&2
    echo "The stopped container was retained: $container_name" >&2
    return 69
  fi
  if ! docker logs "$container_ref" >"$partial_path/docker.log" 2>&1; then
    echo "Could not copy container logs. Partial evidence: $partial_path" >&2
    echo "The stopped container was retained: $container_name" >&2
    return 69
  fi
  if [[ "$operator_mode" == true ]]; then
    printf 'docker logs %s\n' "$container_ref" >"$partial_path/docker-logs-reference.txt" || return 69
  fi
  local inspect_format
  inspect_format='{"image":{{json .Image}},"mounts":{{json .Mounts}},'
  inspect_format+='"state":{"status":{{json .State.Status}},'
  inspect_format+='"running":{{json .State.Running}},'
  inspect_format+='"exit_code":{{json .State.ExitCode}},'
  inspect_format+='"finished_at":{{json .State.FinishedAt}}}}'
  if ! docker inspect --format "$inspect_format" "$container_ref" \
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
  if ! docker rm "$container_ref" >/dev/null; then
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
        --ros2-control)
          ros2_control=true
          shift
          ;;
        --bootstrap-project)
          [[ $# -ge 2 ]] || { usage; exit 64; }
          bootstrap_project=$2
          shift 2
          ;;
        *) usage; exit 64 ;;
      esac
    done
    if [[ -n "$bootstrap_project" && ( "$ros2_control" != true || -n "$project" ) ]]; then
      echo "--bootstrap-project requires --ros2-control and excludes --project." >&2
      exit 64
    fi
    if [[ "$operator_mode" != true && "$ros2_control" == true && -z "$project" && -z "$bootstrap_project" ]]; then
      echo "--ros2-control requires --bootstrap-project or --project selecting the intended first Scenario." >&2
      exit 64
    fi
    if docker inspect "$container_ref" >/dev/null 2>&1; then
      assert_owned_container || return $?
      if [[ $(docker inspect -f '{{.State.Running}}' "$container_ref") == true ]]; then
        existing_ros=$(docker inspect -f '{{index .Config.Labels "manyforge.dev.ros2-control"}}' "$container_ref")
        if [[ "$existing_ros" == true && "$ros2_control" != true ]] ||
            [[ "$existing_ros" != true && "$ros2_control" == true ]]; then
          echo "A workbench with different runtime capabilities is already running; stop it before changing --ros2-control." >&2
          exit 69
        fi
        if [[ -n "$bootstrap_project" ]]; then
          echo "A workbench is already running; use status, or stop it before selecting an empty ROS bootstrap." >&2
          exit 69
        fi
        if [[ "$operator_mode" == true ]]; then
          operator_status true
          exit $?
        fi
        status_args=(status)
        if [[ -n "$project" ]]; then
          status_args+=(--project "$project")
        fi
        if frontend_ready && current=$(docker exec "$container_ref" python3 scripts/dev-workbench.py "${status_args[@]}" 2>/dev/null); then
          echo "$current"
          echo "Development workbench is already running at http://localhost:3000"
          exit 0
        fi
        echo "A different or unhealthy workbench is already running." >&2
        echo "Use '$0 stop' before selecting another scenario." >&2
        exit 69
      fi
      if [[ "$operator_mode" == true ]]; then
        stop_with_evidence
      else
        docker rm "$container_ref" >/dev/null
      fi
    fi
    image_id=$(docker image inspect -f '{{.Id}}' "$image" 2>/dev/null) || {
      echo "Development image is unavailable: $image" >&2
      echo "Prepare it once with: ./scripts/setup-docker.sh && docker compose build dev" >&2
      exit 69
    }
    image_key=${image_id#sha256:}
    cache_root="$cache_base/images/$image_key"
    mkdir -p "$cache_root/build" "$cache_root/node_modules"
    # Installation Composer settings and the Program runner's persistent
    # blackboard values survive stop/start (values are checked against the
    # applied Program; see DEVELOPMENT_WORKBENCH.md). Only these folders are
    # persistent; all other runtime state still resets with the container.
    state_base="$cache_base/state"
    if [[ "$operator_mode" == true ]]; then
      state_base="$cache_base/state/$operator_instance"
    fi
    composer_state="$state_base/composer"
    runner_state="$state_base/program-runner"
    mkdir -p "$composer_state" "$runner_state"
    serve_args=(serve)
    operator_mounts=()
    operator_labels=()
    if [[ "$operator_mode" == true ]]; then
      [[ "$ros2_control" == true && -z "$project" && -z "$bootstrap_project" ]] || { echo "G requires ROS and no auto-Load" >&2; exit 64; }
      runtime_root=${MANYFORGE_DEV_RUNTIME_STATE_ROOT:?explicit runtime state root required}
      install_root=${MANYFORGE_DEV_OPERATOR_INSTALL_ROOT:?qualified install root required}
      [[ "$runtime_root" == /* && "$runtime_root" != /tmp && "$runtime_root" != /tmp/* && "$runtime_root" != /run && "$runtime_root" != /run/* ]] || { echo "Operator runtime state requires persistent absolute host storage" >&2; exit 64; }
      mkdir -p "$runtime_root"
      [[ $(stat -f -c %T "$runtime_root") != tmpfs ]] || { echo "Operator runtime state cannot be tmpfs" >&2; exit 64; }
      [[ "$install_root" == /* && -d "$install_root" ]] || { echo "Qualified install root unavailable" >&2; exit 64; }
      serve_args+=(--operator-runtime --operator-install-root "$install_root"
        --operator-cell-id "${MANYFORGE_DEV_OPERATOR_CELL_ID:?original cell ID required}"
        --operator-instance-id "$operator_instance"
        --operator-readiness-timeout "${MANYFORGE_DEV_OPERATOR_READINESS_TIMEOUT:-60}"
        --canonical-empty-admission-filename "${MANYFORGE_DEV_OPERATOR_ADMISSION:?qualified admission required}"
        --package-aggregate-sha256 "${MANYFORGE_DEV_OPERATOR_PACKAGE_SHA256:?qualified package digest required}"
        --composer-port 9000 --composer-bind-host 0.0.0.0)
      operator_mounts=(-v "$runtime_root:/tmp/manyforge-state" -v "$install_root:$install_root:ro")
      for option in COMPOSER_STATIC_DIR SIMULATION_CONTROL_CONFIG ISAAC_SITE_SETTINGS OPERATOR_NODE_CATALOG OPERATOR_ALARM_CATALOG; do
        variable="MANYFORGE_DEV_$option"
        if [[ -n ${!variable:-} ]]; then
          path=${!variable}
          [[ "$path" == /* ]] || { echo "Operator input must be absolute: $option" >&2; exit 64; }
          case "$option" in
            COMPOSER_STATIC_DIR) [[ -f "$path/index.html" ]] || exit 64; option_name=--composer-static-dir; parent=$path ;;
            SIMULATION_CONTROL_CONFIG) [[ -f "$path" ]] || exit 64; option_name=--simulation-control-config; parent=$(dirname "$path") ;;
            ISAAC_SITE_SETTINGS) [[ -f "$path" ]] || exit 64; option_name=--isaac-site-settings; parent=$(dirname "$path") ;;
            OPERATOR_NODE_CATALOG) [[ -f "$path" ]] || exit 64; option_name=--node-catalog-path; parent=$(dirname "$path") ;;
            OPERATOR_ALARM_CATALOG) [[ -f "$path" ]] || exit 64; option_name=--alarm-catalog-path; parent=$(dirname "$path") ;;
          esac
          operator_mounts+=(-v "$parent:$parent:ro")
          serve_args+=("$option_name" "$path")
        fi
      done
      operator_labels=(--log-driver json-file --label "manyforge.launch-instance=$operator_instance"
        --label "manyforge.launch-input-digest=$operator_digest")
    fi
    ros_environment=()
    ros_identity=()
    if [[ "$ros2_control" == true ]]; then
      serve_args+=(--ros2-control)
      # The full ROS image has its own development UID. Keep all bind-mounted
      # writes with the launching user's ownership, including npm/ROS caches.
      mkdir -p "$cache_root/home"
      for directory in "$repo_root" "$cache_root" "$cache_root/build" \
          "$cache_root/home" "$cache_root/node_modules" "$composer_state" "$runner_state"; do
        if [[ ! -w "$directory" ]]; then
          echo "ROS workbench requires a writable host-owned directory: $directory" >&2
          exit 69
        fi
      done
      ros_identity=(--user "$(id -u):$(id -g)" --env HOME=/runner/home \
        --env MANYFORGE_BUILD_DIR=/runner/build)
      # Preserve explicitly configured DDS inputs. The image entrypoint owns
      # ROS/venv setup; this launcher neither sources a topology nor picks peers.
      for variable in ROS_DOMAIN_ID RMW_IMPLEMENTATION ROS_AUTOMATIC_DISCOVERY_RANGE \
          ROS_STATIC_PEERS FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE \
          ROS_DISCOVERY_SERVER ROS_SUPER_CLIENT CYCLONEDDS_URI; do
        if [[ -v "$variable" ]]; then
          ros_environment+=(--env "$variable")
        fi
      done
    fi
    if [[ -n "$project" ]]; then
      serve_args+=(--project "$project")
    fi
    if [[ -n "$bootstrap_project" ]]; then
      serve_args+=(--bootstrap-project "$bootstrap_project")
    fi
    container_ref=$(docker run -d \
      --name "$container_name" \
      --label "manyforge.dev.ros2-control=$ros2_control" \
      "${operator_labels[@]}" \
      --network host \
      --init \
      --log-opt max-size=50m \
      --log-opt max-file=3 \
      -e MANYFORGE_DEV_BUILD_JOBS="${MANYFORGE_DEV_BUILD_JOBS:-}" \
      -e MANYFORGE_STATE_DIR=/var/lib/manyforge \
      "${ros_identity[@]}" \
      "${ros_environment[@]}" \
      "${operator_mounts[@]}" \
      -v "$repo_root:/workspace" \
      -v "$cache_root:/runner" \
      -v "$composer_state:/var/lib/manyforge/composer" \
      -v "$runner_state:/var/lib/manyforge/program-runner" \
      -v "$cache_root/node_modules:/workspace/manyforge_composer/frontend/node_modules" \
      -w /workspace \
      "$image_id" \
      python3 scripts/dev-workbench.py "${serve_args[@]}")
    started_container=true
    deadline=$((SECONDS + ${MANYFORGE_DEV_START_TIMEOUT_SECONDS:-900}))
    while ((SECONDS < deadline)); do
      if [[ $(docker inspect -f '{{.State.Running}}' "$container_ref") != true ]]; then
        docker logs --tail 200 "$container_ref" >&2
        exit 69
      fi
      if [[ "$operator_mode" == true ]]; then
        if current=$(operator_status true); then
          echo "$current"
          started_container=false
          exit 0
        fi
        sleep 1
        continue
      fi
      status_args=(status)
      if [[ -n "$project" ]]; then
        status_args+=(--project "$project")
      fi
      if frontend_ready && current=$(docker exec "$container_ref" python3 scripts/dev-workbench.py "${status_args[@]}" 2>/dev/null); then
        echo "$current"
        echo "Development workbench: http://localhost:3000"
        started_container=false
        exit 0
      fi
      sleep 1
    done
    docker logs --tail 200 "$container_ref" >&2
    echo "Development workbench did not become ready before its bounded startup timeout." >&2
    exit 69
    ;;
  status)
    if [[ "$operator_mode" == true ]]; then
      (($# == 0)) || { usage; exit 64; }
      if docker inspect "$container_ref" >/dev/null 2>&1; then
        operator_status
      else
        # A daemon/transport error is not proof of absence.
        candidates=$(docker container ls --all --no-trunc --format '{{.ID}} {{.Names}}')
        present=$(python3 -c '
import sys
ref=sys.argv[1]
print(any(ref in line.split() for line in sys.stdin), end="")
' "$container_ref" <<<"$candidates")
        [[ "$present" == False ]] || { echo "Container inspection failed despite listed occurrence" >&2; exit 75; }
        echo '{"container_id":null,"running":false,"workbench":null}'
      fi
      exit $?
    fi
    (($# == 0)) || { usage; exit 64; }
    docker inspect "$container_ref" >/dev/null 2>&1 || {
      echo "Development workbench is stopped." >&2
      exit 69
    }
    [[ $(docker inspect -f '{{.State.Running}}' "$container_ref") == true ]] || {
      echo "Development workbench container is not running." >&2
      exit 69
    }
    docker exec "$container_ref" python3 scripts/dev-workbench.py status
    ;;
  stop)
    keep_evidence=false
    if [[ ${1:-} == --keep-evidence ]]; then
      keep_evidence=true
      shift
    fi
    (($# == 0)) || { usage; exit 64; }
    if [[ "$operator_mode" == true ]]; then
      keep_evidence=true
    fi
    if [[ "$keep_evidence" == true ]]; then
      docker inspect "$container_ref" >/dev/null 2>&1 || {
        echo "Cannot preserve evidence: development workbench container does not exist." >&2
        exit 69
      }
      stop_with_evidence
      echo "Development workbench is stopped. Build and npm caches were retained."
      exit 0
    fi
    if docker inspect "$container_ref" >/dev/null 2>&1; then
      docker stop --time 15 "$container_ref" >/dev/null || true
      docker rm "$container_ref" >/dev/null || true
    fi
    echo "Development workbench is stopped. Build and npm caches were retained."
    ;;
  *) usage; exit 64 ;;
esac
