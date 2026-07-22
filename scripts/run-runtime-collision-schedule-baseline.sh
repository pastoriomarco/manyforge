#!/usr/bin/env bash
# Run one typed record from a prepared detached 2bfe30e runtime baseline lane.

set -euo pipefail

ORIGINAL_RUNNER_ARGV=("$0" "$@")
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LANE_DIR="${MANYFORGE_BASELINE_LANE_DIR:-${REPO_ROOT}/build/step18-runtime-baseline-lane}"
SOURCE_IDENTITY_CLI="${REPO_ROOT}/scripts/qualification/source_identity.py"
ARTIFACT_WRAPPER="${REPO_ROOT}/scripts/qualification/wrap_runtime_collision_schedule_artifact.py"
MEASUREMENT_KIND=""
PURPOSE="formal"
FORWARD_ARGS=()

usage() {
  cat >&2 <<'USAGE'
usage: run-runtime-collision-schedule-baseline.sh \
  --measurement-kind {timing,observation} \
  [--purpose {formal,parity-probe}] [qualification options]

Formal observation runs require MANYFORGE_BASELINE_PARITY_ARTIFACT. Set
MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT to archive the emitted typed record.
USAGE
}

while (($# > 0)); do
  case "$1" in
    --measurement-kind)
      [[ $# -ge 2 ]] || { echo "--measurement-kind requires a value" >&2; exit 2; }
      MEASUREMENT_KIND="$2"; shift 2 ;;
    --purpose)
      [[ $# -ge 2 ]] || { echo "--purpose requires a value" >&2; exit 2; }
      PURPOSE="$2"; shift 2 ;;
    --help|-h)
      usage; exit 0 ;;
    --arm)
      echo "the baseline runner fixes --arm current_all_active_distance" >&2; exit 2 ;;
    *)
      FORWARD_ARGS+=("$1"); shift ;;
  esac
done
[[ "${MEASUREMENT_KIND}" == "timing" || "${MEASUREMENT_KIND}" == "observation" ]] || {
  echo "--measurement-kind must be timing or observation" >&2; exit 2;
}
[[ "${PURPOSE}" == "formal" || "${PURPOSE}" == "parity-probe" ]] || {
  echo "--purpose must be formal or parity-probe" >&2; exit 2;
}

json_argv() {
  python3 -c 'import json,sys; print(json.dumps(sys.argv[1:], separators=(",", ":")))' "$@"
}

LANE_DIR="$(realpath -m "${LANE_DIR}")"
case "${LANE_DIR}" in
  "${REPO_ROOT}/build/"*) ;;
  *) echo "baseline lane must be below ${REPO_ROOT}/build" >&2; exit 2 ;;
esac
ARTIFACT_DIR="${LANE_DIR}/artifacts"
METADATA="${ARTIFACT_DIR}/lane-metadata.json"
CLEAN_MANIFEST="${ARTIFACT_DIR}/clean-checkout-manifest.json"
HARNESS_PATCH="${ARTIFACT_DIR}/baseline-harness.patch"
OBSERVATION_PATCH="${REPO_ROOT}/scripts/qualification/baseline/runtime_collision_schedule_observation.patch"
HARNESS_SOURCE="${REPO_ROOT}/tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp"
CMAKE_PATCH="${REPO_ROOT}/scripts/qualification/baseline/runtime_collision_schedule_cmake.patch"
BASELINE_COMPAT_PATCH="${REPO_ROOT}/scripts/qualification/baseline/runtime_collision_schedule_baseline_compat.patch"
for required_file in "${METADATA}" "${CLEAN_MANIFEST}" "${HARNESS_PATCH}" \
  "${OBSERVATION_PATCH}" "${HARNESS_SOURCE}" "${CMAKE_PATCH}" \
  "${BASELINE_COMPAT_PATCH}" "${SOURCE_IDENTITY_CLI}" "${ARTIFACT_WRAPPER}"; do
  [[ -f "${required_file}" ]] || { echo "prepared baseline input missing: ${required_file}" >&2; exit 1; }
done
METADATA_SHA256="$(sha256sum "${METADATA}" | awk '{print $1}')"
RUNNER_SCRIPT_SHA256="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"
WRAPPER_SCRIPT_SHA256="$(sha256sum "${ARTIFACT_WRAPPER}" | awk '{print $1}')"

mapfile -t META < <(python3 - "${METADATA}" <<'PY'
import json, sys
from pathlib import Path
value = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if value.get("schema_version") != "manyforge.runtime_collision_baseline_lane.v2":
    raise SystemExit("invalid baseline lane metadata schema")
for key in (
    "timing_source_identity", "observation_source_identity", "specs_source_identity",
    "specs_contract_identity",
    "image_id", "requested_cpu_affinity", "cpu_governor_set",
    "cpu_frequency_driver_set", "kernel_id", "compiler_flags", "build_type",
    "clean_checkout_manifest_sha256", "qualification_patch_sha256",
    "observation_patch_sha256", "timing_binary_sha256", "observation_binary_sha256",
    "build_commands_argv", "prepare_script_sha256", "source_identity_script_sha256",
    "harness_source_sha256", "cmake_patch_sha256", "baseline_compat_patch_sha256",
):
    item = value[key]
    print(json.dumps(item, sort_keys=True, separators=(",", ":")) if isinstance(item, (dict, list)) else item)
PY
)
[[ ${#META[@]} -eq 22 ]] || { echo "baseline lane metadata is incomplete" >&2; exit 1; }
TIMING_SOURCE_IDENTITY="${META[0]}"
OBSERVATION_SOURCE_IDENTITY="${META[1]}"
SPECS_SOURCE_IDENTITY="${META[2]}"
SPECS_CONTRACT_IDENTITY="${META[3]}"
IMAGE_ID="${META[4]}"
CPUSET="${META[5]}"
EXPECTED_GOVERNORS="${META[6]}"
EXPECTED_DRIVERS="${META[7]}"
EXPECTED_KERNEL="${META[8]}"
COMPILER_FLAGS="${META[9]}"
BUILD_TYPE="${META[10]}"
CLEAN_MANIFEST_SHA256="${META[11]}"
HARNESS_PATCH_SHA256="${META[12]}"
OBSERVATION_PATCH_SHA256="${META[13]}"
TIMING_BINARY_SHA256="${META[14]}"
OBSERVATION_BINARY_SHA256="${META[15]}"
BUILD_COMMANDS_JSON="${META[16]}"
PREPARE_SCRIPT_SHA256="${META[17]}"
SOURCE_IDENTITY_SCRIPT_SHA256="${META[18]}"
HARNESS_SOURCE_SHA256="${META[19]}"
CMAKE_PATCH_SHA256="${META[20]}"
BASELINE_COMPAT_PATCH_SHA256="${META[21]}"

if [[ -n "${MANYFORGE_RUNTIME_SCHEDULE_CPUSET:-}" &&
      "${MANYFORGE_RUNTIME_SCHEDULE_CPUSET}" != "${CPUSET}" ]]; then
  echo "run CPU affinity differs from the prepared lane" >&2
  exit 2
fi
mapfile -t CPU_ENV < <(python3 - "${CPUSET}" <<'PY'
import sys
from pathlib import Path
cpus = set()
for part in sys.argv[1].split(","):
    if "-" in part:
        first, last = (int(value) for value in part.split("-", 1))
        cpus.update(range(first, last + 1))
    else:
        cpus.add(int(part))
if len(cpus) != 4:
    raise SystemExit("prepared formal CPU affinity must contain exactly four CPUs")
governors = set()
drivers = set()
for cpu in sorted(cpus):
    root = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    for name, values in (("scaling_governor", governors), ("scaling_driver", drivers)):
        path = root / name
        if not path.is_file():
            raise SystemExit(f"missing CPU frequency provenance: {path}")
        values.add(path.read_text(encoding="utf-8").strip())
print(",".join(sorted(governors)))
print(",".join(sorted(drivers)))
PY
)
[[ ${#CPU_ENV[@]} -eq 2 && "${CPU_ENV[0]}" == "${EXPECTED_GOVERNORS}" &&
   "${CPU_ENV[1]}" == "${EXPECTED_DRIVERS}" && "$(uname -srvm)" == "${EXPECTED_KERNEL}" ]] || {
  echo "baseline host governor/driver/kernel differs from preparation" >&2; exit 1;
}

IMAGE_REFERENCE="$(docker compose -f "${REPO_ROOT}/docker-compose.yaml" images -q dev 2>/dev/null)"
[[ -n "${IMAGE_REFERENCE}" && "${IMAGE_REFERENCE}" != *$'\n'* ]] || {
  echo "Compose dev service does not resolve to exactly one image" >&2; exit 1;
}
ACTUAL_IMAGE_ID="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
ACTUAL_IMAGE_ID="${ACTUAL_IMAGE_ID#sha256:}"
[[ "${ACTUAL_IMAGE_ID}" =~ ^[0-9a-f]{64}$ && "${ACTUAL_IMAGE_ID}" == "${IMAGE_ID}" ]] || {
  echo "actual Compose dev image differs from the prepared baseline image" >&2; exit 1;
}
if [[ -n "${MANYFORGE_QUAL_IMAGE_ID:-}" ]]; then
  EXPECTED_IMAGE_ID="${MANYFORGE_QUAL_IMAGE_ID#sha256:}"
  [[ "${EXPECTED_IMAGE_ID}" == "${ACTUAL_IMAGE_ID}" ]] || {
    echo "MANYFORGE_QUAL_IMAGE_ID differs from the actual image" >&2; exit 2;
  }
fi

for path_and_hash in \
  "${CLEAN_MANIFEST}:${CLEAN_MANIFEST_SHA256}" \
  "${HARNESS_PATCH}:${HARNESS_PATCH_SHA256}" \
  "${OBSERVATION_PATCH}:${OBSERVATION_PATCH_SHA256}"; do
  path="${path_and_hash%:*}"
  expected="${path_and_hash##*:}"
  [[ "$(sha256sum "${path}" | awk '{print $1}')" == "${expected}" ]] || {
    echo "baseline provenance hash changed: ${path}" >&2; exit 1;
  }
done
[[ "$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')" == "${SOURCE_IDENTITY_SCRIPT_SHA256}" ]] || {
  echo "source identity helper differs from prepared lane" >&2; exit 1;
}
for path_and_hash in \
  "${HARNESS_SOURCE}:${HARNESS_SOURCE_SHA256}" \
  "${CMAKE_PATCH}:${CMAKE_PATCH_SHA256}" \
  "${BASELINE_COMPAT_PATCH}:${BASELINE_COMPAT_PATCH_SHA256}"; do
  path="${path_and_hash%:*}"
  expected="${path_and_hash##*:}"
  [[ "$(sha256sum "${path}" | awk '{print $1}')" == "${expected}" ]] || {
    echo "baseline preparation input changed: ${path}" >&2; exit 1;
  }
done

SPECS_SOURCE="${LANE_DIR}/specs-source"
PARITY_ARTIFACT=""
if [[ "${MEASUREMENT_KIND}" == "timing" ]]; then
  SOURCE_TREE="${LANE_DIR}/timing-source"
  SOURCE_IDENTITY="${TIMING_SOURCE_IDENTITY}"
  BINARY="${LANE_DIR}/timing-build/manyforge_integration_tests/perf_runtime_collision_schedule_baseline_timing"
  BINARY_SHA256="${TIMING_BINARY_SHA256}"
  SOURCE_ROLE="pristine_baseline_timing"
  OBSERVATION_HASH=""
  PARITY_HASH=""
else
  SOURCE_TREE="${LANE_DIR}/observation-source"
  SOURCE_IDENTITY="${OBSERVATION_SOURCE_IDENTITY}"
  BINARY="${LANE_DIR}/observation-build/manyforge_integration_tests/perf_runtime_collision_schedule_baseline_observation"
  BINARY_SHA256="${OBSERVATION_BINARY_SHA256}"
  SOURCE_ROLE="pristine_baseline_observation"
  OBSERVATION_HASH="${OBSERVATION_PATCH_SHA256}"
  PARITY_ARTIFACT="${MANYFORGE_BASELINE_PARITY_ARTIFACT:-}"
  if [[ "${PURPOSE}" == "formal" && -z "${PARITY_ARTIFACT}" ]]; then
    echo "formal observation requires MANYFORGE_BASELINE_PARITY_ARTIFACT" >&2; exit 2
  fi
  if [[ -n "${PARITY_ARTIFACT}" ]]; then
    [[ -f "${PARITY_ARTIFACT}" ]] || { echo "parity artifact missing" >&2; exit 1; }
    PARITY_HASH="$(sha256sum "${PARITY_ARTIFACT}" | awk '{print $1}')"
  else
    PARITY_HASH=""
  fi
fi

validate_archive_destination() {
  python3 - "$1" "${REPO_ROOT}" "${SOURCE_TREE}" "${SPECS_SOURCE}" <<'PY'
import subprocess
import sys
from pathlib import Path

destination = Path(sys.argv[1]).expanduser().resolve(strict=False)
repository, source, specs = (Path(value).resolve() for value in sys.argv[2:])

def within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents

if destination.exists():
    raise SystemExit(f"refusing to overwrite runtime artifact: {destination}")
if within(destination, source) or within(destination, specs):
    raise SystemExit("runtime artifact output must not be inside a detached checkout")
if within(destination, repository):
    allowed = (
        repository / "qualification",
        repository / ".manyforge-var" / "qualification",
        repository / "build",
    )
    if not any(within(destination, root) for root in allowed):
        raise SystemExit(
            "runtime artifact output inside ManyForge must be below an ignored "
            "qualification or build evidence root"
        )
    relative = destination.relative_to(repository)
    if subprocess.run(
        ["git", "-C", str(repository), "check-ignore", "-q", "--", str(relative)],
        check=False,
    ).returncode != 0:
        raise SystemExit(
            "runtime artifact output inside ManyForge must be actually ignored by git"
        )
print(destination)
PY
}

ARCHIVE_OUTPUT=""
if [[ -n "${MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT:-}" ]]; then
  ARCHIVE_OUTPUT="$(
    validate_archive_destination "${MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT}"
  )"
fi

SPECS_IDENTITY_BUNDLE="$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_SOURCE}" --include-specs-contract-identity)"
OBSERVED_SPECS_SOURCE_IDENTITY="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["source_identity"], sort_keys=True, separators=(",", ":")))' "${SPECS_IDENTITY_BUNDLE}")"
OBSERVED_SPECS_CONTRACT_IDENTITY="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])["specs_contract_identity"], sort_keys=True, separators=(",", ":")))' "${SPECS_IDENTITY_BUNDLE}")"
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SOURCE_TREE}")" == "${SOURCE_IDENTITY}" &&
   "${OBSERVED_SPECS_SOURCE_IDENTITY}" == "${SPECS_SOURCE_IDENTITY}" &&
   "${OBSERVED_SPECS_CONTRACT_IDENTITY}" == "${SPECS_CONTRACT_IDENTITY}" ]] || {
  echo "prepared baseline source identity changed" >&2; exit 1;
}
[[ -x "${BINARY}" && "$(sha256sum "${BINARY}" | awk '{print $1}')" == "${BINARY_SHA256}" ]] || {
  echo "prepared baseline binary identity changed" >&2; exit 1;
}

mapfile -t SOURCE_FIELDS < <(python3 -c \
  'import json,sys; v=json.loads(sys.argv[1]); print(v["head_commit"]); print(v["head_tree"]); print(int(v["dirty"])); print(v["delta_sha256"]); print(v["fingerprint_sha256"])' "${SOURCE_IDENTITY}")
mapfile -t SPECS_FIELDS < <(python3 -c \
  'import json,sys; v=json.loads(sys.argv[1]); print(v["head_commit"]); print(v["head_tree"]); print(int(v["dirty"])); print(v["delta_sha256"]); print(v["fingerprint_sha256"])' "${SPECS_SOURCE_IDENTITY}")
mapfile -t SPECS_CONTRACT_FIELDS < <(python3 -c \
  'import json,sys; v=json.loads(sys.argv[1]); print(int(v["contract_dirty"])); print(v["contract_delta_sha256"]); print(v["contract_fingerprint_sha256"])' "${SPECS_CONTRACT_IDENTITY}")

PROCESS_INSTANCE_ID="$(python3 -c 'import uuid; print(uuid.uuid4())')"
RUN_ID="${MANYFORGE_QUAL_RUN_ID:-$(date -u +%Y%m%dT%H%M%S%NZ)-baseline-${MEASUREMENT_KIND}-${PROCESS_INSTANCE_ID}}"
CONTAINER_BINARY="/workspace/${BINARY#"${REPO_ROOT}/"}"
RUNNER_COMMAND_JSON="$(json_argv "${ORIGINAL_RUNNER_ARGV[@]}")"
RUNNER_ENVIRONMENT_JSON="$(python3 - \
  "${CPUSET}" "${IMAGE_ID}" "${COMPILER_FLAGS}" "${BUILD_TYPE}" \
  "${EXPECTED_GOVERNORS}" "${EXPECTED_DRIVERS}" "${EXPECTED_KERNEL}" \
  "1" "${PURPOSE}" "${SOURCE_FIELDS[4]}" "${SPECS_FIELDS[4]}" \
  "${SPECS_CONTRACT_FIELDS[2]}" "${SOURCE_TREE}" "${SPECS_SOURCE}" \
  "${METADATA_SHA256}" "${HARNESS_SOURCE_SHA256}" "${CMAKE_PATCH_SHA256}" \
  "${BASELINE_COMPAT_PATCH_SHA256}" <<'PY'
import json, sys
keys = (
    "requested_cpu_affinity", "image_id", "compiler_flags", "build_type",
    "cpu_governor_set", "cpu_frequency_driver_set", "kernel_id", "build_jobs",
    "purpose",
    "source_fingerprint_sha256", "specs_fingerprint_sha256",
    "specs_contract_fingerprint_sha256", "source_root", "specs_root",
    "lane_metadata_sha256",
    "harness_source_sha256", "cmake_patch_sha256",
    "baseline_compat_patch_sha256",
)
print(json.dumps(dict(zip(keys, sys.argv[1:])), sort_keys=True, separators=(",", ":")))
PY
)"

COMMAND=(docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T
  -e "MANYFORGE_QUAL_RUN_ID=${RUN_ID}"
  -e "MANYFORGE_QUAL_PROCESS_INSTANCE_ID=${PROCESS_INSTANCE_ID}"
  -e "MANYFORGE_SOURCE_HEAD_COMMIT=${SOURCE_FIELDS[0]}"
  -e "MANYFORGE_SOURCE_HEAD_TREE=${SOURCE_FIELDS[1]}"
  -e "MANYFORGE_SOURCE_DIRTY=${SOURCE_FIELDS[2]}"
  -e "MANYFORGE_SOURCE_DELTA_SHA256=${SOURCE_FIELDS[3]}"
  -e "MANYFORGE_SOURCE_FINGERPRINT_SHA256=${SOURCE_FIELDS[4]}"
  -e "MANYFORGE_SPECS_HEAD_COMMIT=${SPECS_FIELDS[0]}"
  -e "MANYFORGE_SPECS_HEAD_TREE=${SPECS_FIELDS[1]}"
  -e "MANYFORGE_SPECS_DIRTY=${SPECS_FIELDS[2]}"
  -e "MANYFORGE_SPECS_DELTA_SHA256=${SPECS_FIELDS[3]}"
  -e "MANYFORGE_SPECS_FINGERPRINT_SHA256=${SPECS_FIELDS[4]}"
  -e "MANYFORGE_SPECS_CONTRACT_DIRTY=${SPECS_CONTRACT_FIELDS[0]}"
  -e "MANYFORGE_SPECS_CONTRACT_DELTA_SHA256=${SPECS_CONTRACT_FIELDS[1]}"
  -e "MANYFORGE_SPECS_CONTRACT_FINGERPRINT_SHA256=${SPECS_CONTRACT_FIELDS[2]}"
  -e "MANYFORGE_QUAL_IMAGE_ID=${IMAGE_ID}"
  -e "MANYFORGE_QUAL_BINARY_SHA256=${BINARY_SHA256}"
  -e "MANYFORGE_RUNTIME_SCHEDULE_CPUSET=${CPUSET}"
  -e "MANYFORGE_CPU_GOVERNOR_SET=${EXPECTED_GOVERNORS}"
  -e "MANYFORGE_CPU_FREQUENCY_DRIVER_SET=${EXPECTED_DRIVERS}"
  -e "MANYFORGE_HOST_KERNEL_ID=${EXPECTED_KERNEL}"
  -e "MANYFORGE_MATCHED_COMPILER_FLAGS=${COMPILER_FLAGS}"
  -e "MANYFORGE_MATCHED_BUILD_TYPE=${BUILD_TYPE}"
  -e "MANYFORGE_SOURCE_ROLE=${SOURCE_ROLE}"
  -e "MANYFORGE_CHECKOUT_BASE_CLEAN=1"
  -e "MANYFORGE_CLEAN_CHECKOUT_MANIFEST_SHA256=${CLEAN_MANIFEST_SHA256}"
  -e "MANYFORGE_QUALIFICATION_PATCH_SHA256=${HARNESS_PATCH_SHA256}"
  -e "MANYFORGE_OBSERVATION_PATCH_SHA256=${OBSERVATION_HASH}"
  -e "MANYFORGE_PRISTINE_DECISION_PARITY_SHA256=${PARITY_HASH}"
  -e "MANYFORGE_QUAL_RUNNER_ENVIRONMENT_JSON=${RUNNER_ENVIRONMENT_JSON}"
  -e "MANYFORGE_QUAL_RUNNER_SCRIPT_SHA256=${RUNNER_SCRIPT_SHA256}"
  -e "MANYFORGE_QUAL_PREPARE_SCRIPT_SHA256=${PREPARE_SCRIPT_SHA256}"
  -e "MANYFORGE_SOURCE_IDENTITY_SCRIPT_SHA256=${SOURCE_IDENTITY_SCRIPT_SHA256}"
  dev bash -lc
  'set -euo pipefail
   export CMAKE_PREFIX_PATH="/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix:${CMAKE_PREFIX_PATH:-}"
   export LD_LIBRARY_PATH="/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix/lib:${LD_LIBRARY_PATH:-}"
   exec taskset -c "$1" "$2" "${@:3}"'
  baseline-runtime "${CPUSET}" "${CONTAINER_BINARY}"
  --measurement-kind "${MEASUREMENT_KIND}"
  --arm current_all_active_distance
  "${FORWARD_ARGS[@]}")
OUTER_COMMAND_JSON="$(json_argv "${COMMAND[@]}")"

RAW_OUTPUT="$(mktemp)"
WRAPPED_OUTPUT="$(mktemp)"
STAGED_ARCHIVE=""
trap 'rm -f "${RAW_OUTPUT}" "${WRAPPED_OUTPUT}" "${STAGED_ARCHIVE:-}" || true' EXIT
cd "${REPO_ROOT}"
"${COMMAND[@]}" >"${RAW_OUTPUT}"
IMAGE_ID_AFTER_MEASUREMENT="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
IMAGE_ID_AFTER_MEASUREMENT="${IMAGE_ID_AFTER_MEASUREMENT#sha256:}"
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SOURCE_TREE}")" == "${SOURCE_IDENTITY}" &&
   "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_SOURCE}" --include-specs-contract-identity)" == "${SPECS_IDENTITY_BUNDLE}" &&
   "${IMAGE_ID_AFTER_MEASUREMENT}" == "${IMAGE_ID}" &&
   "$(uname -srvm)" == "${EXPECTED_KERNEL}" ]] || {
  echo "baseline source, image, or kernel identity changed during measurement" >&2; exit 1;
}

python3 "${ARTIFACT_WRAPPER}" --input "${RAW_OUTPUT}" \
  --process-instance-id "${PROCESS_INSTANCE_ID}" \
  --source-identity-json "${SOURCE_IDENTITY}" \
  --specs-source-identity-json "${SPECS_SOURCE_IDENTITY}" \
  --specs-contract-identity-json "${SPECS_CONTRACT_IDENTITY}" \
  --runner-command-json "${RUNNER_COMMAND_JSON}" \
  --build-commands-json "${BUILD_COMMANDS_JSON}" \
  --outer-command-json "${OUTER_COMMAND_JSON}" \
  --runner-environment-json "${RUNNER_ENVIRONMENT_JSON}" \
  --runner-script-sha256 "${RUNNER_SCRIPT_SHA256}" \
  --source-identity-script-sha256 "${SOURCE_IDENTITY_SCRIPT_SHA256}" \
  --prepare-script-sha256 "${PREPARE_SCRIPT_SHA256}" \
  >"${WRAPPED_OUTPUT}"

IMAGE_ID_AFTER_WRAPPING="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
IMAGE_ID_AFTER_WRAPPING="${IMAGE_ID_AFTER_WRAPPING#sha256:}"
if [[ -n "${PARITY_HASH}" &&
      "$(sha256sum "${PARITY_ARTIFACT}" | awk '{print $1}')" != "${PARITY_HASH}" ]]; then
  echo "baseline parity artifact changed while wrapping" >&2
  exit 1
fi
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SOURCE_TREE}")" == "${SOURCE_IDENTITY}" &&
   "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_SOURCE}" --include-specs-contract-identity)" == "${SPECS_IDENTITY_BUNDLE}" &&
   "${IMAGE_ID_AFTER_WRAPPING}" == "${IMAGE_ID}" &&
   "$(uname -srvm)" == "${EXPECTED_KERNEL}" &&
   "$(sha256sum "${BINARY}" | awk '{print $1}')" == "${BINARY_SHA256}" &&
   "$(sha256sum "${METADATA}" | awk '{print $1}')" == "${METADATA_SHA256}" &&
   "$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')" == "${CLEAN_MANIFEST_SHA256}" &&
   "$(sha256sum "${HARNESS_PATCH}" | awk '{print $1}')" == "${HARNESS_PATCH_SHA256}" &&
   "$(sha256sum "${OBSERVATION_PATCH}" | awk '{print $1}')" == "${OBSERVATION_PATCH_SHA256}" &&
   "$(sha256sum "${HARNESS_SOURCE}" | awk '{print $1}')" == "${HARNESS_SOURCE_SHA256}" &&
   "$(sha256sum "${CMAKE_PATCH}" | awk '{print $1}')" == "${CMAKE_PATCH_SHA256}" &&
   "$(sha256sum "${BASELINE_COMPAT_PATCH}" | awk '{print $1}')" == "${BASELINE_COMPAT_PATCH_SHA256}" &&
   "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" == "${RUNNER_SCRIPT_SHA256}" &&
   "$(sha256sum "${ARTIFACT_WRAPPER}" | awk '{print $1}')" == "${WRAPPER_SCRIPT_SHA256}" &&
   "$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')" == "${SOURCE_IDENTITY_SCRIPT_SHA256}" ]] || {
  echo "baseline source, image, or kernel identity changed while wrapping artifact" >&2; exit 1;
}

if [[ -n "${ARCHIVE_OUTPUT}" ]]; then
  mkdir -p "$(dirname "${ARCHIVE_OUTPUT}")"
  STAGED_ARCHIVE="$(
    mktemp "$(dirname "${ARCHIVE_OUTPUT}")/.$(basename "${ARCHIVE_OUTPUT}").pending.XXXXXX"
  )"
  python3 - "${WRAPPED_OUTPUT}" "${STAGED_ARCHIVE}" <<'PY'
import os
import sys
from pathlib import Path

payload = Path(sys.argv[1]).read_bytes()
with Path(sys.argv[2]).open("wb") as stream:
    stream.write(payload)
    stream.flush()
    os.fsync(stream.fileno())
PY
fi
cat "${WRAPPED_OUTPUT}"
if [[ -n "${ARCHIVE_OUTPUT}" ]]; then
  [[ "$(validate_archive_destination "${ARCHIVE_OUTPUT}")" == "${ARCHIVE_OUTPUT}" ]] || {
    echo "runtime artifact destination changed before publication" >&2
    exit 1
  }
fi
if [[ -n "${PARITY_HASH}" &&
      "$(sha256sum "${PARITY_ARTIFACT}" | awk '{print $1}')" != "${PARITY_HASH}" ]]; then
  echo "baseline parity artifact changed before archival completed" >&2
  exit 1
fi
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SOURCE_TREE}")" == "${SOURCE_IDENTITY}" &&
   "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_SOURCE}" --include-specs-contract-identity)" == "${SPECS_IDENTITY_BUNDLE}" &&
   "$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null | sed 's/^sha256://')" == "${IMAGE_ID}" &&
   "$(uname -srvm)" == "${EXPECTED_KERNEL}" &&
   "$(sha256sum "${BINARY}" | awk '{print $1}')" == "${BINARY_SHA256}" &&
   "$(sha256sum "${METADATA}" | awk '{print $1}')" == "${METADATA_SHA256}" &&
   "$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')" == "${CLEAN_MANIFEST_SHA256}" &&
   "$(sha256sum "${HARNESS_PATCH}" | awk '{print $1}')" == "${HARNESS_PATCH_SHA256}" &&
   "$(sha256sum "${OBSERVATION_PATCH}" | awk '{print $1}')" == "${OBSERVATION_PATCH_SHA256}" &&
   "$(sha256sum "${HARNESS_SOURCE}" | awk '{print $1}')" == "${HARNESS_SOURCE_SHA256}" &&
   "$(sha256sum "${CMAKE_PATCH}" | awk '{print $1}')" == "${CMAKE_PATCH_SHA256}" &&
   "$(sha256sum "${BASELINE_COMPAT_PATCH}" | awk '{print $1}')" == "${BASELINE_COMPAT_PATCH_SHA256}" &&
   "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" == "${RUNNER_SCRIPT_SHA256}" &&
   "$(sha256sum "${ARTIFACT_WRAPPER}" | awk '{print $1}')" == "${WRAPPER_SCRIPT_SHA256}" &&
   "$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')" == "${SOURCE_IDENTITY_SCRIPT_SHA256}" ]] || {
  echo "baseline source, Specs, binary, or runtime qualification input changed before archival completed" >&2
  exit 1
}
if [[ -n "${ARCHIVE_OUTPUT}" ]]; then
  python3 - "${WRAPPED_OUTPUT}" "${STAGED_ARCHIVE}" "${ARCHIVE_OUTPUT}" <<'PY'
import hashlib
import os
import sys
from pathlib import Path

source, staged, destination = (Path(value) for value in sys.argv[1:])
if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(
    staged.read_bytes()
).digest():
    raise SystemExit("staged runtime artifact changed before publication")
try:
    os.link(staged, destination)
except FileExistsError as error:
    raise SystemExit(f"refusing to overwrite runtime artifact: {destination}") from error
finally:
    try:
        staged.unlink(missing_ok=True)
    except OSError:
        pass
PY
  STAGED_ARCHIVE=""
fi
