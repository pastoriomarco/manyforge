#!/usr/bin/env bash
# Execute and retain evidence for exactly one attended Lite6 lifecycle action.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
CONTAINER=${MANYFORGE_LITE6_CONTAINER:-manyforge-lite6-supervised}
ACTION=${MANYFORGE_LITE6_ACTION:-}
CONFIRMATION=${MANYFORGE_LITE6_ACTION_CONFIRMATION:-}
RUNTIME_BINDING=${MANYFORGE_LITE6_RUNTIME_BINDING:-/tmp/manyforge-real-lite6/resolved/lite6-a/runtime-binding.json}
OUTPUT_ROOT=${MANYFORGE_LITE6_EVIDENCE_ROOT:-${REPO_ROOT}/.manyforge-var/real-lite6/live-evidence}
COMMAND_SOURCE=${MANYFORGE_LITE6_COMMAND_SOURCE:-composer.local_operator}
CALLER_CONTEXT_ID=${MANYFORGE_LITE6_CALLER_CONTEXT_ID:-local-attended-qualification}
TIMEOUT_MS=${MANYFORGE_LITE6_COMMAND_TIMEOUT_MS:-8000}
TRANSIENT_CAPTURE_MS=${MANYFORGE_LITE6_TRANSIENT_CAPTURE_MS:-3000}
TRANSIENT_POSE_LIMIT_RAD=${MANYFORGE_LITE6_TRANSIENT_POSE_LIMIT_RAD:-}
SHUTDOWN_CONFIRMATION=${MANYFORGE_LITE6_SHUTDOWN_CONFIRMATION:-}
OWNER_LABEL=org.manyforge.xarm-transport-owner
OWNER_ID=lite6-a:sole-owner

case "${ACTION}" in
    robot.clear_fault|robot.clear_warning|robot.enable|\
    robot.select_external_control|robot.start_control|\
    robot.stop_control|robot.disable|shutdown_controller) ;;
    robot.prepare_for_motion|robot.make_noncommandable) ;;
    *)
        echo "MANYFORGE_LITE6_ACTION must name one supported lifecycle action" >&2
        exit 2
        ;;
esac

if [[ "${CONFIRMATION}" != "${ACTION}" ]]; then
    echo "MANYFORGE_LITE6_ACTION_CONFIRMATION must exactly match ${ACTION}" >&2
    exit 2
fi
if [[ "${ACTION}" == "shutdown_controller" ]] &&
   [[ "${SHUTDOWN_CONFIRMATION}" != "SHUTDOWN_CONTROLLER" ]]; then
    echo "MANYFORGE_LITE6_SHUTDOWN_CONFIRMATION must exactly equal SHUTDOWN_CONTROLLER" >&2
    exit 2
fi
if [[ ! "${TIMEOUT_MS}" =~ ^[0-9]+$ ]] || ((TIMEOUT_MS < 1000 || TIMEOUT_MS > 30000)); then
    echo "MANYFORGE_LITE6_COMMAND_TIMEOUT_MS must be 1000..30000" >&2
    exit 2
fi
if [[ ! "${TRANSIENT_CAPTURE_MS}" =~ ^[0-9]+$ ]] ||
   ((TRANSIENT_CAPTURE_MS < 100 || TRANSIENT_CAPTURE_MS > 10000)); then
    echo "MANYFORGE_LITE6_TRANSIENT_CAPTURE_MS must be 100..10000" >&2
    exit 2
fi
if [[ -n "${TRANSIENT_POSE_LIMIT_RAD}" ]] &&
   { [[ ! "${TRANSIENT_POSE_LIMIT_RAD}" =~ ^(0|[1-9][0-9]*)(\.[0-9]+)?$ ]] ||
     ! awk -v value="${TRANSIENT_POSE_LIMIT_RAD}" 'BEGIN { exit !(value > 0) }'; }; then
    echo "MANYFORGE_LITE6_TRANSIENT_POSE_LIMIT_RAD must be empty or a positive explicit bound" >&2
    exit 2
fi
if [[ "$(docker inspect "${CONTAINER}" --format '{{.State.Running}}' 2>/dev/null || true)" != "true" ]]; then
    echo "Lite6 supervised owner is not running: ${CONTAINER}" >&2
    exit 2
fi
if [[ "$(docker inspect "${CONTAINER}" --format '{{.HostConfig.NetworkMode}}')" != "host" ]] ||
   [[ "$(docker inspect "${CONTAINER}" --format '{{.HostConfig.IpcMode}}')" != "host" ]]; then
    echo "Lite6 supervised owner must use host network and host IPC" >&2
    exit 2
fi
if [[ "$(docker inspect "${CONTAINER}" --format "{{index .Config.Labels \"${OWNER_LABEL}\"}}")" != "${OWNER_ID}" ]]; then
    echo "Container is not the labeled Lite6 supervised owner" >&2
    exit 2
fi
if ! docker exec "${CONTAINER}" test -f "${RUNTIME_BINDING}"; then
    echo "Resolved runtime binding is absent in ${CONTAINER}" >&2
    exit 2
fi
if ! docker exec "${CONTAINER}" test -f \
    /tmp/manyforge-build/real-lite6/install/setup.bash; then
    echo "ManyForge real-Lite6 runtime is absent in ${CONTAINER}" >&2
    exit 2
fi
"${SCRIPT_DIR}/verify-lite6-supervised-topology.sh"

CAPTURE_ID="$(date -u +%Y%m%dT%H%M%SZ)-${ACTION//./-}"
REQUEST_ID="attended-${CAPTURE_ID}"
AUTHORITY_SESSION_ID="attended-session-${CAPTURE_ID}"
HOST_OUTPUT="${OUTPUT_ROOT}/${CAPTURE_ID}"
CONTAINER_OUTPUT="/tmp/manyforge-lite6-command-${CAPTURE_ID}"
if [[ -e "${HOST_OUTPUT}" ]]; then
    echo "Evidence output already exists: ${HOST_OUTPUT}" >&2
    exit 2
fi
mkdir -p "${OUTPUT_ROOT}"

set +e
docker exec \
    -e MANYFORGE_EVIDENCE_ACTION="${ACTION}" \
    -e MANYFORGE_EVIDENCE_CONFIRMATION="${CONFIRMATION}" \
    -e MANYFORGE_EVIDENCE_REQUEST_ID="${REQUEST_ID}" \
    -e MANYFORGE_EVIDENCE_SESSION_ID="${AUTHORITY_SESSION_ID}" \
    -e MANYFORGE_EVIDENCE_COMMAND_SOURCE="${COMMAND_SOURCE}" \
    -e MANYFORGE_EVIDENCE_CALLER_CONTEXT="${CALLER_CONTEXT_ID}" \
    -e MANYFORGE_EVIDENCE_RUNTIME_BINDING="${RUNTIME_BINDING}" \
    -e MANYFORGE_EVIDENCE_OUTPUT="${CONTAINER_OUTPUT}" \
    -e MANYFORGE_EVIDENCE_TIMEOUT_MS="${TIMEOUT_MS}" \
    -e MANYFORGE_EVIDENCE_TRANSIENT_CAPTURE_MS="${TRANSIENT_CAPTURE_MS}" \
    -e MANYFORGE_EVIDENCE_TRANSIENT_POSE_LIMIT_RAD="${TRANSIENT_POSE_LIMIT_RAD}" \
    "${CONTAINER}" \
    bash -lc '
      set -euo pipefail
      set +u
      source /opt/ros/jazzy/setup.bash
      source /opt/manyforge/xarm/install/setup.bash
      source /tmp/manyforge-build/real-lite6/install/setup.bash
      set -u
      transient_args=()
      if [[ -n "${MANYFORGE_EVIDENCE_TRANSIENT_POSE_LIMIT_RAD}" ]]; then
        transient_args+=(
          --max-transient-deviation-rad
          "${MANYFORGE_EVIDENCE_TRANSIENT_POSE_LIMIT_RAD}"
        )
      fi
      ros2 run manyforge_xarm_adapter \
        manyforge-xarm-capture-supervised-command \
        --runtime-binding "${MANYFORGE_EVIDENCE_RUNTIME_BINDING}" \
        --output "${MANYFORGE_EVIDENCE_OUTPUT}" \
        --action-id "${MANYFORGE_EVIDENCE_ACTION}" \
        --confirm-action "${MANYFORGE_EVIDENCE_CONFIRMATION}" \
        --request-id "${MANYFORGE_EVIDENCE_REQUEST_ID}" \
        --authority-session-id "${MANYFORGE_EVIDENCE_SESSION_ID}" \
        --command-source "${MANYFORGE_EVIDENCE_COMMAND_SOURCE}" \
        --caller-context-id "${MANYFORGE_EVIDENCE_CALLER_CONTEXT}" \
        --timeout-ms "${MANYFORGE_EVIDENCE_TIMEOUT_MS}" \
        --transient-capture-ms \
          "${MANYFORGE_EVIDENCE_TRANSIENT_CAPTURE_MS}" \
        "${transient_args[@]}"
    '
CAPTURE_STATUS=$?
set -e

if ! docker cp "${CONTAINER}:${CONTAINER_OUTPUT}" "${HOST_OUTPUT}"; then
    echo "Command evidence could not be copied from ${CONTAINER}" >&2
    exit 1
fi
docker exec "${CONTAINER}" rm -r -- "${CONTAINER_OUTPUT}"

IMAGE_NAME=$(docker inspect "${CONTAINER}" --format '{{.Config.Image}}')
IMAGE_ID=$(docker inspect "${CONTAINER}" --format '{{.Image}}')
SOURCE_COMMIT=$(git -C "${REPO_ROOT}" rev-parse HEAD)
SOURCE_TREE=$(git -C "${REPO_ROOT}" rev-parse HEAD^{tree})
SOURCE_DIRTY=false
if [[ -n "$(git -C "${REPO_ROOT}" status --porcelain=v1 --untracked-files=all)" ]]; then
    SOURCE_DIRTY=true
fi
python3 - \
    "${HOST_OUTPUT}/capture-provenance.json" \
    "${IMAGE_NAME}" \
    "${IMAGE_ID}" \
    "${SOURCE_COMMIT}" \
    "${SOURCE_TREE}" \
    "${SOURCE_DIRTY}" \
    "${ACTION}" <<'PY'
from __future__ import annotations

import json
import os
from pathlib import Path
import sys

path = Path(sys.argv[1])
payload = {
    'schema': 'manyforge.real_lite6.command_capture_provenance.v1',
    'image': sys.argv[2],
    'image_id': sys.argv[3],
    'manyforge_commit': sys.argv[4],
    'manyforge_tree': sys.argv[5],
    'manyforge_dirty': sys.argv[6] == 'true',
    'action_id': sys.argv[7],
    'network_address_retained': False,
}
temporary = path.with_name(f'.{path.name}.{os.getpid()}.tmp')
with temporary.open('x', encoding='utf-8') as handle:
    json.dump(payload, handle, indent=2, sort_keys=True)
    handle.write('\n')
    handle.flush()
    os.fsync(handle.fileno())
os.replace(temporary, path)
PY

echo "Lite6 command evidence retained at ${HOST_OUTPUT}"
if ((CAPTURE_STATUS != 0)); then
    echo "The command evidence gate failed; inspect evidence.json and raw output." >&2
fi
exit "${CAPTURE_STATUS}"
