#!/usr/bin/env bash
# Run and provenance-bind the Step 18 execution-kernel 30x3 loaded gate.

set -euo pipefail

ORIGINAL_ARGV=("$0" "$@")
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SPECS_ROOT="${MANYFORGE_SPECS_ROOT:-${REPO_ROOT}/../manyforge_specs}"
SOURCE_HELPER="${REPO_ROOT}/scripts/qualification/source_identity.py"
VERIFIER="${REPO_ROOT}/scripts/qualification/verify_step18_loaded_kernel.py"
GATE_SCRIPT="${REPO_ROOT}/manyforge_core/tools/stability_gate.sh"
COMPOSE_FILE="${REPO_ROOT}/docker-compose.yaml"
CPUSET="${MANYFORGE_STEP18_LOADED_KERNEL_CPUSET:-10-13}"
BUILD_JOBS="${MANYFORGE_BUILD_JOBS:-2}"
OUTPUT_DIR=""
BUILD_DIR=""

usage() {
  printf '%s\n' \
    'usage: run-step18-loaded-kernel-qualification.sh --output-dir DIR [options]' \
    '' \
    'Options:' \
    '  --cpuset CPUSET       exact four-CPU affinity (default: 10-13)' \
    '  --specs-root DIR      sibling manyforge_specs checkout' \
    '  --build-dir DIR       fresh retained build below ManyForge/build' >&2
}

while (($# > 0)); do
  case "$1" in
    --output-dir)
      [[ $# -ge 2 ]] || { echo "--output-dir requires a value" >&2; exit 2; }
      OUTPUT_DIR="$2"; shift 2 ;;
    --cpuset)
      [[ $# -ge 2 ]] || { echo "--cpuset requires a value" >&2; exit 2; }
      CPUSET="$2"; shift 2 ;;
    --specs-root)
      [[ $# -ge 2 ]] || { echo "--specs-root requires a value" >&2; exit 2; }
      SPECS_ROOT="$2"; shift 2 ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "--build-dir requires a value" >&2; exit 2; }
      BUILD_DIR="$2"; shift 2 ;;
    --help|-h)
      usage; exit 0 ;;
    *)
      echo "unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "${OUTPUT_DIR}" ]]; then
  echo "--output-dir is required" >&2
  usage
  exit 2
fi
for path in "${SOURCE_HELPER}" "${VERIFIER}" "${GATE_SCRIPT}" "${COMPOSE_FILE}"; do
  if [[ ! -f "${path}" ]]; then
    echo "required qualification input is missing: ${path}" >&2
    exit 2
  fi
done
if [[ ! -d "${SPECS_ROOT}/.git" ]]; then
  echo "manyforge_specs checkout is unavailable: ${SPECS_ROOT}" >&2
  exit 2
fi

PROCESS_INSTANCE_ID="$(< /proc/sys/kernel/random/uuid)"
if [[ -z "${BUILD_DIR}" ]]; then
  BUILD_DIR="${REPO_ROOT}/build/step18-loaded-kernel-${PROCESS_INSTANCE_ID}"
fi

OUTPUT_DIR="$(realpath -m "${OUTPUT_DIR}")"
BUILD_DIR="$(realpath -m "${BUILD_DIR}")"
SPECS_ROOT="$(realpath "${SPECS_ROOT}")"
case "${OUTPUT_DIR}" in
  "${REPO_ROOT}/.manyforge-var/qualification/"*) ;;
  *)
    echo "output directory must be below ${REPO_ROOT}/.manyforge-var/qualification" >&2
    exit 2
    ;;
esac
if ! git -C "${REPO_ROOT}" check-ignore --quiet -- "${OUTPUT_DIR}"; then
  echo "output directory is not covered by the checkout's ignore rules: ${OUTPUT_DIR}" >&2
  exit 2
fi
case "${BUILD_DIR}" in
  "${REPO_ROOT}/build/"*) ;;
  *)
    echo "build directory must be below ${REPO_ROOT}/build" >&2
    exit 2
    ;;
esac
if [[ -e "${BUILD_DIR}" ]]; then
  echo "loaded-kernel build directory must not already exist: ${BUILD_DIR}" >&2
  exit 2
fi
if [[ -e "${OUTPUT_DIR}" ]]; then
  echo "output directory already exists: ${OUTPUT_DIR}" >&2
  exit 2
fi
mkdir -p "${OUTPUT_DIR}/raw"

if [[ ! "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "MANYFORGE_BUILD_JOBS must be a positive integer" >&2
  exit 2
fi

mapfile -t CPU_PROVENANCE < <(python3 - "${CPUSET}" <<'PY'
import sys
from pathlib import Path

cpus = set()
for part in sys.argv[1].split(","):
    if "-" in part:
        first, last = (int(value) for value in part.split("-", 1))
        if first > last:
            raise SystemExit("CPU range is descending")
        cpus.update(range(first, last + 1))
    else:
        cpus.add(int(part))
if len(cpus) != 4:
    raise SystemExit("Step 18 loaded-kernel affinity must contain exactly four CPUs")
ordered = sorted(cpus)
ranges = []
start = previous = ordered[0]
for cpu in ordered[1:]:
    if cpu == previous + 1:
        previous = cpu
        continue
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    start = previous = cpu
ranges.append(str(start) if start == previous else f"{start}-{previous}")
governors = set()
drivers = set()
for cpu in ordered:
    root = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    for name, destination in (
        ("scaling_governor", governors),
        ("scaling_driver", drivers),
    ):
        path = root / name
        if not path.is_file():
            raise SystemExit(f"missing CPU frequency provenance: {path}")
        value = path.read_text(encoding="utf-8").strip()
        if not value:
            raise SystemExit(f"empty CPU frequency provenance: {path}")
        destination.add(value)
print(",".join(ranges))
print(",".join(sorted(governors)))
print(",".join(sorted(drivers)))
PY
)
if [[ ${#CPU_PROVENANCE[@]} -ne 3 ]]; then
  echo "failed to collect exact CPU provenance" >&2
  exit 2
fi
CPUSET="${CPU_PROVENANCE[0]}"
CPU_GOVERNOR_SET="${CPU_PROVENANCE[1]}"
CPU_FREQUENCY_DRIVER_SET="${CPU_PROVENANCE[2]}"
HOST_KERNEL_ID="$(uname -srvm)"

identity_json() {
  python3 "${SOURCE_HELPER}" --repo-root "$1"
}

specs_identity_bundle_json() {
  python3 "${SOURCE_HELPER}" --repo-root "$1" --include-specs-contract-identity
}

MANYFORGE_BEFORE="$(identity_json "${REPO_ROOT}")"
SPECS_BUNDLE_BEFORE="$(specs_identity_bundle_json "${SPECS_ROOT}")"
MANYFORGE_FINGERPRINT="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["fingerprint_sha256"])' <<<"${MANYFORGE_BEFORE}")"
SPECS_CONTRACT_FINGERPRINT="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["specs_contract_identity"]["contract_fingerprint_sha256"])' <<<"${SPECS_BUNDLE_BEFORE}")"

COMPOSE_PROJECT_NAME_RESOLVED="$(
  docker compose -f "${COMPOSE_FILE}" config --format json |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["name"])'
)"
if [[ ! "${COMPOSE_PROJECT_NAME_RESOLVED}" =~ ^[a-z0-9][a-z0-9_-]*$ ]]; then
  echo "resolved Compose project name is not canonical: ${COMPOSE_PROJECT_NAME_RESOLVED}" >&2
  exit 2
fi
COMPOSE_CONFIG_JSON="$(
  docker compose --project-name "${COMPOSE_PROJECT_NAME_RESOLVED}" \
    -f "${COMPOSE_FILE}" config --format json
)"
IMAGE_REFERENCE="${COMPOSE_PROJECT_NAME_RESOLVED}-dev"
IMAGE_ID="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
IMAGE_ID="${IMAGE_ID#sha256:}"
if [[ ! "${IMAGE_ID}" =~ ^[0-9a-f]{64}$ ]]; then
  echo "exact ${IMAGE_REFERENCE} image is unavailable; build the dev service first" >&2
  exit 1
fi

RUN_ID="$(date -u +%Y%m%dT%H%M%S%NZ)-step18-loaded-kernel-${PROCESS_INSTANCE_ID}"
CONTAINER_NAME="manyforge-step18-loaded-${PROCESS_INSTANCE_ID}"
EXPECTED_MOUNTS_JSON="$(python3 - "${COMPOSE_CONFIG_JSON}" <<'PY'
import json
import sys
from pathlib import Path

config = json.loads(sys.argv[1])
volumes = config.get("services", {}).get("dev", {}).get("volumes")
declared_volumes = config.get("volumes", {})
if not isinstance(volumes, list) or not isinstance(declared_volumes, dict):
    raise SystemExit("Compose dev service mount inventory is unavailable")
normalized = []
for mount in volumes:
    if not isinstance(mount, dict):
        raise SystemExit("Compose dev service has a malformed mount")
    kind = mount.get("type")
    source = mount.get("source")
    destination = mount.get("target")
    if kind == "bind":
        source = str(Path(str(source)).resolve())
    elif kind == "volume":
        declaration = declared_volumes.get(str(source))
        if not isinstance(declaration, dict) or not declaration.get("name"):
            raise SystemExit(f"Compose named volume {source!r} has no resolved name")
        source = declaration["name"]
    else:
        raise SystemExit(f"unsupported Compose dev mount type: {kind!r}")
    normalized.append(
        {
            "type": kind,
            "source": source,
            "destination": destination,
            "rw": not bool(mount.get("read_only", False)),
        }
    )
normalized.sort(key=lambda value: (value["destination"], value["type"], value["source"]))
if [value["destination"] for value in normalized] != ["/tmp/manyforge-build", "/workspace"]:
    raise SystemExit("Compose dev service mount inventory changed from the exact qualification lane")
if any(value["rw"] is not True for value in normalized):
    raise SystemExit("Compose dev qualification mounts must be read-write")
print(json.dumps(normalized, sort_keys=True, separators=(",", ":")))
PY
)"
CONTAINER_LABELS_JSON="$(python3 - \
  "${RUN_ID}" "${PROCESS_INSTANCE_ID}" "${MANYFORGE_FINGERPRINT}" \
  "${SPECS_CONTRACT_FINGERPRINT}" "${COMPOSE_PROJECT_NAME_RESOLVED}" <<'PY'
import json
import sys

run_id, process_id, source_fingerprint, specs_fingerprint, project = sys.argv[1:]
print(json.dumps({
    "io.manyforge.qualification.kind": "step18-loaded-kernel",
    "io.manyforge.qualification.run_id": run_id,
    "io.manyforge.qualification.process_instance_id": process_id,
    "io.manyforge.qualification.source_fingerprint_sha256": source_fingerprint,
    "io.manyforge.qualification.specs_contract_fingerprint_sha256": specs_fingerprint,
    "io.manyforge.qualification.compose_project": project,
}, sort_keys=True, separators=(",", ":")))
PY
)"
mapfile -t CONTAINER_LABEL_ARGS < <(python3 -c '
import json, sys
for key, value in sorted(json.loads(sys.argv[1]).items()):
    print(f"{key}={value}")
' "${CONTAINER_LABELS_JSON}")
OUTPUT_RELATIVE="${OUTPUT_DIR#"${REPO_ROOT}/"}"
BUILD_RELATIVE="${BUILD_DIR#"${REPO_ROOT}/"}"
CONTAINER_OUTPUT="/workspace/${OUTPUT_RELATIVE}"
CONTAINER_BUILD="/workspace/${BUILD_RELATIVE}"
RAW_MANIFEST="${OUTPUT_DIR}/raw/stability-gate-manifest.json"
CONTAINER_INSPECT="${OUTPUT_DIR}/container-inspect.json"
CONTAINER_LOG="${OUTPUT_DIR}/container.log"
RUN_MANIFEST="${OUTPUT_DIR}/run-manifest.json"
VERIFICATION_SUMMARY="${OUTPUT_DIR}/verification-summary.json"

CONTAINER_ENVIRONMENT_JSON="$(python3 - \
  "${RUN_ID}" "${PROCESS_INSTANCE_ID}" "${CONTAINER_OUTPUT}/raw" \
  "${CONTAINER_BUILD}/test_execution_kernel" "${MANYFORGE_FINGERPRINT}" \
  "${SPECS_CONTRACT_FINGERPRINT}" "${BUILD_JOBS}" <<'PY'
import json
import sys
from pathlib import Path

(
    run_id,
    process_id,
    log_dir,
    binary,
    source_fingerprint,
    specs_fingerprint,
    build_jobs,
) = sys.argv[1:]
print(json.dumps({
    "LD_PRELOAD": "",
    "SEGFAULT_SIGNALS": "",
    "MANYFORGE_BUILD_JOBS": build_jobs,
    "MANYFORGE_STABILITY_RUN_ID": run_id,
    "MANYFORGE_STABILITY_PROCESS_INSTANCE_ID": process_id,
    "MANYFORGE_STABILITY_LOG_DIR": log_dir,
    "MANYFORGE_STABILITY_MANIFEST": f"{log_dir}/stability-gate-manifest.json",
    "MANYFORGE_STABILITY_TEST_BINARY": binary,
    "MANYFORGE_CORE_BUILD_DIR": str(Path(binary).parent),
    "MANYFORGE_STABILITY_ITERATIONS": "30",
    "MANYFORGE_STABILITY_LOAD_WORKERS": "3",
    "MANYFORGE_STABILITY_TIMEOUT_SECONDS": "30",
    "MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256": source_fingerprint,
    "MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256": specs_fingerprint,
}, sort_keys=True, separators=(",", ":")))
PY
)"
mapfile -t CONTAINER_ENVIRONMENT_ARGS < <(python3 -c '
import json, sys
for key, value in sorted(json.loads(sys.argv[1]).items()):
    print(f"{key}={value}")
' "${CONTAINER_ENVIRONMENT_JSON}")

INNER_SCRIPT='set -euo pipefail
python3 /workspace/scripts/qualification/source_identity.py --repo-root /workspace --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256}" >/dev/null
if [[ -e "${MANYFORGE_CORE_BUILD_DIR}" ]]; then
  echo "loaded-kernel build directory was not fresh: ${MANYFORGE_CORE_BUILD_DIR}" >&2
  exit 2
fi
cmake -S /workspace/manyforge_core -B "${MANYFORGE_STABILITY_TEST_BINARY%/test_execution_kernel}" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Werror
cmake --build "${MANYFORGE_STABILITY_TEST_BINARY%/test_execution_kernel}" --target test_execution_kernel --parallel "${MANYFORGE_BUILD_JOBS}"
/workspace/manyforge_core/tools/stability_gate.sh
python3 /workspace/scripts/qualification/source_identity.py --repo-root /workspace --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256}" >/dev/null'

CONTAINER_COMMAND=(taskset -c "${CPUSET}" bash -lc "${INNER_SCRIPT}")
OUTER_COMMAND=(docker compose --project-name "${COMPOSE_PROJECT_NAME_RESOLVED}"
  -f "${COMPOSE_FILE}" run --detach --no-deps -T
  --name "${CONTAINER_NAME}")
for entry in "${CONTAINER_ENVIRONMENT_ARGS[@]}"; do
  OUTER_COMMAND+=(-e "${entry}")
done
for entry in "${CONTAINER_LABEL_ARGS[@]}"; do
  OUTER_COMMAND+=(--label "${entry}")
done
OUTER_COMMAND+=(dev "${CONTAINER_COMMAND[@]}")

container_created=0
cleanup_container() {
  if [[ "${container_created}" -eq 1 ]]; then
    docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  fi
}
trap cleanup_container EXIT INT TERM

"${OUTER_COMMAND[@]}" >/dev/null
container_created=1
CONTAINER_ID="$(docker inspect "${CONTAINER_NAME}" --format '{{.Id}}')"
if [[ ! "${CONTAINER_ID}" =~ ^[0-9a-f]{64}$ ]]; then
  echo "unable to resolve exact qualification container id" >&2
  exit 1
fi

set +e
CONTAINER_STATUS="$(docker wait "${CONTAINER_ID}")"
wait_status=$?
set -e
docker logs "${CONTAINER_ID}" >"${CONTAINER_LOG}" 2>&1 || true
docker inspect "${CONTAINER_ID}" >"${CONTAINER_INSPECT}"
if [[ "${wait_status}" -ne 0 || "${CONTAINER_STATUS}" != 0 ]]; then
  echo "loaded-kernel container failed: wait=${wait_status} exit=${CONTAINER_STATUS}" >&2
  tail -n 200 "${CONTAINER_LOG}" >&2 || true
  exit 1
fi
if [[ ! -s "${RAW_MANIFEST}" || ! -s "${CONTAINER_LOG}" || ! -s "${CONTAINER_INSPECT}" ]]; then
  echo "loaded-kernel container omitted a required retained artifact" >&2
  exit 1
fi

OBSERVED_IMAGE_ID="$(python3 -c '
import json, sys
value=json.load(open(sys.argv[1], encoding="utf-8"))
print(value[0]["Image"].removeprefix("sha256:"))
' "${CONTAINER_INSPECT}")"
if [[ "${OBSERVED_IMAGE_ID}" != "${IMAGE_ID}" ]]; then
  echo "qualification container image differs from the predeclared image" >&2
  exit 1
fi

MANYFORGE_AFTER="$(identity_json "${REPO_ROOT}")"
SPECS_BUNDLE_AFTER="$(specs_identity_bundle_json "${SPECS_ROOT}")"
if [[ "${MANYFORGE_AFTER}" != "${MANYFORGE_BEFORE}" ||
      "${SPECS_BUNDLE_AFTER}" != "${SPECS_BUNDLE_BEFORE}" ]]; then
  echo "source or Specs identity changed during loaded-kernel qualification" >&2
  exit 1
fi

argv_json() {
  python3 -c 'import json,sys; print(json.dumps(sys.argv[1:], separators=(",", ":")))' "$@"
}
OUTER_COMMAND_JSON="$(argv_json "${OUTER_COMMAND[@]}")"
CONTAINER_COMMAND_JSON="$(argv_json "${CONTAINER_COMMAND[@]}")"

python3 - "${RUN_MANIFEST}" "${REPO_ROOT}" "${SPECS_ROOT}" \
  "${RAW_MANIFEST}" "${CONTAINER_INSPECT}" "${CONTAINER_LOG}" \
  "${RUN_ID}" "${PROCESS_INSTANCE_ID}" "${MANYFORGE_BEFORE}" \
  "${MANYFORGE_AFTER}" "${SPECS_BUNDLE_BEFORE}" "${SPECS_BUNDLE_AFTER}" \
  "${CPUSET}" "${HOST_KERNEL_ID}" "${CPU_GOVERNOR_SET}" \
  "${CPU_FREQUENCY_DRIVER_SET}" "${CONTAINER_ID}" "${CONTAINER_NAME}" \
  "${IMAGE_ID}" "${IMAGE_REFERENCE}" "${COMPOSE_PROJECT_NAME_RESOLVED}" \
  "${OUTER_COMMAND_JSON}" "${CONTAINER_COMMAND_JSON}" \
  "${CONTAINER_ENVIRONMENT_JSON}" "${CONTAINER_LABELS_JSON}" \
  "${EXPECTED_MOUNTS_JSON}" "${BASH_SOURCE[0]}" "${SOURCE_HELPER}" \
  "${VERIFIER}" "${GATE_SCRIPT}" "${COMPOSE_FILE}" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

(
    output_raw, repo_raw, specs_raw, raw_manifest_raw, inspect_raw, log_raw,
    run_id, process_id, manyforge_before_raw, manyforge_after_raw,
    specs_bundle_before_raw, specs_bundle_after_raw, cpuset, host_kernel,
    governors, drivers, container_id, container_name, image_id,
    image_reference, compose_project_name, outer_command_raw,
    container_command_raw, environment_raw, labels_raw, mounts_raw,
    runner_raw, helper_raw, verifier_raw, gate_raw, compose_raw,
) = sys.argv[1:]

def sha256(path_raw: str) -> str:
    return hashlib.sha256(Path(path_raw).read_bytes()).hexdigest()

specs_before = json.loads(specs_bundle_before_raw)
specs_after = json.loads(specs_bundle_after_raw)
manifest = {
    "schema_version": "manyforge.step18.loaded_kernel_run.v1",
    "run_id": run_id,
    "process_instance_id": process_id,
    "paths": {
        "manyforge_root": str(Path(repo_raw).resolve()),
        "specs_root": str(Path(specs_raw).resolve()),
        "raw_manifest": str(Path(raw_manifest_raw).resolve()),
        "container_inspect": str(Path(inspect_raw).resolve()),
        "container_log": str(Path(log_raw).resolve()),
    },
    "identities": {
        "manyforge_before": json.loads(manyforge_before_raw),
        "manyforge_after": json.loads(manyforge_after_raw),
        "specs_before": specs_before["source_identity"],
        "specs_after": specs_after["source_identity"],
        "specs_contract_before": specs_before["specs_contract_identity"],
        "specs_contract_after": specs_after["specs_contract_identity"],
    },
    "execution": {
        "requested_cpu_affinity": cpuset,
        "host_kernel_id": host_kernel,
        "cpu_governor_set": governors,
        "cpu_frequency_driver_set": drivers,
        "compose_service": "dev",
        "container_id": container_id,
        "container_name": container_name,
        "image_id": image_id,
        "image_reference": image_reference,
        "compose_project_name": compose_project_name,
        "container_labels": json.loads(labels_raw),
        "expected_mounts": json.loads(mounts_raw),
        "outer_command_argv": json.loads(outer_command_raw),
        "container_command_argv": json.loads(container_command_raw),
        "container_environment": json.loads(environment_raw),
    },
    "hashes": {
        "runner_script_sha256": sha256(runner_raw),
        "source_identity_helper_sha256": sha256(helper_raw),
        "verifier_script_sha256": sha256(verifier_raw),
        "gate_script_sha256": sha256(gate_raw),
        "compose_file_sha256": sha256(compose_raw),
        "raw_manifest_sha256": sha256(raw_manifest_raw),
        "container_inspect_sha256": sha256(inspect_raw),
        "container_log_sha256": sha256(log_raw),
    },
}
Path(output_raw).write_text(
    json.dumps(manifest, indent=2, sort_keys=True, allow_nan=False) + "\n",
    encoding="utf-8",
)
PY

python3 "${VERIFIER}" --manifest "${RUN_MANIFEST}" --output "${VERIFICATION_SUMMARY}"
docker rm "${CONTAINER_ID}" >/dev/null
container_created=0
trap - EXIT INT TERM

echo "Step 18 loaded-kernel qualification passed"
echo "run manifest: ${RUN_MANIFEST}"
echo "verification summary: ${VERIFICATION_SUMMARY}"
