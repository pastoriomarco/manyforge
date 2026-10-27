#!/usr/bin/env bash
# One supported D0 development workbench: explicit start/status/stop, exact
# backend-owned scenario activation, and Vite HMR. It never runs qualification,
# tests, sanitizers, release packaging, or Stop-proof generation.

set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
container_name=${MANYFORGE_DEV_CONTAINER_NAME:-manyforge-development-workbench}
[[ "$container_name" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || { echo "Invalid Docker container name" >&2; exit 64; }
image=${MANYFORGE_DEV_IMAGE:-manyforge-dev:latest}
cache_base=${MANYFORGE_DEV_CACHE_BASE:-"$repo_root/.manyforge-dev"}
runtime_root=${MANYFORGE_DEV_RUNTIME_STATE_ROOT:-"$cache_base/runtime/$container_name"}
retention_helper="$repo_root/scripts/lib/export_retention.py"
session_helper="$repo_root/scripts/lib/workbench_session.py"
project=
bootstrap_project=
ros2_control=true
started_container=false
stopped_container_receipt=
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

assert_runtime_root_available() {
  local running
  running=$(docker container ls --filter status=running --filter status=paused --filter status=restarting \
    --quiet --no-trunc) || return $?
  [[ -n "$running" ]] || return 0
  local -a running_ids
  mapfile -t running_ids <<<"$running"
  docker inspect "${running_ids[@]}" | python3 -c '
import json,os,sys
root=os.path.realpath(sys.argv[1])
try:
    rows=json.load(sys.stdin)
    if not isinstance(rows,list):
        raise ValueError("container inspection is not a list")
except (ValueError,TypeError) as error:
    raise SystemExit("Could not verify exclusive ManyForge state root: "+str(error))
for row in rows:
    if not any(row["State"].get(key, False) for key in ("Running", "Paused", "Restarting")):
        continue
    for mount in row["Mounts"]:
        if mount.get("Type") == "bind":
            source=os.path.realpath(mount["Source"])
            if os.path.commonpath((source,root)) in (source,root):
                raise SystemExit("Refusing overlapping live ManyForge state root: "+root+" already bound by "+row["Id"])
' "$runtime_root"
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
    stop_with_evidence || true
  fi
  exit "$result"
}
trap cleanup_failed_start EXIT

usage() {
  echo "Usage: $0 start [--project <scenario-project-folder> | --bootstrap-project <first-ros-scenario-folder>] | status | stop [--keep-evidence]" >&2
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

  if ! capture_stopped_container; then
    enforce_retained_limit || true
    return 69
  fi
}

container_state_mount() {
  docker inspect --format '{{json .Mounts}}' "$container_ref" | python3 -c '
import json,sys
mounts=json.load(sys.stdin)
roots=[row["Source"] for row in mounts if row.get("Type")=="bind" and row.get("Destination")=="/tmp/manyforge-state"]
if len(roots)>1: raise SystemExit("Ambiguous runtime state mount")
print(roots[0] if roots else "")
'
}

rotate_quiescent_root() {
  local runtime_root=$1
  local -a aggregate=()
  if [[ -z ${MANYFORGE_DEV_RUNTIME_STATE_ROOT:-} ]] && python3 -c '
import pathlib,sys
raise SystemExit(0 if pathlib.Path(sys.argv[1]).resolve().parent == pathlib.Path(sys.argv[2]).resolve() else 1)
' "$runtime_root" "$cache_base/runtime"; then
    aggregate=(--default-roots "$cache_base/runtime")
  fi
  if assert_runtime_root_available; then
    python3 "$retention_helper" rotate "$runtime_root" "${aggregate[@]}" || echo "WARNING ManyForge retention helper failed; runtime remains available" >&2
  else
    echo "WARNING ManyForge retention skipped: state root has another live user or inspection failed" >&2
  fi
}

capture_stopped_container() {
  local mounted_root timestamp evidence_root evidence_path partial_path inspect_format kind
  local inspect_summary tail_path
  local -a prepare_options=()
  [[ $(docker inspect -f '{{.State.Running}}' "$container_ref") == false ]] || {
    echo "Container $container_ref is not confirmed stopped; capture/removal refused" >&2; return 69;
  }
  mounted_root=$(container_state_mount) || return 69
  timestamp=$(date -u +%Y%m%dT%H%M%SZ.%N)
  evidence_root=${MANYFORGE_DEV_EVIDENCE_ROOT:-"$cache_base/evidence"}
  kind=legacy_state
  if [[ -n "$mounted_root" ]]; then
    mounted_root=$(python3 -c 'import pathlib,sys;print(pathlib.Path(sys.argv[1]).resolve())' "$mounted_root") || return 69
    evidence_root="$mounted_root/evidence"
    prepare_options=(--state-root "$mounted_root")
    kind=console_tail
  fi
  if [[ -z "$mounted_root" ]]; then
    evidence_root=$(python3 -c 'import pathlib,sys;print(pathlib.Path(sys.argv[1]).resolve())' "$evidence_root") || return 69
    prepare_options=(--legacy)
  fi
  evidence_path="$evidence_root/$timestamp"
  if [[ -e "$evidence_path" ]]; then
    echo "Container is stopped, capture failed, container retained: $container_ref; destination exists: $evidence_path" >&2
    return 69
  fi
  # This summary is also the approved failed-capture receipt; write it FIRST.
  inspect_format='{"container_id":{{json .Id}},"manyforge_export_kind":"'"$kind"'","image":{{json .Image}},"mounts":{{json .Mounts}},'
  inspect_format+='"state":{"status":{{json .State.Status}},"running":{{json .State.Running}},'
  inspect_format+='"exit_code":{{json .State.ExitCode}},"finished_at":{{json .State.FinishedAt}}}}'
  if ! inspect_summary=$(docker inspect --format "$inspect_format" "$container_ref" | python3 -c '
import json,sys
data=sys.stdin.buffer.read(65537)
if len(data)>65536: raise SystemExit("Stopped-container summary exceeds 64KB")
row=json.loads(data)
if row["state"]["running"] is not False or row["state"]["status"] not in ("exited","dead","created"):
    raise SystemExit("Container is no longer stopped")
print(json.dumps(row,separators=(",",":")))
'); then
    echo "Could not inspect the stopped-container capture." >&2
    echo "Container is stopped, capture failed, container retained: $container_ref; manually export or explicitly remove after investigation" >&2
    return 69
  fi
  if ! partial_path=$(python3 "$retention_helper" prepare "$evidence_root" --timestamp "$timestamp" "${prepare_options[@]}" <<<"$inspect_summary"); then
    echo "Could not prepare the stopped-container capture. Partial evidence: $evidence_root" >&2
    echo "Container is stopped, capture failed, container retained: $container_ref; manually export or explicitly remove after investigation" >&2
    return 69
  fi
  if [[ -z "$mounted_root" ]]; then
    # Transitional unmounted containers may hold the only durable evidence.
    if ! docker cp "$container_ref:/tmp/manyforge-state" - |
        tar --extract --no-same-owner --directory "$partial_path" \
          --exclude='manyforge-state/installed-targets' \
          --exclude='manyforge-state/artifacts/raw-artifacts' \
          --exclude='manyforge-state/artifacts/prepared-activations' \
          --exclude='manyforge-state/execution-run/manyforge-retained-planning-*'; then
      echo "Could not copy legacy runtime evidence. Partial evidence: $partial_path" >&2
      echo "Container is stopped, capture failed, container retained: $container_ref; manually export or explicitly remove after investigation" >&2
      return 69
    fi
  fi
  tail_path="$partial_path/docker.log"
  [[ -z "$mounted_root" ]] || tail_path="$partial_path/.docker.log.next"
  if ! docker logs "$container_ref" 2>&1 | python3 "$retention_helper" tail "$tail_path"; then
    echo "Could not copy container logs. Partial evidence: $partial_path" >&2
    echo "Container is stopped, capture failed, container retained: $container_ref; manually export or explicitly remove after investigation" >&2
    return 69
  fi
  if [[ -n "$mounted_root" ]] && ! mv --no-target-directory "$tail_path" "$partial_path/docker.log"; then
    echo "Could not replace bounded console tail. Partial evidence: $partial_path" >&2
    echo "Container is stopped, capture failed, container retained: $container_ref; prior tail preserved" >&2
    return 69
  fi
  if ! mv --no-clobber --no-target-directory "$partial_path" "$evidence_path" || [[ -e "$partial_path" ]]; then
    echo "Could not finalize evidence folder. Partial evidence: $partial_path" >&2
    echo "Container is stopped, capture failed, container retained: $container_ref" >&2
    return 69
  fi
  [[ $(docker inspect -f '{{.State.Running}}' "$container_ref") == false ]] || return 69
  if ! docker rm "$container_ref" >/dev/null; then
    echo "Evidence was preserved at $evidence_path, but the stopped container could not be removed." >&2
    return 69
  fi
  echo "Evidence preserved at $evidence_path"
  [[ -z "$mounted_root" ]] || stopped_container_receipt="$evidence_path/docker-inspect.json"
  [[ -z "$mounted_root" ]] || rotate_quiescent_root "$mounted_root"
}

enforce_retained_limit() {
  local current=$container_ref current_id ids candidates older mode
  current_id=$(docker inspect --format '{{.Id}}' "$current") || return 69
  ids=$(docker container ls --all --filter label=manyforge.dev.ros2-control --quiet --no-trunc) || return 69
  [[ -n "$ids" ]] || return 0
  local -a stopped_ids
  mapfile -t stopped_ids <<<"$ids"
  candidates=$(docker inspect "${stopped_ids[@]}" | python3 "$retention_helper" retained "$repo_root" \
    --current-id "$current_id" --legacy-evidence "${MANYFORGE_DEV_EVIDENCE_ROOT:-$cache_base/evidence}") || return 69
  while read -r older mode; do
    [[ -n "$older" ]] || continue
    # Requalify the actual stopped occurrence immediately before retrying it.
    [[ $(docker inspect "$older" | python3 "$retention_helper" retained "$repo_root" \
      --current-id "$current_id" --legacy-evidence "${MANYFORGE_DEV_EVIDENCE_ROOT:-$cache_base/evidence}") == "$older $mode" ]] || continue
    container_ref=$older
    if capture_stopped_container; then
      echo "Retried older retained container $older once; capture succeeded and container was removed" >&2
    elif [[ "$mode" == mounted && $(docker inspect -f '{{.State.Running}}' "$older") == false ]]; then
      if docker rm "$older" >/dev/null; then
        echo "WARNING ManyForge older retained container $older removed after one failed capture retry; persistent state remains, console capture failed" >&2
      else
        echo "WARNING ManyForge could not remove older retained container $older after retry" >&2
      fi
    else
      echo "WARNING ManyForge kept legacy container $older: failed retry may hold sole open-incident journal; manually export or explicitly remove after investigation" >&2
    fi
    container_ref=$current
  done <<<"$candidates"
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
        --bootstrap-project)
          [[ $# -ge 2 ]] || { usage; exit 64; }
          bootstrap_project=$2
          shift 2
          ;;
        *) usage; exit 64 ;;
      esac
    done
    if [[ -n "$bootstrap_project" && ( "$ros2_control" != true || -n "$project" ) ]]; then
      echo "--bootstrap-project requires ROS control and excludes --project." >&2
      exit 64
    fi
    if docker inspect "$container_ref" >/dev/null 2>&1; then
      assert_owned_container || return $?
      if [[ $(docker inspect -f '{{.State.Running}}' "$container_ref") == true ]]; then
        existing_ros=$(docker inspect -f '{{index .Config.Labels "manyforge.dev.ros2-control"}}' "$container_ref")
        if [[ "$existing_ros" == true && "$ros2_control" != true ]] ||
            [[ "$existing_ros" != true && "$ros2_control" == true ]]; then
          echo "A workbench with different runtime capabilities is already running; stop it before starting this ROS workbench." >&2
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
      existing_mount=$(container_state_mount) || exit 69
      if [[ -z "$existing_mount" && -d "$runtime_root" && -n $(find "$runtime_root" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
        echo "Refusing legacy unmounted transition into nonempty runtime root: $runtime_root; preserve/export legacy state and choose an empty root" >&2
        exit 69
      fi
      stop_with_evidence
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
    # applied Program; see DEVELOPMENT_WORKBENCH.md).
    state_base="$cache_base/state"
    if [[ "$operator_mode" == true ]]; then
      state_base="$cache_base/state/$operator_instance"
    fi
    composer_state="$state_base/composer"
    runner_state="$state_base/program-runner"
    mkdir -p "$composer_state" "$runner_state"
    serve_args=(serve)
    if [[ "$operator_mode" == true ]]; then
      runtime_root=${MANYFORGE_DEV_RUNTIME_STATE_ROOT:?explicit runtime state root required}
    fi
    [[ "$runtime_root" == /* ]] || { echo "Runtime state root must be an absolute host path" >&2; exit 64; }
    # The selected container is already absent here. A different live owned
    # container must never share these journals, even with an explicit override.
    assert_runtime_root_available || exit 69
    if [[ "$operator_mode" != true ]]; then
      mkdir -p "$runtime_root"
    fi
    startup_mounts=()
    operator_labels=()
    if [[ "$operator_mode" == true ]]; then
      [[ "$ros2_control" == true && -z "$project" && -z "$bootstrap_project" ]] || { echo "G requires ROS and no auto-Load" >&2; exit 64; }
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
      startup_mounts=(-v "$install_root:$install_root:ro")
      for option in COMPOSER_STATIC_DIR SIMULATION_CONTROL_CONFIG OPERATOR_NODE_CATALOG OPERATOR_ALARM_CATALOG; do
        variable="MANYFORGE_DEV_$option"
        if [[ -n ${!variable:-} ]]; then
          path=${!variable}
          [[ "$path" == /* ]] || { echo "Operator input must be absolute: $option" >&2; exit 64; }
          case "$option" in
            COMPOSER_STATIC_DIR) [[ -f "$path/index.html" ]] || exit 64; option_name=--composer-static-dir; parent=$path ;;
            SIMULATION_CONTROL_CONFIG) [[ -f "$path" ]] || exit 64; option_name=--simulation-control-config; parent=$(dirname "$path") ;;
            OPERATOR_NODE_CATALOG) [[ -f "$path" ]] || exit 64; option_name=--node-catalog-path; parent=$(dirname "$path") ;;
            OPERATOR_ALARM_CATALOG) [[ -f "$path" ]] || exit 64; option_name=--alarm-catalog-path; parent=$(dirname "$path") ;;
          esac
          startup_mounts+=(-v "$parent:$parent:ro")
          serve_args+=("$option_name" "$path")
        fi
      done
      operator_labels=(--log-driver json-file --label "manyforge.launch-instance=$operator_instance"
        --label "manyforge.launch-input-digest=$operator_digest")
    fi
    ros_environment=()
    ros_identity=()
    if [[ "$ros2_control" == true ]]; then
      # The full ROS image has its own development UID. Keep all bind-mounted
      # writes with the launching user's ownership, including npm/ROS caches.
      mkdir -p "$cache_root/home"
      for directory in "$repo_root" "$runtime_root" "$cache_root" "$cache_root/build" \
          "$cache_root/home" "$cache_root/node_modules" "$composer_state" "$runner_state"; do
        if [[ ! -w "$directory" ]]; then
          echo "ROS workbench requires a writable host-owned directory: $directory" >&2
          exit 69
        fi
      done
      ros_identity=(--user "$(id -u):$(id -g)" --env HOME=/runner/home \
        --env MANYFORGE_BUILD_DIR=/runner/build)
      # Preserve explicitly configured DDS and build inputs. The image entrypoint owns
      # ROS/venv setup; this launcher neither sources a topology nor picks peers.
      for variable in ROS_DOMAIN_ID RMW_IMPLEMENTATION ROS_AUTOMATIC_DISCOVERY_RANGE \
          ROS_STATIC_PEERS FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE \
          ROS_DISCOVERY_SERVER ROS_SUPER_CLIENT CYCLONEDDS_URI \
          MANYFORGE_ROS2_CONTROL_BUILD_TESTING MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS; do
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
    rotate_quiescent_root "$runtime_root"
    # Same inode lock as the child launcher: hold it across the final old-
    # process proof, durable session switch and creation of the new container.
    exec {session_lock_fd}<"$runtime_root"
    flock --exclusive --nonblock "$session_lock_fd" || { echo "Another workbench owns the state-root lock" >&2; exit 69; }
    assert_runtime_root_available || exit 69
    session_id=$(python3 -c 'import uuid;print(uuid.uuid4().hex)')
    session_proof=()
    [[ -z "$stopped_container_receipt" ]] || session_proof=(--stopped-container-receipt "$stopped_container_receipt")
    python3 "$session_helper" "$runtime_root" --session-id "$session_id" --lock-fd "$session_lock_fd" "${session_proof[@]}" >/dev/null
    session_environment=(-e "MANYFORGE_DEV_COLD_SESSION_ID=$session_id")
    for variable in MANYFORGE_MOTION_TRACE_DIR \
        MANYFORGE_EXPERIMENTAL_RELEASE_TIMER \
        MANYFORGE_EXPERIMENTAL_MIDTICK_STAMP MANYFORGE_EXPERIMENTAL_V0_PAYLOAD \
        MANYFORGE_EXPERIMENTAL_SCHEDULED_SPRING_BACK_FAST \
        MANYFORGE_EXPERIMENTAL_RELEASE_TIMER_LATENCY_MS \
        MANYFORGE_EXPERIMENTAL_REPLACEMENT_START_ALLOWANCE_MS; do
      [[ ! -v "$variable" ]] || session_environment+=(--env "$variable")
    done
    container_ref=$(docker run -d \
      --name "$container_name" \
      --label "manyforge.dev.ros2-control=$ros2_control" \
      --label "manyforge.workbench-session=$session_id" \
      "${operator_labels[@]}" \
      --network host \
      --init \
      --log-opt max-size=50m \
      --log-opt max-file=3 \
      -e MANYFORGE_DEV_BUILD_JOBS="${MANYFORGE_DEV_BUILD_JOBS:-}" \
      -e MANYFORGE_STATE_DIR=/var/lib/manyforge \
      "${session_environment[@]}" \
      "${ros_identity[@]}" \
      "${ros_environment[@]}" \
      "${startup_mounts[@]}" \
      -v "$repo_root:/workspace" \
      -v "$runtime_root:/tmp/manyforge-state" \
      -v "$cache_root:/runner" \
      -v "$composer_state:/var/lib/manyforge/composer" \
      -v "$runner_state:/var/lib/manyforge/program-runner" \
      -v "$cache_root/node_modules:/workspace/manyforge_composer/frontend/node_modules" \
      -w /workspace \
      "$image_id" \
      python3 scripts/dev-workbench.py "${serve_args[@]}")
    started_container=true
    python3 "$session_helper" "$runtime_root" --session-id "$session_id" --lock-fd "$session_lock_fd" --bind-container-id "$container_ref"
    # The running container is now the process-set boundary. Its launcher
    # takes the root flock before opening any store; contenders recheck all
    # live overlapping mounts while holding the lock before any rotation.
    exec {session_lock_fd}<&-
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
    if docker inspect "$container_ref" >/dev/null 2>&1; then
      stop_with_evidence
    elif [[ "$keep_evidence" == true || "$operator_mode" == true ]]; then
      echo "Cannot preserve evidence: development workbench container does not exist." >&2
      exit 69
    fi
    echo "Development workbench is stopped. Build and npm caches were retained."
    ;;
  *) usage; exit 64 ;;
esac
