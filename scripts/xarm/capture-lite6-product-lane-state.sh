#!/usr/bin/env bash
# Capture one bounded, read-only product-lane supervision/lifecycle snapshot.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
ACTIVE_RUN_FILE="${REPO_ROOT}/.manyforge-var/real-lite6/active-run"
COMPOSER_URL=${MANYFORGE_COMPOSER_URL:-http://127.0.0.1:9000}
CAPTURE_LABEL=${MANYFORGE_LITE6_CAPTURE_LABEL:-snapshot}
COMPOSER_CONTAINER=${MANYFORGE_REAL_ROBOT_CONTAINER:-manyforge-real-robot-composer}
OWNER_CONTAINER=${MANYFORGE_REAL_ROBOT_OWNER_CONTAINER:-manyforge-real-robot-owner}

if [[ ! "${CAPTURE_LABEL}" =~ ^[A-Za-z0-9._-]{1,64}$ ]]; then
    echo "MANYFORGE_LITE6_CAPTURE_LABEL must be a bounded filename token" >&2
    exit 2
fi
if [[ ! -f "${ACTIVE_RUN_FILE}" ]]; then
    echo "No active real-Lite6 product run is recorded" >&2
    exit 2
fi

mapfile -t active_run <"${ACTIVE_RUN_FILE}"
RUNTIME_ROOT=${active_run[0]:-}
case "${RUNTIME_ROOT}" in
    "${REPO_ROOT}/.manyforge-var/real-lite6/runs/"*) ;;
    *)
        echo "Active real-Lite6 runtime root is invalid" >&2
        exit 2
        ;;
esac
if [[ ! -d "${RUNTIME_ROOT}" ]]; then
    echo "Active real-Lite6 runtime root does not exist" >&2
    exit 2
fi

CAPTURE_ID="$(date -u +%Y%m%dT%H%M%S.%NZ)-${CAPTURE_LABEL}"
OUTPUT_ROOT="${RUNTIME_ROOT}/live-evidence/${CAPTURE_ID}"
mkdir -p "${OUTPUT_ROOT}/raw"

python3 - \
    "${COMPOSER_URL}" \
    "${RUNTIME_ROOT}" \
    "${OUTPUT_ROOT}" \
    "${COMPOSER_CONTAINER}" \
    "${OWNER_CONTAINER}" <<'PY'
from __future__ import annotations

from datetime import UTC, datetime
from hashlib import sha256
from pathlib import Path
import json
import subprocess
import sys
import time
import urllib.request


MAX_BYTES = 1024 * 1024
base_url = sys.argv[1].rstrip("/")
runtime_root = Path(sys.argv[2]).resolve()
output_root = Path(sys.argv[3]).resolve()
containers = tuple(sys.argv[4:6])
raw_root = output_root / "raw"
results: list[dict[str, object]] = []


def retain(name: str, payload: bytes, *, source: str) -> dict[str, object]:
    if len(payload) > MAX_BYTES:
        raise RuntimeError(f"{name}_output_exceeded_limit")
    path = raw_root / f"{name}.json"
    path.write_bytes(payload)
    parsed = json.loads(payload)
    if not isinstance(parsed, dict):
        raise RuntimeError(f"{name}_payload_not_object")
    return {
        "name": name,
        "source": source,
        "status": "captured",
        "bytes": len(payload),
        "sha256": sha256(payload).hexdigest(),
        "rawPath": str(path.relative_to(output_root)),
    }


def capture_url(name: str, route: str) -> None:
    url = f"{base_url}{route}"
    started = time.monotonic_ns()
    try:
        with urllib.request.urlopen(url, timeout=5.0) as response:
            payload = response.read(MAX_BYTES + 1)
        result = retain(name, payload, source=url)
    except (OSError, RuntimeError, ValueError) as exc:
        result = {
            "name": name,
            "source": url,
            "status": "failed",
            "reason": str(exc)[:512],
        }
    result["durationNs"] = time.monotonic_ns() - started
    results.append(result)


def capture_lifecycle_history() -> None:
    route = (
        "/api/execution/channels"
        "?limit=300&channels=robot_lifecycle"
    )
    url = f"{base_url}{route}"
    started = time.monotonic_ns()
    try:
        with urllib.request.urlopen(url, timeout=5.0) as response:
            source_payload = response.read(MAX_BYTES + 1)
        if len(source_payload) > MAX_BYTES:
            raise RuntimeError("robot_lifecycle_history_output_exceeded_limit")
        source = json.loads(source_payload)
        channels = source.get("channels") if isinstance(source, dict) else None
        if not isinstance(channels, dict):
            raise RuntimeError("robot_lifecycle_history_channels_invalid")
        events = sorted(
            (
                event
                for channel in channels.values()
                if isinstance(channel, list)
                for event in channel
                if isinstance(event, dict)
                and event.get("component") == "manyforge.robot_lifecycle"
            ),
            key=lambda event: int(event.get("event_index") or 0),
        )[-256:]
        payload = (
            json.dumps(
                {
                    "schema": (
                        "manyforge.robot_lifecycle.history_capture.v1"
                    ),
                    "source": url,
                    "sourceTimestampNs": source.get("timestampNs"),
                    "events": events,
                },
                sort_keys=True,
            )
            + "\n"
        ).encode("utf-8")
        result = retain(
            "robot-lifecycle-history",
            payload,
            source=url,
        )
    except (OSError, RuntimeError, ValueError) as exc:
        result = {
            "name": "robot-lifecycle-history",
            "source": url,
            "status": "failed",
            "reason": str(exc)[:512],
        }
    result["durationNs"] = time.monotonic_ns() - started
    results.append(result)


def capture_file(name: str, path: Path) -> None:
    started = time.monotonic_ns()
    try:
        payload = path.read_bytes()
        result = retain(name, payload, source=str(path))
    except (OSError, RuntimeError, ValueError) as exc:
        result = {
            "name": name,
            "source": str(path),
            "status": "failed",
            "reason": str(exc)[:512],
        }
    result["durationNs"] = time.monotonic_ns() - started
    results.append(result)


def capture_container(name: str) -> None:
    started = time.monotonic_ns()
    try:
        completed = subprocess.run(
            [
                "docker",
                "inspect",
                "--format",
                (
                    '{"id":{{json .Id}},"image":{{json .Image}},'
                    '"state":{{json .State}}}'
                ),
                name,
            ],
            check=False,
            capture_output=True,
            timeout=5.0,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                completed.stderr.decode("utf-8", errors="replace")[:512]
                or f"docker_inspect_exit_{completed.returncode}"
            )
        result = retain(
            f"container-{name}",
            completed.stdout,
            source=f"docker:{name}",
        )
    except (OSError, subprocess.SubprocessError, RuntimeError, ValueError) as exc:
        result = {
            "name": f"container-{name}",
            "source": f"docker:{name}",
            "status": "failed",
            "reason": str(exc)[:512],
        }
    result["durationNs"] = time.monotonic_ns() - started
    results.append(result)


for result_name, route in (
    ("runtime-status", "/api/runtime/status"),
    ("robot-lifecycle-status", "/api/robot-lifecycle/status"),
    ("cycle-status", "/api/cycle/status"),
    ("kernel-snapshot", "/api/kernel/snapshot"),
):
    capture_url(result_name, route)

capture_lifecycle_history()
capture_file(
    "cell-supervisor-heartbeat",
    runtime_root / "cell-supervisor-heartbeat.json",
)
capture_file(
    "cell-supervisor-policy",
    runtime_root / "cell-supervisor-policy.json",
)
for container in containers:
    capture_container(container)

artifact_hashes: dict[str, str] = {}
for relative in (
    "resolved/lite6-a/runtime-binding.json",
    "resolved/lite6-a/runtime-record.json",
):
    path = runtime_root / relative
    try:
        artifact_hashes[relative] = sha256(path.read_bytes()).hexdigest()
    except OSError:
        artifact_hashes[relative] = ""

success = all(result["status"] == "captured" for result in results)
evidence = {
    "schema": "manyforge.real_lite6.product_state_capture.v1",
    "capturedAtUtc": datetime.now(UTC).isoformat(),
    "capturedAtSteadyTimeNs": time.monotonic_ns(),
    "runtimeRoot": str(runtime_root),
    "artifactSha256": artifact_hashes,
    "results": results,
    "success": success,
}
evidence_path = output_root / "evidence.json"
temporary = output_root / ".evidence.json.tmp"
temporary.write_text(
    json.dumps(evidence, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
temporary.replace(evidence_path)
print(evidence_path)
raise SystemExit(0 if success else 1)
PY
