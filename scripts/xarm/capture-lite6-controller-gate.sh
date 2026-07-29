#!/usr/bin/env bash
# Capture attended no-trajectory Lite6 controller/gate evidence.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
CONTAINER=${MANYFORGE_LITE6_CONTAINER:-manyforge-lite6-supervised}
CONFIRMATION=${MANYFORGE_LITE6_CONTROLLER_GATE_CONFIRMATION:-}
POSE_LIMIT_RAD=${MANYFORGE_LITE6_CONTROLLER_GATE_POSE_LIMIT_RAD:-}
RUNTIME_BINDING=${MANYFORGE_LITE6_RUNTIME_BINDING:-/tmp/manyforge-real-lite6/resolved/lite6-a/runtime-binding.json}
OUTPUT_ROOT=${MANYFORGE_LITE6_EVIDENCE_ROOT:-${REPO_ROOT}/.manyforge-var/real-lite6/live-evidence}
TIMEOUT_S=${MANYFORGE_LITE6_CONTROLLER_GATE_TIMEOUT_S:-15}
OWNER_LABEL=org.manyforge.xarm-transport-owner
OWNER_ID=lite6-a:sole-owner

if [[ "${CONFIRMATION}" != "QUALIFY_NO_TRAJECTORY_CONTROLLER_GATE" ]]; then
    echo "Set MANYFORGE_LITE6_CONTROLLER_GATE_CONFIRMATION=QUALIFY_NO_TRAJECTORY_CONTROLLER_GATE" >&2
    exit 2
fi
if [[ ! "${POSE_LIMIT_RAD}" =~ ^(0|[1-9][0-9]*)(\.[0-9]+)?$ ]] ||
   ! awk -v value="${POSE_LIMIT_RAD}" 'BEGIN { exit !(value > 0) }'; then
    echo "MANYFORGE_LITE6_CONTROLLER_GATE_POSE_LIMIT_RAD must be a positive explicit bound" >&2
    exit 2
fi
if [[ ! "${TIMEOUT_S}" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
   ! awk -v value="${TIMEOUT_S}" 'BEGIN { exit !(value >= 5 && value <= 60) }'; then
    echo "MANYFORGE_LITE6_CONTROLLER_GATE_TIMEOUT_S must be 5..60" >&2
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
"${SCRIPT_DIR}/verify-lite6-supervised-topology.sh"

CAPTURE_ID="$(date -u +%Y%m%dT%H%M%SZ)-controller-gate"
REQUEST_ID="attended-${CAPTURE_ID}"
HOST_OUTPUT="${OUTPUT_ROOT}/${CAPTURE_ID}"
CONTAINER_OUTPUT="/tmp/manyforge-lite6-controller-gate-${CAPTURE_ID}"
if [[ -e "${HOST_OUTPUT}" ]]; then
    echo "Evidence output already exists: ${HOST_OUTPUT}" >&2
    exit 2
fi
mkdir -p "${OUTPUT_ROOT}"

set +e
docker exec \
    -e MANYFORGE_EVIDENCE_REQUEST_ID="${REQUEST_ID}" \
    -e MANYFORGE_EVIDENCE_CONFIRMATION="${CONFIRMATION}" \
    -e MANYFORGE_EVIDENCE_POSE_LIMIT_RAD="${POSE_LIMIT_RAD}" \
    -e MANYFORGE_EVIDENCE_RUNTIME_BINDING="${RUNTIME_BINDING}" \
    -e MANYFORGE_EVIDENCE_OUTPUT="${CONTAINER_OUTPUT}" \
    -e MANYFORGE_EVIDENCE_TIMEOUT_S="${TIMEOUT_S}" \
    "${CONTAINER}" \
    bash -lc '
      set -euo pipefail
      set +u
      source /opt/ros/jazzy/setup.bash
      source /opt/manyforge/xarm/install/setup.bash
      source /tmp/manyforge-build/real-lite6/install/setup.bash
      set -u
      ros2 run manyforge_xarm_adapter \
        manyforge-xarm-capture-controller-gate \
        --runtime-binding "${MANYFORGE_EVIDENCE_RUNTIME_BINDING}" \
        --output "${MANYFORGE_EVIDENCE_OUTPUT}" \
        --request-id "${MANYFORGE_EVIDENCE_REQUEST_ID}" \
        --confirm-operation "${MANYFORGE_EVIDENCE_CONFIRMATION}" \
        --max-pose-deviation-rad "${MANYFORGE_EVIDENCE_POSE_LIMIT_RAD}" \
        --timeout-s "${MANYFORGE_EVIDENCE_TIMEOUT_S}"
    '
CAPTURE_STATUS=$?
set -e

if ! docker cp "${CONTAINER}:${CONTAINER_OUTPUT}" "${HOST_OUTPUT}"; then
    echo "Controller/gate evidence could not be copied from ${CONTAINER}" >&2
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
    "${POSE_LIMIT_RAD}" <<'PY'
from __future__ import annotations

import json
import os
from pathlib import Path
import sys

path = Path(sys.argv[1])
payload = {
    'schema': 'manyforge.real_lite6.controller_gate_provenance.v1',
    'image': sys.argv[2],
    'image_id': sys.argv[3],
    'manyforge_commit': sys.argv[4],
    'manyforge_tree': sys.argv[5],
    'manyforge_dirty': sys.argv[6] == 'true',
    'max_pose_deviation_limit_rad': float(sys.argv[7]),
    'network_address_retained': False,
    'trajectory_published': False,
}
temporary = path.with_name(f'.{path.name}.{os.getpid()}.tmp')
with temporary.open('x', encoding='utf-8') as handle:
    json.dump(payload, handle, indent=2, sort_keys=True)
    handle.write('\n')
    handle.flush()
    os.fsync(handle.fileno())
os.replace(temporary, path)
PY

echo "Lite6 controller/gate evidence retained at ${HOST_OUTPUT}"
if ((CAPTURE_STATUS != 0)); then
    echo "The controller/gate evidence gate failed; inspect evidence.json." >&2
fi
exit "${CAPTURE_STATUS}"
