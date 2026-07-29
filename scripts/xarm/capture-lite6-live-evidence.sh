#!/usr/bin/env bash
# Copy one bounded, command-free Lite6 evidence capture from the live container.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
CONTAINER=${MANYFORGE_LITE6_CONTAINER:-manyforge-lite6-read-only}
MODE=${MANYFORGE_LITE6_EVIDENCE_MODE:-read_only}
OUTPUT_ROOT=${MANYFORGE_LITE6_EVIDENCE_ROOT:-${REPO_ROOT}/.manyforge-var/real-lite6/live-evidence}

case "${MODE}" in
    read_only|supervised_preflight) ;;
    *)
        echo "MANYFORGE_LITE6_EVIDENCE_MODE must be read_only or supervised_preflight" >&2
        exit 2
        ;;
esac

if [[ "$(docker inspect "${CONTAINER}" --format '{{.State.Running}}' 2>/dev/null || true)" != "true" ]]; then
    echo "Lite6 container is not running: ${CONTAINER}" >&2
    exit 2
fi

SESSION_ID="$(date -u +%Y%m%dT%H%M%SZ)-${MODE}"
HOST_OUTPUT="${OUTPUT_ROOT}/${SESSION_ID}"
CONTAINER_OUTPUT="/tmp/manyforge-lite6-evidence-${SESSION_ID}"
if [[ -e "${HOST_OUTPUT}" ]]; then
    echo "Evidence output already exists: ${HOST_OUTPUT}" >&2
    exit 2
fi
mkdir -p "${OUTPUT_ROOT}"

set +e
docker exec \
    -e MANYFORGE_EVIDENCE_MODE="${MODE}" \
    -e MANYFORGE_EVIDENCE_OUTPUT="${CONTAINER_OUTPUT}" \
    "${CONTAINER}" \
    bash -lc '
      set -euo pipefail
      set +u
      source /opt/ros/jazzy/setup.bash
      source /opt/manyforge/xarm/install/setup.bash
      source /tmp/manyforge-build/real-lite6/install/setup.bash
      set -u
      ros2 run manyforge_xarm_adapter manyforge-xarm-capture-live-evidence \
        --mode "${MANYFORGE_EVIDENCE_MODE}" \
        --binding-manifest /tmp/manyforge-real-lite6/binding-manifest.json \
        --output "${MANYFORGE_EVIDENCE_OUTPUT}"
    '
CAPTURE_STATUS=$?
set -e

if ! docker cp "${CONTAINER}:${CONTAINER_OUTPUT}" "${HOST_OUTPUT}"; then
    echo "Live evidence could not be copied from ${CONTAINER}" >&2
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
    "${SOURCE_DIRTY}" <<'PY'
from __future__ import annotations

import json
import os
from pathlib import Path
import sys

path = Path(sys.argv[1])
payload = {
    'schema': 'manyforge.real_lite6.live_capture_provenance.v1',
    'image': sys.argv[2],
    'image_id': sys.argv[3],
    'manyforge_commit': sys.argv[4],
    'manyforge_tree': sys.argv[5],
    'manyforge_dirty': sys.argv[6] == 'true',
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

echo "Lite6 live evidence retained at ${HOST_OUTPUT}"
if ((CAPTURE_STATUS != 0)); then
    echo "The evidence gate failed; inspect evidence.json and raw probes." >&2
fi
exit "${CAPTURE_STATUS}"
