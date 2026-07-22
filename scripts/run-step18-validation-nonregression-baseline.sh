#!/usr/bin/env bash
# Run one proof-cell record from the prepared pristine 2bfe30e timing lane.

set -euo pipefail

ORIGINAL_RUNNER_ARGV=("$0" "$@")

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LANE_DIR="${MANYFORGE_STEP18_NONREGRESSION_BASELINE_DIR:-${REPO_ROOT}/build/step18-validation-nonregression-baseline-lane}"
SOURCE_IDENTITY_HELPER="${REPO_ROOT}/scripts/qualification/source_identity.py"
POLICY_COMPAT_PATCH="${REPO_ROOT}/scripts/qualification/baseline/step18_policy_support_baseline_compat.patch"
FREE_SPACE_PROFILE_PATCH="${REPO_ROOT}/scripts/qualification/baseline/step18_free_space_profile.patch"
FREE_SPACE_QUERY_PROFILE_PATCH="${REPO_ROOT}/scripts/qualification/baseline/step18_free_space_query_profile.patch"
WARMUP_RUNS=5
MEASURED_RUNS=50
CELL=""
FORWARD_ARGS=()

usage() {
  cat >&2 <<'USAGE'
usage: run-step18-validation-nonregression-baseline.sh \
  --cell {policy-support-contact,precise-reference-parity,contact-grasp,free-space} [benchmark options]

Policy-support-contact and contact-grasp options are forwarded to their
qualification binaries. free-space uses the fixed 5/50 profile automatically. Set
MANYFORGE_STEP18_PROOF_JSON_OUT to archive the typed provenance envelope.
USAGE
}

while (($# > 0)); do
  case "$1" in
    --cell)
      [[ $# -ge 2 ]] || { echo "--cell requires a value" >&2; exit 2; }
      CELL="$2"
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
if [[ "${CELL}" != "policy-support-contact" &&
      "${CELL}" != "precise-reference-parity" &&
      "${CELL}" != "contact-grasp" && "${CELL}" != "free-space" ]]; then
  echo "--cell must be policy-support-contact, precise-reference-parity, contact-grasp, or free-space" >&2
  exit 2
fi
if [[ "${CELL}" == "free-space" && ${#FORWARD_ARGS[@]} -ne 0 ]]; then
  echo "free-space accepts no benchmark options" >&2
  exit 2
fi
if [[ "${CELL}" == "precise-reference-parity" ]]; then
  if [[ ${#FORWARD_ARGS[@]} -ne 0 ]]; then
    echo "precise-reference-parity accepts no benchmark options" >&2
    exit 2
  fi
  FORWARD_ARGS=(
    --precise-reference-only
    --warmup-runs "${WARMUP_RUNS}"
    --measured-runs "${MEASURED_RUNS}"
  )
elif [[ "${CELL}" != "free-space" ]]; then
  python3 - "${FORWARD_ARGS[@]}" <<'PY'
import sys

arguments = sys.argv[1:]
expected = {"--warmup-runs": "5", "--measured-runs": "50"}
for option, value in expected.items():
    positions = [index for index, item in enumerate(arguments) if item == option]
    if len(positions) != 1 or positions[0] + 1 >= len(arguments):
        raise SystemExit(f"{option} must occur exactly once")
    if arguments[positions[0] + 1] != value:
        raise SystemExit(f"{option} must be {value}")
PY
else
  FORWARD_ARGS=(
    --warmup-runs "${WARMUP_RUNS}"
    --measured-runs "${MEASURED_RUNS}"
  )
fi

LANE_DIR="$(realpath -m "${LANE_DIR}")"
case "${LANE_DIR}" in
  "${REPO_ROOT}/build/"*) ;;
  *) echo "baseline lane must be below ${REPO_ROOT}/build" >&2; exit 2 ;;
esac
ARTIFACT_DIR="${LANE_DIR}/artifacts"
METADATA="${ARTIFACT_DIR}/lane-metadata.json"
CLEAN_MANIFEST="${ARTIFACT_DIR}/clean-checkout-manifest.json"
QUALIFICATION_PATCH="${ARTIFACT_DIR}/qualification-overlay.patch"
WRAPPER="${REPO_ROOT}/scripts/qualification/wrap_step18_proof_artifact.py"
if [[ ! -f "${METADATA}" || ! -f "${CLEAN_MANIFEST}" ||
      ! -f "${QUALIFICATION_PATCH}" || ! -f "${WRAPPER}" ||
      ! -f "${SOURCE_IDENTITY_HELPER}" || ! -f "${POLICY_COMPAT_PATCH}" ||
      ! -f "${FREE_SPACE_PROFILE_PATCH}" ||
      ! -f "${FREE_SPACE_QUERY_PROFILE_PATCH}" ]]; then
  echo "prepared baseline provenance is incomplete" >&2
  exit 1
fi
METADATA_SHA256="$(sha256sum "${METADATA}" | awk '{print $1}')"

mapfile -t META < <(python3 - "${METADATA}" <<'PY'
import json
import sys
from pathlib import Path

value = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if value.get("schema_version") != "manyforge.step18.validation_nonregression_baseline_lane.v2":
    raise SystemExit("invalid baseline lane metadata schema")
for key in (
    "source_identity",
    "specs_source_identity",
    "specs_contract_identity",
    "expected_source_fingerprint_sha256",
    "expected_specs_source_fingerprint_sha256",
    "expected_specs_contract_fingerprint_sha256",
    "source_root",
    "specs_root",
    "image_id",
    "requested_cpu_affinity",
    "baseline_toolchain_cxx_flags",
    "compiler",
    "build_type",
    "build_jobs",
    "clean_checkout_manifest_sha256",
    "qualification_patch_sha256",
    "contact_grasp_binary_sha256",
    "policy_support_binary_sha256",
    "free_space_binary_sha256",
    "policy_support_corpus_sha256",
    "build_command_argv",
    "prepare_script_sha256",
    "policy_compat_patch_sha256",
    "free_space_profile_patch_sha256",
    "free_space_query_profile_patch_sha256",
    "source_identity_helper_sha256",
):
    item = value[key]
    print(json.dumps(item, separators=(",", ":")) if isinstance(item, (dict, list)) else item)
PY
)
if [[ ${#META[@]} -ne 26 ]]; then
  echo "baseline lane metadata is incomplete" >&2
  exit 1
fi
SOURCE_IDENTITY_JSON="${META[0]}"
SPECS_SOURCE_IDENTITY_JSON="${META[1]}"
SPECS_CONTRACT_IDENTITY_JSON="${META[2]}"
EXPECTED_SOURCE_FINGERPRINT_SHA256="${META[3]}"
EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256="${META[4]}"
EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256="${META[5]}"
SOURCE_TREE="${META[6]}"
SPECS_TREE="${META[7]}"
IMAGE_ID="${META[8]}"
CPUSET="${META[9]}"
MATCHED_TOOLCHAIN_CXX_FLAGS="${META[10]}"
COMPILER_VERSION="${META[11]}"
BUILD_TYPE="${META[12]}"
BUILD_JOBS="${META[13]}"
CLEAN_MANIFEST_SHA256="${META[14]}"
QUALIFICATION_PATCH_SHA256="${META[15]}"
CONTACT_BINARY_SHA256="${META[16]}"
POLICY_BINARY_SHA256="${META[17]}"
FREE_SPACE_BINARY_SHA256="${META[18]}"
CORPUS_SHA256="${META[19]}"
BUILD_COMMAND_JSON="${META[20]}"
PREPARE_SCRIPT_SHA256="${META[21]}"
POLICY_COMPAT_PATCH_SHA256="${META[22]}"
FREE_SPACE_PROFILE_PATCH_SHA256="${META[23]}"
FREE_SPACE_QUERY_PROFILE_PATCH_SHA256="${META[24]}"
SOURCE_IDENTITY_HELPER_SHA256="${META[25]}"

if [[ ! "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ||
      ( -n "${MANYFORGE_BUILD_JOBS:-}" &&
        "${MANYFORGE_BUILD_JOBS}" != "${BUILD_JOBS}" ) ]]; then
  echo "run build parallelism differs from the prepared lane" >&2
  exit 2
fi

if [[ -n "${MANYFORGE_STEP18_NONREGRESSION_CPUSET:-}" &&
      "${MANYFORGE_STEP18_NONREGRESSION_CPUSET}" != "${CPUSET}" ]]; then
  echo "run CPU affinity differs from the prepared lane" >&2
  exit 2
fi
mapfile -t CPU_FREQUENCY_ENV < <(python3 - "${CPUSET}" <<'PY'
import sys
from pathlib import Path

cpus = set()
for part in sys.argv[1].split(","):
    if "-" in part:
        first, last = (int(value) for value in part.split("-", 1))
        if first > last:
            raise SystemExit("invalid descending CPU affinity range")
        cpus.update(range(first, last + 1))
    else:
        cpus.add(int(part))
if len(cpus) != 4:
    raise SystemExit("Step 18 formal CPU affinity must select exactly four cores")
governors = set()
drivers = set()
for cpu in sorted(cpus):
    root = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    for name, values in (("scaling_governor", governors), ("scaling_driver", drivers)):
        path = root / name
        if not path.is_file():
            raise SystemExit(f"missing CPU frequency provenance: {path}")
        value = path.read_text(encoding="utf-8").strip()
        if not value:
            raise SystemExit(f"empty CPU frequency provenance: {path}")
        values.add(value)
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
print(",".join(ranges))
print(",".join(sorted(governors)))
print(",".join(sorted(drivers)))
PY
)
if [[ ${#CPU_FREQUENCY_ENV[@]} -ne 3 ]]; then
  echo "failed to resolve CPU governor/driver provenance" >&2
  exit 1
fi
if [[ "${CPU_FREQUENCY_ENV[0]}" != "${CPUSET}" ]]; then
  echo "prepared lane CPU affinity is not canonical" >&2
  exit 1
fi
CPU_GOVERNOR_SET="${CPU_FREQUENCY_ENV[1]}"
CPU_FREQUENCY_DRIVER_SET="${CPU_FREQUENCY_ENV[2]}"
HOST_KERNEL_ID="$(uname -srvm)"

resolve_dev_image_id() {
  local image_reference
  image_reference="$(
    docker compose -f "${REPO_ROOT}/docker-compose.yaml" images -q dev
  )"
  if [[ -z "${image_reference}" || "${image_reference}" == *$'\n'* ]]; then
    echo "Compose dev service does not resolve to exactly one image" >&2
    return 1
  fi
  docker image inspect "${image_reference}" --format '{{.Id}}' |
    sed 's/^sha256://'
}

if [[ "$(realpath -m "${SOURCE_TREE}")" != "${LANE_DIR}/source" ||
      "$(realpath -m "${SPECS_TREE}")" != "${LANE_DIR}/specs-source" ]]; then
  echo "prepared baseline source roots are not the exact lane roots" >&2
  exit 1
fi
RUNNER_SCRIPT_SHA256="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"
WRAPPER_SCRIPT_SHA256="$(sha256sum "${WRAPPER}" | awk '{print $1}')"

validate_archive_destination() {
  python3 - "$1" "${REPO_ROOT}" "${SOURCE_TREE}" "${SPECS_TREE}" <<'PY'
import subprocess
import sys
from pathlib import Path

destination = Path(sys.argv[1]).expanduser().resolve(strict=False)
repository, source, specs = (Path(value).resolve() for value in sys.argv[2:])

def within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents

if destination.exists():
    raise SystemExit(f"refusing to overwrite proof artifact: {destination}")
if within(destination, source) or within(destination, specs):
    raise SystemExit("proof artifact output must not be inside a detached checkout")
if within(destination, repository):
    allowed = (
        repository / "qualification",
        repository / ".manyforge-var" / "qualification",
        repository / "build",
    )
    if not any(within(destination, root) for root in allowed):
        raise SystemExit(
            "proof artifact output inside ManyForge must be below an ignored "
            "qualification or build evidence root"
        )
    relative = destination.relative_to(repository)
    if subprocess.run(
        ["git", "-C", str(repository), "check-ignore", "-q", "--", str(relative)],
        check=False,
    ).returncode != 0:
        raise SystemExit(
            "proof artifact output inside ManyForge must be actually ignored by git"
        )
print(destination)
PY
}

ARCHIVE_OUTPUT=""
if [[ -n "${MANYFORGE_STEP18_PROOF_JSON_OUT:-}" ]]; then
  ARCHIVE_OUTPUT="$(
    validate_archive_destination "${MANYFORGE_STEP18_PROOF_JSON_OUT}"
  )"
fi
SOURCE_IDENTITY_BEFORE_MEASUREMENT="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_BEFORE_MEASUREMENT="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" \
    --include-specs-contract-identity \
    --expect-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
    --expect-contract-fingerprint-sha256 \
    "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
)"
mapfile -t SPECS_IDENTITIES_BEFORE_MEASUREMENT < <(python3 - \
  "${SPECS_IDENTITY_BUNDLE_BEFORE_MEASUREMENT}" <<'PY'
import json
import sys

value = json.loads(sys.argv[1])
for field in ("source_identity", "specs_contract_identity"):
    print(json.dumps(value[field], sort_keys=True, separators=(",", ":")))
PY
)
if [[ ${#SPECS_IDENTITIES_BEFORE_MEASUREMENT[@]} -ne 2 ]]; then
  echo "failed to capture baseline Specs identities before measurement" >&2
  exit 1
fi
SPECS_SOURCE_IDENTITY_BEFORE_MEASUREMENT="${SPECS_IDENTITIES_BEFORE_MEASUREMENT[0]}"
SPECS_CONTRACT_IDENTITY_BEFORE_MEASUREMENT="${SPECS_IDENTITIES_BEFORE_MEASUREMENT[1]}"
python3 - "${SOURCE_IDENTITY_JSON}" "${SOURCE_IDENTITY_BEFORE_MEASUREMENT}" \
  "${SPECS_SOURCE_IDENTITY_JSON}" \
  "${SPECS_SOURCE_IDENTITY_BEFORE_MEASUREMENT}" \
  "${SPECS_CONTRACT_IDENTITY_JSON}" \
  "${SPECS_CONTRACT_IDENTITY_BEFORE_MEASUREMENT}" <<'PY'
import json
import sys

(
    expected_source,
    actual_source,
    expected_specs,
    actual_specs,
    expected_contract,
    actual_contract,
) = (
    json.loads(value) for value in sys.argv[1:]
)
if (
    expected_source != actual_source
    or expected_specs != actual_specs
    or expected_contract != actual_contract
):
    raise SystemExit("prepared baseline source identity objects changed")
PY
ACTUAL_IMAGE_ID="$(resolve_dev_image_id)"
EXPECTED_IMAGE_ID="${MANYFORGE_QUAL_IMAGE_ID:-}"
EXPECTED_IMAGE_ID="${EXPECTED_IMAGE_ID#sha256:}"
ACTUAL_COMPILER_VERSION="$(
  docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev \
    bash -lc 'c++ -dumpfullversion -dumpversion'
)"
if [[ "${ACTUAL_IMAGE_ID}" != "${IMAGE_ID}" ||
      ( -n "${EXPECTED_IMAGE_ID}" && "${EXPECTED_IMAGE_ID}" != "${ACTUAL_IMAGE_ID}" ) ||
      "${ACTUAL_COMPILER_VERSION}" != "${COMPILER_VERSION}" ||
      "${BUILD_TYPE}" != "Release" ||
      "$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')" != "${SOURCE_IDENTITY_HELPER_SHA256}" ||
      "$(sha256sum "${POLICY_COMPAT_PATCH}" | awk '{print $1}')" != "${POLICY_COMPAT_PATCH_SHA256}" ||
      "$(sha256sum "${FREE_SPACE_PROFILE_PATCH}" | awk '{print $1}')" != "${FREE_SPACE_PROFILE_PATCH_SHA256}" ||
      "$(sha256sum "${FREE_SPACE_QUERY_PROFILE_PATCH}" | awk '{print $1}')" != "${FREE_SPACE_QUERY_PROFILE_PATCH_SHA256}" ||
      "$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')" != "${CLEAN_MANIFEST_SHA256}" ||
      "$(sha256sum "${QUALIFICATION_PATCH}" | awk '{print $1}')" != "${QUALIFICATION_PATCH_SHA256}" ]]; then
  echo "prepared baseline source, image, toolchain, or provenance identity changed" >&2
  exit 1
fi

if [[ "${CELL}" == "contact-grasp" ]]; then
  BINARY="${LANE_DIR}/build/manyforge_integration_tests/perf_contact_grasp_validation_baseline"
  BINARY_SHA256="${CONTACT_BINARY_SHA256}"
  ENVELOPE_CELL="contact_grasp"
elif [[ "${CELL}" == "policy-support-contact" ]]; then
  BINARY="${LANE_DIR}/build/manyforge_integration_tests/perf_policy_support_contact_baseline"
  BINARY_SHA256="${POLICY_BINARY_SHA256}"
  ENVELOPE_CELL="policy_support_contact"
elif [[ "${CELL}" == "precise-reference-parity" ]]; then
  BINARY="${LANE_DIR}/build/manyforge_integration_tests/perf_policy_support_contact_baseline"
  BINARY_SHA256="${POLICY_BINARY_SHA256}"
  ENVELOPE_CELL="precise_reference_parity"
else
  BINARY="${LANE_DIR}/build/manyforge_integration_tests/perf_collision_engine_qualification"
  BINARY_SHA256="${FREE_SPACE_BINARY_SHA256}"
  ENVELOPE_CELL="free_space"
fi
if [[ ! -x "${BINARY}" ||
      "$(sha256sum "${BINARY}" | awk '{print $1}')" != "${BINARY_SHA256}" ]]; then
  echo "prepared baseline binary identity changed" >&2
  exit 1
fi

PROCESS_INSTANCE_ID="$(python3 -c 'import uuid; print(uuid.uuid4())')"
RUN_ID="${MANYFORGE_QUAL_RUN_ID:-$(date -u +%Y%m%dT%H%M%S%NZ)-baseline-${CELL}-${PROCESS_INSTANCE_ID}}"
CONTAINER_BINARY="/workspace/${BINARY#"${REPO_ROOT}/"}"
RAW_OUTPUT="$(mktemp)"
WRAPPED_OUTPUT="$(mktemp)"
STAGED_ARCHIVE=""
trap 'rm -f "${RAW_OUTPUT}" "${WRAPPED_OUTPUT}" "${STAGED_ARCHIVE:-}" || true' EXIT
COMMAND=(taskset -c "${CPUSET}" "${CONTAINER_BINARY}" "${FORWARD_ARGS[@]}")
OUTER_COMMAND=(docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T
  -e "MANYFORGE_QUAL_IMAGE_ID=${IMAGE_ID}"
  -e "MANYFORGE_QUAL_RUN_ID=${RUN_ID}"
  -e "MANYFORGE_QUAL_BINARY_SHA256=${BINARY_SHA256}"
  -e "MANYFORGE_STEP18_NONREGRESSION_CPUSET=${CPUSET}"
  -e "MANYFORGE_POLICY_SUPPORT_CORPUS_SHA256=${CORPUS_SHA256}"
  -e "MANYFORGE_CPU_GOVERNOR_SET=${CPU_GOVERNOR_SET}"
  -e "MANYFORGE_CPU_FREQUENCY_DRIVER_SET=${CPU_FREQUENCY_DRIVER_SET}"
  -e "MANYFORGE_HOST_KERNEL_ID=${HOST_KERNEL_ID}"
  -e "MANYFORGE_MATCHED_COMPILER_FLAGS=${MATCHED_TOOLCHAIN_CXX_FLAGS}"
  -e "MANYFORGE_SOURCE_ROLE=pristine_baseline_timing"
  -e "MANYFORGE_CLEAN_CHECKOUT_MANIFEST_SHA256=${CLEAN_MANIFEST_SHA256}"
  -e "MANYFORGE_QUALIFICATION_PATCH_SHA256=${QUALIFICATION_PATCH_SHA256}"
  dev bash -lc 'set -euo pipefail
   export CMAKE_PREFIX_PATH="/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix:${CMAKE_PREFIX_PATH:-}"
   export LD_LIBRARY_PATH="/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix/lib:${LD_LIBRARY_PATH:-}"
   exec "$@"' baseline-proof "${COMMAND[@]}")
"${OUTER_COMMAND[@]}" >"${RAW_OUTPUT}"

SOURCE_IDENTITY_AFTER_MEASUREMENT="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_AFTER_MEASUREMENT="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" \
    --include-specs-contract-identity \
    --expect-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
    --expect-contract-fingerprint-sha256 \
    "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
)"
IMAGE_ID_AFTER_MEASUREMENT="$(resolve_dev_image_id)"
COMPILER_VERSION_AFTER_MEASUREMENT="$(
  docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev \
    bash -lc 'c++ -dumpfullversion -dumpversion'
)"
if [[ "${SOURCE_IDENTITY_AFTER_MEASUREMENT}" != "${SOURCE_IDENTITY_BEFORE_MEASUREMENT}" ||
      "${SPECS_IDENTITY_BUNDLE_AFTER_MEASUREMENT}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_MEASUREMENT}" ||
      "${IMAGE_ID_AFTER_MEASUREMENT}" != "${IMAGE_ID}" ||
      "${COMPILER_VERSION_AFTER_MEASUREMENT}" != "${COMPILER_VERSION}" ]]; then
  echo "source, Specs, image, or compiler identity changed during baseline measurement" >&2
  exit 1
fi

argv_json() {
  python3 -c 'import json, sys; print(json.dumps(sys.argv[1:], separators=(",", ":")))' "$@"
}
RUNNER_COMMAND_JSON="$(argv_json "${ORIGINAL_RUNNER_ARGV[@]}")"
OUTER_COMMAND_JSON="$(argv_json "${OUTER_COMMAND[@]}")"
RUNNER_ENVIRONMENT_JSON="$(python3 - \
  "${CPUSET}" "${IMAGE_ID}" "${MATCHED_TOOLCHAIN_CXX_FLAGS}" \
  "${COMPILER_VERSION}" "${BUILD_TYPE}" "${CPU_GOVERNOR_SET}" \
  "${CPU_FREQUENCY_DRIVER_SET}" "${HOST_KERNEL_ID}" "${CORPUS_SHA256}" \
  "${SOURCE_TREE}" "${SPECS_TREE}" "${PREPARE_SCRIPT_SHA256}" \
  "${POLICY_COMPAT_PATCH_SHA256}" "${FREE_SPACE_PROFILE_PATCH_SHA256}" \
  "${FREE_SPACE_QUERY_PROFILE_PATCH_SHA256}" \
  "${SOURCE_IDENTITY_HELPER_SHA256}" "${WARMUP_RUNS}" \
  "${MEASURED_RUNS}" "${BUILD_JOBS}" <<'PY'
import json
import sys
keys = (
    "requested_cpu_affinity", "image_id", "compiler_flags",
    "compiler", "build_type", "cpu_governor_set",
    "cpu_frequency_driver_set", "kernel_id",
    "policy_support_corpus_sha256", "source_root", "specs_root",
    "prepare_script_sha256", "policy_compat_patch_sha256",
    "free_space_profile_patch_sha256", "free_space_query_profile_patch_sha256",
    "source_identity_helper_sha256",
    "warmup_runs", "measured_runs", "build_jobs",
)
values = dict(zip(keys, sys.argv[1:]))
values["warmup_runs"] = int(values["warmup_runs"])
values["measured_runs"] = int(values["measured_runs"])
values["build_jobs"] = int(values["build_jobs"])
print(json.dumps(values, sort_keys=True, separators=(",", ":")))
PY
)"
WRAP_COMMAND=(python3 "${WRAPPER}"
  --input "${RAW_OUTPUT}"
  --cell "${ENVELOPE_CELL}"
  --run-id "${RUN_ID}"
  --process-instance-id "${PROCESS_INSTANCE_ID}"
  --source-role pristine_baseline_timing
  --source-identity-json "${SOURCE_IDENTITY_BEFORE_MEASUREMENT}"
  --specs-source-identity-json "${SPECS_SOURCE_IDENTITY_BEFORE_MEASUREMENT}"
  --specs-contract-identity-json "${SPECS_CONTRACT_IDENTITY_BEFORE_MEASUREMENT}"
  --expected-source-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
  --expected-specs-source-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}"
  --expected-specs-contract-fingerprint-sha256 "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
  --image-id "${IMAGE_ID}"
  --binary-sha256 "${BINARY_SHA256}"
  --requested-cpu-affinity "${CPUSET}"
  --warmup-runs "${WARMUP_RUNS}"
  --measured-runs "${MEASURED_RUNS}"
  --checkout-base-clean 1
  --clean-checkout-manifest-sha256 "${CLEAN_MANIFEST_SHA256}"
  --qualification-patch-sha256 "${QUALIFICATION_PATCH_SHA256}"
  --runner-script-sha256 "${RUNNER_SCRIPT_SHA256}"
  --wrapper-script-sha256 "${WRAPPER_SCRIPT_SHA256}"
  --source-identity-helper-sha256 "${SOURCE_IDENTITY_HELPER_SHA256}"
  --runner-command-json "${RUNNER_COMMAND_JSON}"
  --build-command-json "${BUILD_COMMAND_JSON}"
  --outer-command-json "${OUTER_COMMAND_JSON}"
  --runner-environment-json "${RUNNER_ENVIRONMENT_JSON}"
  -- "${COMMAND[@]}")
"${WRAP_COMMAND[@]}" >"${WRAPPED_OUTPUT}"

SOURCE_IDENTITY_AFTER_WRAPPING="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_AFTER_WRAPPING="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" \
    --include-specs-contract-identity \
    --expect-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
    --expect-contract-fingerprint-sha256 \
      "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
)"
IMAGE_ID_AFTER_WRAPPING="$(resolve_dev_image_id)"
COMPILER_VERSION_AFTER_WRAPPING="$(
  docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev \
    bash -lc 'c++ -dumpfullversion -dumpversion'
)"
if [[ "${SOURCE_IDENTITY_AFTER_WRAPPING}" != "${SOURCE_IDENTITY_BEFORE_MEASUREMENT}" ||
      "${SPECS_IDENTITY_BUNDLE_AFTER_WRAPPING}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_MEASUREMENT}" ||
      "${IMAGE_ID_AFTER_WRAPPING}" != "${IMAGE_ID}" ||
      "${COMPILER_VERSION_AFTER_WRAPPING}" != "${COMPILER_VERSION}" ||
      "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" != "${RUNNER_SCRIPT_SHA256}" ||
      "$(sha256sum "${WRAPPER}" | awk '{print $1}')" != "${WRAPPER_SCRIPT_SHA256}" ||
      "$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')" != "${SOURCE_IDENTITY_HELPER_SHA256}" ]]; then
  echo "source, Specs, image, or compiler identity changed during baseline wrapping" >&2
  exit 1
fi

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
    echo "proof artifact destination changed before publication" >&2
    exit 1
  }
fi
SOURCE_IDENTITY_FINAL="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_FINAL="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" \
    --include-specs-contract-identity \
    --expect-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
    --expect-contract-fingerprint-sha256 \
      "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
)"
if [[ "${SOURCE_IDENTITY_FINAL}" != "${SOURCE_IDENTITY_BEFORE_MEASUREMENT}" ||
      "${SPECS_IDENTITY_BUNDLE_FINAL}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_MEASUREMENT}" ||
      "$(resolve_dev_image_id)" != "${IMAGE_ID}" ||
      "$(docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev bash -lc 'c++ -dumpfullversion -dumpversion')" != "${COMPILER_VERSION}" ||
      "$(sha256sum "${BINARY}" | awk '{print $1}')" != "${BINARY_SHA256}" ||
      "$(sha256sum "${METADATA}" | awk '{print $1}')" != "${METADATA_SHA256}" ||
      "$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')" != "${CLEAN_MANIFEST_SHA256}" ||
      "$(sha256sum "${QUALIFICATION_PATCH}" | awk '{print $1}')" != "${QUALIFICATION_PATCH_SHA256}" ||
      "$(sha256sum "${POLICY_COMPAT_PATCH}" | awk '{print $1}')" != "${POLICY_COMPAT_PATCH_SHA256}" ||
      "$(sha256sum "${FREE_SPACE_PROFILE_PATCH}" | awk '{print $1}')" != "${FREE_SPACE_PROFILE_PATCH_SHA256}" ||
      "$(sha256sum "${FREE_SPACE_QUERY_PROFILE_PATCH}" | awk '{print $1}')" != "${FREE_SPACE_QUERY_PROFILE_PATCH_SHA256}" ||
      "$(sha256sum "${REPO_ROOT}/benchmarks/roboplan_05/corpus/corpus-v1.json" | awk '{print $1}')" != "${CORPUS_SHA256}" ||
      "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" != "${RUNNER_SCRIPT_SHA256}" ||
      "$(sha256sum "${WRAPPER}" | awk '{print $1}')" != "${WRAPPER_SCRIPT_SHA256}" ||
      "$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')" != "${SOURCE_IDENTITY_HELPER_SHA256}" ]]; then
  echo "baseline source, Specs, binary, image, compiler, or qualification input changed before proof archival completed" >&2
  exit 1
fi
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
    raise SystemExit("staged proof artifact changed before publication")
try:
    os.link(staged, destination)
except FileExistsError as error:
    raise SystemExit(f"refusing to overwrite proof artifact: {destination}") from error
finally:
    try:
        staged.unlink(missing_ok=True)
    except OSError:
        pass
PY
  STAGED_ARCHIVE=""
fi
