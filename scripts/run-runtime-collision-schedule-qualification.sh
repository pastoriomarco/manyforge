#!/usr/bin/env bash
# Build and run one current-candidate Step 18 runtime-schedule process while
# binding the record to exact v1 source identities and host provenance.

set -euo pipefail

ORIGINAL_RUNNER_ARGV=("$0" "$@")
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_IDENTITY_CLI="${REPO_ROOT}/scripts/qualification/source_identity.py"
ARTIFACT_WRAPPER="${REPO_ROOT}/scripts/qualification/wrap_runtime_collision_schedule_artifact.py"
MATCHED_TOOLCHAIN_CXX_FLAGS="-Wno-error=maybe-uninitialized"
MATCHED_BUILD_TYPE="Release"
IN_CONTAINER=false
MEASUREMENT_KIND=""
FORWARD_ARGS=()

usage() {
  cat >&2 <<'USAGE'
usage: run-runtime-collision-schedule-qualification.sh \
  --measurement-kind {timing,observation} \
  --arm {signed_penetration_selective,binary_contact,bounded_penetration} \
  [qualification options]

The host entry point requires all of:
  MANYFORGE_RUNTIME_SCHEDULE_CPUSET (exactly four CPUs)
  MANYFORGE_SPECS_SOURCE_ROOT
  MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256
  MANYFORGE_EXPECTED_SPECS_FINGERPRINT_SHA256
  MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256

Set MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT to archive the typed record. The
historical arm must be run through the detached baseline lane.
USAGE
}

while (($# > 0)); do
  case "$1" in
    --in-container)
      IN_CONTAINER=true
      shift
      ;;
    --measurement-kind)
      [[ $# -ge 2 ]] || { echo "--measurement-kind requires a value" >&2; exit 2; }
      MEASUREMENT_KIND="$2"
      FORWARD_ARGS+=("$1" "$2")
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      FORWARD_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ "${MEASUREMENT_KIND}" != "timing" &&
      "${MEASUREMENT_KIND}" != "observation" ]]; then
  echo "--measurement-kind must be timing or observation" >&2
  usage
  exit 2
fi

source_build_environment() {
  if [[ -f /opt/ros/jazzy/setup.bash ]]; then
    # shellcheck disable=SC1091
    set +u
    source /opt/ros/jazzy/setup.bash
    set -u
  fi
  local cmeel_prefix="${MANYFORGE_CMEEL_PREFIX:-/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix}"
  if [[ -d "${cmeel_prefix}" ]]; then
    export CMAKE_PREFIX_PATH="${cmeel_prefix}:${CMAKE_PREFIX_PATH:-}"
    export LD_LIBRARY_PATH="${cmeel_prefix}/lib:${LD_LIBRARY_PATH:-}"
    export PKG_CONFIG_PATH="${cmeel_prefix}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  fi
}

json_argv() {
  python3 -c 'import json, sys; print(json.dumps(sys.argv[1:], separators=(",", ":")))' "$@"
}

build_commands_json() {
  python3 - "$1" "$2" "$3" "$4" "$5" <<'PY'
import json
import sys
workspace, build_dir, target, jobs, flags = sys.argv[1:]
commands = [
    [
        "cmake", "-S", f"{workspace}/manyforge_roboplan", "-B", build_dir,
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
        f"-DCMAKE_CXX_FLAGS={flags}",
    ],
    ["cmake", "--build", build_dir, "--target", target, "--parallel", jobs],
]
print(json.dumps(commands, separators=(",", ":")))
PY
}

run_in_container() {
  source_build_environment
  local workspace="${WORKSPACE:-/workspace}"
  local build_dir="${MANYFORGE_RUNTIME_SCHEDULE_BUILD_DIR:-/tmp/manyforge-build/step18-runtime-schedule}"
  local build_jobs="${MANYFORGE_BUILD_JOBS:-1}"
  local target="perf_runtime_collision_schedule_${MEASUREMENT_KIND}"
  local binary="${build_dir}/manyforge_integration_tests/${target}"
  local cpuset="${MANYFORGE_RUNTIME_SCHEDULE_CPUSET:-}"

  [[ "${build_jobs}" == "1" ]] || {
    echo "formal runtime qualification requires MANYFORGE_BUILD_JOBS=1" >&2; exit 2;
  }
  [[ "${cpuset}" =~ ^[0-9,-]+$ ]] || {
    echo "MANYFORGE_RUNTIME_SCHEDULE_CPUSET must be explicit" >&2; exit 2;
  }
  local configure=(cmake -S "${workspace}/manyforge_roboplan" -B "${build_dir}"
    "-DCMAKE_BUILD_TYPE=${MATCHED_BUILD_TYPE}" -DBUILD_TESTING=ON
    "-DCMAKE_CXX_FLAGS=${MATCHED_TOOLCHAIN_CXX_FLAGS}")
  local build=(cmake --build "${build_dir}" --target "${target}"
    --parallel "${build_jobs}")
  "${configure[@]}" >&2
  "${build[@]}" >&2
  python3 "${workspace}/scripts/qualification/source_identity.py" \
    --repo-root "${workspace}" \
    --expect-fingerprint-sha256 "${MANYFORGE_SOURCE_FINGERPRINT_SHA256}" \
    >/dev/null
  [[ -x "${binary}" ]] || {
    echo "qualification binary missing after build: ${binary}" >&2; exit 1;
  }
  local binary_sha256
  binary_sha256="$(sha256sum "${binary}" | awk '{print $1}')"
  local command=("${binary}" "${FORWARD_ARGS[@]}" --binary-sha256 "${binary_sha256}")
  exec taskset -c "${cpuset}" "${command[@]}"
}

if [[ "${IN_CONTAINER}" == "true" ]]; then
  run_in_container
fi

for required_file in "${SOURCE_IDENTITY_CLI}" "${ARTIFACT_WRAPPER}"; do
  [[ -f "${required_file}" ]] || { echo "qualification helper missing: ${required_file}" >&2; exit 1; }
done
command -v docker >/dev/null 2>&1 || { echo "docker not found" >&2; exit 1; }

for legacy_variable in \
  MANYFORGE_SOURCE_COMMIT MANYFORGE_SPECS_COMMIT \
  MANYFORGE_SPECS_CANDIDATE_SHA256 MANYFORGE_WORKING_TREE_CANDIDATE_SHA256; do
  if [[ -n "${!legacy_variable:-}" ]]; then
    echo "${legacy_variable} is obsolete; use v1 source fingerprints" >&2
    exit 2
  fi
done

SPECS_ROOT="${MANYFORGE_SPECS_SOURCE_ROOT:-}"
EXPECTED_SOURCE_FINGERPRINT="${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256:-}"
EXPECTED_SPECS_FINGERPRINT="${MANYFORGE_EXPECTED_SPECS_FINGERPRINT_SHA256:-}"
EXPECTED_SPECS_CONTRACT_FINGERPRINT="${MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256:-}"
if [[ -z "${SPECS_ROOT}" || ! -d "${SPECS_ROOT}" ||
      ! "${EXPECTED_SOURCE_FINGERPRINT}" =~ ^[0-9a-f]{64}$ ||
      ! "${EXPECTED_SPECS_FINGERPRINT}" =~ ^[0-9a-f]{64}$ ||
      ! "${EXPECTED_SPECS_CONTRACT_FINGERPRINT}" =~ ^[0-9a-f]{64}$ ]]; then
  echo "exact source and Specs roots/fingerprints are required" >&2
  exit 2
fi
SPECS_ROOT="$(realpath "${SPECS_ROOT}")"

validate_archive_destination() {
  python3 - "$1" "${REPO_ROOT}" "${SPECS_ROOT}" <<'PY'
import subprocess
import sys
from pathlib import Path

destination = Path(sys.argv[1]).expanduser().resolve(strict=False)
repository = Path(sys.argv[2]).resolve()
specs = Path(sys.argv[3]).resolve()

def within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents

if destination.exists():
    raise SystemExit(f"refusing to overwrite runtime artifact: {destination}")
if within(destination, specs):
    raise SystemExit("runtime artifact output must not be inside the Specs checkout")
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

mapfile -t CPU_ENV < <(python3 - "${MANYFORGE_RUNTIME_SCHEDULE_CPUSET:-}" <<'PY'
import sys
from pathlib import Path
text = sys.argv[1]
cpus = set()
for part in text.split(","):
    if not part:
        raise SystemExit("empty CPU affinity")
    if "-" in part:
        first, last = (int(value) for value in part.split("-", 1))
        if first > last:
            raise SystemExit("descending CPU affinity range")
        cpus.update(range(first, last + 1))
    else:
        cpus.add(int(part))
if len(cpus) != 4:
    raise SystemExit("formal runtime qualification requires exactly four CPUs")
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
    for name, values in (("scaling_governor", governors), ("scaling_driver", drivers)):
        path = root / name
        if not path.is_file():
            raise SystemExit(f"missing CPU frequency provenance: {path}")
        value = path.read_text(encoding="utf-8").strip()
        if not value:
            raise SystemExit(f"empty CPU frequency provenance: {path}")
        values.add(value)
print(",".join(ranges))
print(",".join(sorted(governors)))
print(",".join(sorted(drivers)))
PY
)
[[ ${#CPU_ENV[@]} -eq 3 ]] || { echo "failed to resolve CPU provenance" >&2; exit 1; }
CPUSET="${CPU_ENV[0]}"
CPU_GOVERNOR_SET="${CPU_ENV[1]}"
CPU_FREQUENCY_DRIVER_SET="${CPU_ENV[2]}"
HOST_KERNEL_ID="$(uname -srvm)"

SOURCE_IDENTITY_JSON="$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${REPO_ROOT}" \
  --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT}")"
SPECS_IDENTITY_BUNDLE_JSON="$(python3 "${SOURCE_IDENTITY_CLI}" \
  --repo-root "${SPECS_ROOT}" --include-specs-contract-identity \
  --expect-fingerprint-sha256 "${EXPECTED_SPECS_FINGERPRINT}" \
  --expect-contract-fingerprint-sha256 "${EXPECTED_SPECS_CONTRACT_FINGERPRINT}")"
SPECS_IDENTITY_JSON="$(python3 -c \
  'import json,sys; print(json.dumps(json.loads(sys.argv[1])["source_identity"], sort_keys=True, separators=(",", ":")))' \
  "${SPECS_IDENTITY_BUNDLE_JSON}")"
SPECS_CONTRACT_IDENTITY_JSON="$(python3 -c \
  'import json,sys; print(json.dumps(json.loads(sys.argv[1])["specs_contract_identity"], sort_keys=True, separators=(",", ":")))' \
  "${SPECS_IDENTITY_BUNDLE_JSON}")"
mapfile -t SOURCE_FIELDS < <(python3 -c \
  'import json,sys; v=json.loads(sys.argv[1]); print(v["head_commit"]); print(v["head_tree"]); print(int(v["dirty"])); print(v["delta_sha256"]); print(v["fingerprint_sha256"])' \
  "${SOURCE_IDENTITY_JSON}")
mapfile -t SPECS_FIELDS < <(python3 -c \
  'import json,sys; v=json.loads(sys.argv[1]); print(v["head_commit"]); print(v["head_tree"]); print(int(v["dirty"])); print(v["delta_sha256"]); print(v["fingerprint_sha256"])' \
  "${SPECS_IDENTITY_JSON}")
mapfile -t SPECS_CONTRACT_FIELDS < <(python3 -c \
  'import json,sys; v=json.loads(sys.argv[1]); print(int(v["contract_dirty"])); print(v["contract_delta_sha256"]); print(v["contract_fingerprint_sha256"])' \
  "${SPECS_CONTRACT_IDENTITY_JSON}")

COMPOSE_FILE="${REPO_ROOT}/docker-compose.yaml"
IMAGE_REFERENCE="$(docker compose -f "${COMPOSE_FILE}" images -q dev 2>/dev/null)"
[[ -n "${IMAGE_REFERENCE}" && "${IMAGE_REFERENCE}" != *$'\n'* ]] || {
  echo "Compose dev service does not resolve to exactly one image" >&2; exit 1;
}
ACTUAL_IMAGE_ID="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
ACTUAL_IMAGE_ID="${ACTUAL_IMAGE_ID#sha256:}"
[[ "${ACTUAL_IMAGE_ID}" =~ ^[0-9a-f]{64}$ ]] || {
  echo "unable to resolve exact Compose dev image id" >&2; exit 1;
}
if [[ -n "${MANYFORGE_QUAL_IMAGE_ID:-}" ]]; then
  EXPECTED_IMAGE_ID="${MANYFORGE_QUAL_IMAGE_ID#sha256:}"
  [[ "${EXPECTED_IMAGE_ID}" == "${ACTUAL_IMAGE_ID}" ]] || {
    echo "MANYFORGE_QUAL_IMAGE_ID differs from the actual Compose dev image" >&2; exit 2;
  }
fi

if [[ -n "${MANYFORGE_SOURCE_ROLE:-}" &&
      "${MANYFORGE_SOURCE_ROLE}" != "current_candidate" ]]; then
  echo "this runner emits current_candidate evidence only" >&2
  exit 2
fi
for baseline_only_variable in \
  MANYFORGE_CLEAN_CHECKOUT_MANIFEST_SHA256 \
  MANYFORGE_QUALIFICATION_PATCH_SHA256 \
  MANYFORGE_OBSERVATION_PATCH_SHA256 \
  MANYFORGE_PRISTINE_DECISION_PARITY_SHA256; do
  [[ -z "${!baseline_only_variable:-}" ]] || {
    echo "${baseline_only_variable} is reserved for the baseline lane" >&2; exit 2;
  }
done

PROCESS_INSTANCE_ID="$(python3 -c 'import uuid; print(uuid.uuid4())')"
RUN_ID="${MANYFORGE_QUAL_RUN_ID:-$(date -u +%Y%m%dT%H%M%S%NZ)-${MEASUREMENT_KIND}-${PROCESS_INSTANCE_ID}}"
BUILD_JOBS="${MANYFORGE_BUILD_JOBS:-1}"
[[ "${BUILD_JOBS}" == "1" ]] || {
  echo "formal runtime qualification requires MANYFORGE_BUILD_JOBS=1" >&2; exit 2;
}
REQUESTED_BUILD_DIR="${MANYFORGE_RUNTIME_SCHEDULE_BUILD_DIR:-/workspace/build/step18-runtime-schedule-current}"
case "${REQUESTED_BUILD_DIR}" in
  /workspace/build/*) ;;
  *)
    echo "formal runtime build directory must be below /workspace/build" >&2
    exit 2
    ;;
esac
HOST_BUILD_DIR="$(realpath -m "${REPO_ROOT}/${REQUESTED_BUILD_DIR#/workspace/}")"
case "${HOST_BUILD_DIR}" in
  "${REPO_ROOT}/build/"*) ;;
  *)
    echo "formal runtime build directory resolves outside the ignored build root" >&2
    exit 2
    ;;
esac
BUILD_DIR="/workspace/${HOST_BUILD_DIR#"${REPO_ROOT}/"}"
TARGET="perf_runtime_collision_schedule_${MEASUREMENT_KIND}"
HOST_BINARY="${HOST_BUILD_DIR}/manyforge_integration_tests/${TARGET}"
BUILD_COMMANDS_JSON="$(build_commands_json /workspace "${BUILD_DIR}" "${TARGET}" "${BUILD_JOBS}" "${MATCHED_TOOLCHAIN_CXX_FLAGS}")"
RUNNER_COMMAND_JSON="$(json_argv "${ORIGINAL_RUNNER_ARGV[@]}")"
RUNNER_SCRIPT_SHA256="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"
SOURCE_IDENTITY_SCRIPT_SHA256="$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')"
WRAPPER_SCRIPT_SHA256="$(sha256sum "${ARTIFACT_WRAPPER}" | awk '{print $1}')"
RUNNER_ENVIRONMENT_JSON="$(python3 - \
  "${CPUSET}" "${ACTUAL_IMAGE_ID}" "${MATCHED_TOOLCHAIN_CXX_FLAGS}" \
  "${MATCHED_BUILD_TYPE}" "${CPU_GOVERNOR_SET}" \
  "${CPU_FREQUENCY_DRIVER_SET}" "${HOST_KERNEL_ID}" "${BUILD_JOBS}" \
  "${SOURCE_FIELDS[4]}" "${SPECS_FIELDS[4]}" \
  "${SPECS_CONTRACT_FIELDS[2]}" "${REPO_ROOT}" "${SPECS_ROOT}" <<'PY'
import json, sys
keys = (
    "requested_cpu_affinity", "image_id", "compiler_flags", "build_type",
    "cpu_governor_set", "cpu_frequency_driver_set", "kernel_id", "build_jobs",
    "source_fingerprint_sha256", "specs_fingerprint_sha256",
    "specs_contract_fingerprint_sha256", "source_root", "specs_root",
)
print(json.dumps(dict(zip(keys, sys.argv[1:])), sort_keys=True, separators=(",", ":")))
PY
)"

DOCKER_COMMAND=(docker compose -f "${COMPOSE_FILE}" run --rm -T
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
  -e "MANYFORGE_QUAL_IMAGE_ID=${ACTUAL_IMAGE_ID}"
  -e "MANYFORGE_SOURCE_ROLE=current_candidate"
  -e "MANYFORGE_CHECKOUT_BASE_CLEAN=$([[ ${SOURCE_FIELDS[2]} == 0 ]] && echo 1 || echo 0)"
  -e "MANYFORGE_CLEAN_CHECKOUT_MANIFEST_SHA256="
  -e "MANYFORGE_QUALIFICATION_PATCH_SHA256="
  -e "MANYFORGE_OBSERVATION_PATCH_SHA256="
  -e "MANYFORGE_PRISTINE_DECISION_PARITY_SHA256="
  -e "MANYFORGE_RUNTIME_SCHEDULE_CPUSET=${CPUSET}"
  -e "MANYFORGE_RUNTIME_SCHEDULE_BUILD_DIR=${BUILD_DIR}"
  -e "MANYFORGE_BUILD_JOBS=${BUILD_JOBS}"
  -e "MANYFORGE_CPU_GOVERNOR_SET=${CPU_GOVERNOR_SET}"
  -e "MANYFORGE_CPU_FREQUENCY_DRIVER_SET=${CPU_FREQUENCY_DRIVER_SET}"
  -e "MANYFORGE_HOST_KERNEL_ID=${HOST_KERNEL_ID}"
  -e "MANYFORGE_MATCHED_COMPILER_FLAGS=${MATCHED_TOOLCHAIN_CXX_FLAGS}"
  -e "MANYFORGE_MATCHED_BUILD_TYPE=${MATCHED_BUILD_TYPE}"
  -e "MANYFORGE_QUAL_RUNNER_ENVIRONMENT_JSON=${RUNNER_ENVIRONMENT_JSON}"
  -e "MANYFORGE_QUAL_RUNNER_SCRIPT_SHA256=${RUNNER_SCRIPT_SHA256}"
  -e "MANYFORGE_SOURCE_IDENTITY_SCRIPT_SHA256=${SOURCE_IDENTITY_SCRIPT_SHA256}"
  dev ./scripts/run-runtime-collision-schedule-qualification.sh
  --in-container "${FORWARD_ARGS[@]}")
OUTER_COMMAND_JSON="$(json_argv "${DOCKER_COMMAND[@]}")"

RAW_OUTPUT="$(mktemp)"
WRAPPED_OUTPUT="$(mktemp)"
STAGED_ARCHIVE=""
trap 'rm -f "${RAW_OUTPUT}" "${WRAPPED_OUTPUT}" "${STAGED_ARCHIVE:-}" || true' EXIT
cd "${REPO_ROOT}"
"${DOCKER_COMMAND[@]}" >"${RAW_OUTPUT}"
if [[ ! -x "${HOST_BINARY}" ]]; then
  echo "runtime qualification binary is not retained on the host" >&2
  exit 1
fi
NATIVE_BINARY_SHA256="$(python3 - "${RAW_OUTPUT}" <<'PY'
import json
import sys
from pathlib import Path
value = json.loads(Path(sys.argv[1]).read_bytes())
print(value["identity"]["binary_sha256"])
PY
)"
if [[ ! "${NATIVE_BINARY_SHA256}" =~ ^[0-9a-f]{64}$ ||
      "$(sha256sum "${HOST_BINARY}" | awk '{print $1}')" != "${NATIVE_BINARY_SHA256}" ]]; then
  echo "retained runtime binary does not match the native record" >&2
  exit 1
fi

# Host-side checks close the build/run TOCTOU window for both repositories.
IMAGE_ID_AFTER_MEASUREMENT="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
IMAGE_ID_AFTER_MEASUREMENT="${IMAGE_ID_AFTER_MEASUREMENT#sha256:}"
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${REPO_ROOT}" --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT}")" == "${SOURCE_IDENTITY_JSON}" ]] || {
  echo "ManyForge source identity changed during runtime qualification" >&2; exit 1;
}
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_ROOT}" --include-specs-contract-identity --expect-fingerprint-sha256 "${EXPECTED_SPECS_FINGERPRINT}" --expect-contract-fingerprint-sha256 "${EXPECTED_SPECS_CONTRACT_FINGERPRINT}")" == "${SPECS_IDENTITY_BUNDLE_JSON}" ]] || {
  echo "Specs source/contract identity changed during runtime qualification" >&2; exit 1;
}
[[ "${IMAGE_ID_AFTER_MEASUREMENT}" == "${ACTUAL_IMAGE_ID}" &&
   "$(uname -srvm)" == "${HOST_KERNEL_ID}" ]] || {
  echo "Compose image or host kernel changed during runtime qualification" >&2; exit 1;
}

python3 "${ARTIFACT_WRAPPER}" --input "${RAW_OUTPUT}" \
  --process-instance-id "${PROCESS_INSTANCE_ID}" \
  --source-identity-json "${SOURCE_IDENTITY_JSON}" \
  --specs-source-identity-json "${SPECS_IDENTITY_JSON}" \
  --specs-contract-identity-json "${SPECS_CONTRACT_IDENTITY_JSON}" \
  --runner-command-json "${RUNNER_COMMAND_JSON}" \
  --build-commands-json "${BUILD_COMMANDS_JSON}" \
  --outer-command-json "${OUTER_COMMAND_JSON}" \
  --runner-environment-json "${RUNNER_ENVIRONMENT_JSON}" \
  --runner-script-sha256 "${RUNNER_SCRIPT_SHA256}" \
  --source-identity-script-sha256 "${SOURCE_IDENTITY_SCRIPT_SHA256}" \
  >"${WRAPPED_OUTPUT}"

IMAGE_ID_AFTER_WRAPPING="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
IMAGE_ID_AFTER_WRAPPING="${IMAGE_ID_AFTER_WRAPPING#sha256:}"
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${REPO_ROOT}" --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT}")" == "${SOURCE_IDENTITY_JSON}" &&
   "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_ROOT}" --include-specs-contract-identity --expect-fingerprint-sha256 "${EXPECTED_SPECS_FINGERPRINT}" --expect-contract-fingerprint-sha256 "${EXPECTED_SPECS_CONTRACT_FINGERPRINT}")" == "${SPECS_IDENTITY_BUNDLE_JSON}" &&
   "${IMAGE_ID_AFTER_WRAPPING}" == "${ACTUAL_IMAGE_ID}" &&
   "$(uname -srvm)" == "${HOST_KERNEL_ID}" &&
   "$(sha256sum "${HOST_BINARY}" | awk '{print $1}')" == "${NATIVE_BINARY_SHA256}" &&
   "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" == "${RUNNER_SCRIPT_SHA256}" &&
   "$(sha256sum "${ARTIFACT_WRAPPER}" | awk '{print $1}')" == "${WRAPPER_SCRIPT_SHA256}" &&
   "$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')" == "${SOURCE_IDENTITY_SCRIPT_SHA256}" ]] || {
  echo "source, image, or kernel identity changed while wrapping runtime artifact" >&2; exit 1;
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
[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${REPO_ROOT}" --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT}")" == "${SOURCE_IDENTITY_JSON}" &&
   "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_ROOT}" --include-specs-contract-identity --expect-fingerprint-sha256 "${EXPECTED_SPECS_FINGERPRINT}" --expect-contract-fingerprint-sha256 "${EXPECTED_SPECS_CONTRACT_FINGERPRINT}")" == "${SPECS_IDENTITY_BUNDLE_JSON}" &&
   "$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null | sed 's/^sha256://')" == "${ACTUAL_IMAGE_ID}" &&
   "$(uname -srvm)" == "${HOST_KERNEL_ID}" &&
   "$(sha256sum "${HOST_BINARY}" | awk '{print $1}')" == "${NATIVE_BINARY_SHA256}" &&
   "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" == "${RUNNER_SCRIPT_SHA256}" &&
   "$(sha256sum "${ARTIFACT_WRAPPER}" | awk '{print $1}')" == "${WRAPPER_SCRIPT_SHA256}" &&
   "$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')" == "${SOURCE_IDENTITY_SCRIPT_SHA256}" ]] || {
  echo "candidate source, Specs, binary, or runtime qualification tool changed before archival completed" >&2
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
