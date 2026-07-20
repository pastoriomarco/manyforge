#!/usr/bin/env bash
# Build and run one exact-current Step 18 proof-cell process, then bind its
# native JSON payload to source/image/binary/command provenance.

set -euo pipefail

ORIGINAL_RUNNER_ARGV=("$0" "$@")

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${MANYFORGE_STEP18_PROOF_BUILD_DIR:-${REPO_ROOT}/build/step18-proof-cells-current}"
CPUSET="${MANYFORGE_STEP18_NONREGRESSION_CPUSET:-}"
BUILD_JOBS="${MANYFORGE_BUILD_JOBS:-2}"
CELL=""
ARM=""
WAYPOINTS=96
WORKERS=4
WARMUP=5
MEASURED=50
MATCHED_TOOLCHAIN_CXX_FLAGS="-Wno-error=maybe-uninitialized"
BUILD_TYPE="Release"
SOURCE_IDENTITY_HELPER="${REPO_ROOT}/scripts/qualification/source_identity.py"

usage() {
  cat >&2 <<'USAGE'
usage: run-step18-proof-cell-qualification.sh --cell CELL [options]

Cells:
  lightweight-collision-only --arm {binary_contact,collision_only}
                             --waypoints {96,500,1000}
  policy-support-contact     --arm {signed_penetration_selective,
                                    binary_contact,bounded_penetration,
                                    collision_only}
                             --waypoints {96,500,1000} --workers {1,4}
  precise-reference-parity   untimed exact baseline/current semantic corpus
  contact-grasp             --waypoints {96,500} --workers {1,4}
  free-space

Common timing options are --warmup N and --measured N. Strict qualification
uses five warm runs and 50 measured runs. Set MANYFORGE_STEP18_PROOF_JSON_OUT
to archive the typed JSON envelope. MANYFORGE_SPECS_ROOT plus
MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256 and
MANYFORGE_EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256 and
MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256 are mandatory.
USAGE
}

while (($# > 0)); do
  case "$1" in
    --cell)
      [[ $# -ge 2 ]] || { echo "--cell requires a value" >&2; exit 2; }
      CELL="$2"; shift 2 ;;
    --arm)
      [[ $# -ge 2 ]] || { echo "--arm requires a value" >&2; exit 2; }
      ARM="$2"; shift 2 ;;
    --waypoints)
      [[ $# -ge 2 ]] || { echo "--waypoints requires a value" >&2; exit 2; }
      WAYPOINTS="$2"; shift 2 ;;
    --workers)
      [[ $# -ge 2 ]] || { echo "--workers requires a value" >&2; exit 2; }
      WORKERS="$2"; shift 2 ;;
    --warmup)
      [[ $# -ge 2 ]] || { echo "--warmup requires a value" >&2; exit 2; }
      WARMUP="$2"; shift 2 ;;
    --measured)
      [[ $# -ge 2 ]] || { echo "--measured requires a value" >&2; exit 2; }
      MEASURED="$2"; shift 2 ;;
    --help|-h)
      usage; exit 0 ;;
    *)
      echo "unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

for value_name in WAYPOINTS WORKERS WARMUP MEASURED BUILD_JOBS; do
  value="${!value_name}"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "${value_name} must be a positive integer" >&2
    exit 2
  fi
done
if [[ -z "${CPUSET}" || ! "${CPUSET}" =~ ^[0-9,-]+$ ]]; then
  echo "MANYFORGE_STEP18_NONREGRESSION_CPUSET must be explicit" >&2
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
CPUSET="${CPU_FREQUENCY_ENV[0]}"
CPU_GOVERNOR_SET="${CPU_FREQUENCY_ENV[1]}"
CPU_FREQUENCY_DRIVER_SET="${CPU_FREQUENCY_ENV[2]}"
HOST_KERNEL_ID="$(uname -srvm)"
if [[ "${WARMUP}" != 5 || "${MEASURED}" != 50 ]]; then
  echo "Step 18 proof cells require exactly --warmup 5 --measured 50" >&2
  exit 2
fi
case "${CELL}" in
  lightweight-collision-only)
    if [[ "${ARM}" != "binary_contact" && "${ARM}" != "collision_only" ]]; then
      echo "lightweight-collision-only requires binary_contact or collision_only" >&2
      exit 2
    fi
    if [[ "${WAYPOINTS}" != 96 && "${WAYPOINTS}" != 500 &&
          "${WAYPOINTS}" != 1000 ]]; then
      echo "lightweight-collision-only waypoints must be 96, 500, or 1000" >&2
      exit 2
    fi
    TARGET="perf_collision_strategy"
    BINARY_REL="perf_collision_strategy"
    ENVELOPE_CELL="lightweight_collision_only"
    ;;
  policy-support-contact)
    case "${ARM}" in
      signed_penetration_selective|binary_contact|bounded_penetration|collision_only) ;;
      *)
        echo "policy-support-contact requires a supported --arm" >&2
        exit 2
        ;;
    esac
    if [[ "${WAYPOINTS}" != 96 && "${WAYPOINTS}" != 500 &&
          "${WAYPOINTS}" != 1000 ]]; then
      echo "policy-support-contact waypoints must be 96, 500, or 1000" >&2
      exit 2
    fi
    if [[ "${WORKERS}" != 1 && "${WORKERS}" != 4 ]]; then
      echo "policy-support-contact workers must be 1 or 4" >&2
      exit 2
    fi
    TARGET="perf_policy_support_contact_qualification"
    BINARY_REL="manyforge_integration_tests/perf_policy_support_contact_qualification"
    ENVELOPE_CELL="policy_support_contact"
    ;;
  precise-reference-parity)
    if [[ -n "${ARM}" ]]; then
      echo "precise-reference-parity accepts no --arm" >&2
      exit 2
    fi
    TARGET="perf_policy_support_contact_qualification"
    BINARY_REL="manyforge_integration_tests/perf_policy_support_contact_qualification"
    ENVELOPE_CELL="precise_reference_parity"
    ;;
  contact-grasp)
    if [[ -n "${ARM}" || ("${WAYPOINTS}" != 96 && "${WAYPOINTS}" != 500) ]]; then
      echo "contact-grasp accepts waypoints 96 or 500 and no --arm" >&2
      exit 2
    fi
    if [[ "${WORKERS}" != 1 && "${WORKERS}" != 4 ]]; then
      echo "contact-grasp workers must be 1 or 4" >&2
      exit 2
    fi
    TARGET="perf_contact_grasp_validation_nonregression"
    BINARY_REL="manyforge_integration_tests/perf_contact_grasp_validation_nonregression"
    ENVELOPE_CELL="contact_grasp"
    ;;
  free-space)
    if [[ -n "${ARM}" ]]; then
      echo "free-space accepts no --arm" >&2
      exit 2
    fi
    TARGET="perf_collision_engine_qualification"
    BINARY_REL="manyforge_integration_tests/perf_collision_engine_qualification"
    ENVELOPE_CELL="free_space"
    ;;
  *)
    echo "unknown --cell: ${CELL}" >&2
    usage
    exit 2
    ;;
esac

SPECS_ROOT="${MANYFORGE_SPECS_ROOT:-}"
EXPECTED_SOURCE_FINGERPRINT_SHA256="${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256:-}"
EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256="${MANYFORGE_EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256:-}"
EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256="${MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256:-}"
if [[ -z "${SPECS_ROOT}" || ! -d "${SPECS_ROOT}" ||
      ! "${EXPECTED_SOURCE_FINGERPRINT_SHA256}" =~ ^[0-9a-f]{64}$ ||
      ! "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" =~ ^[0-9a-f]{64}$ ||
      ! "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}" =~ ^[0-9a-f]{64}$ ]]; then
  echo "MANYFORGE_SPECS_ROOT and exact expected source/full-Specs/Specs-contract fingerprints are required" >&2
  exit 2
fi
SPECS_ROOT="$(realpath "${SPECS_ROOT}")"
CORPUS_FILE="${REPO_ROOT}/benchmarks/roboplan_05/corpus/corpus-v1.json"
WRAPPER="${REPO_ROOT}/scripts/qualification/wrap_step18_proof_artifact.py"
if [[ ! -f "${CORPUS_FILE}" || ! -f "${WRAPPER}" ||
      ! -f "${SOURCE_IDENTITY_HELPER}" ]]; then
  echo "proof corpus, wrapper, or source identity helper is missing" >&2
  exit 1
fi
CORPUS_SHA256="$(sha256sum "${CORPUS_FILE}" | awk '{print $1}')"
RUNNER_SCRIPT_SHA256="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"
WRAPPER_SCRIPT_SHA256="$(sha256sum "${WRAPPER}" | awk '{print $1}')"
SOURCE_IDENTITY_HELPER_SHA256="$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')"

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
    raise SystemExit(f"refusing to overwrite proof artifact: {destination}")
if within(destination, specs):
    raise SystemExit("proof artifact output must not be inside the Specs checkout")
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

SOURCE_IDENTITY_BEFORE_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${REPO_ROOT}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_BEFORE_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_ROOT}" \
    --include-specs-contract-identity \
    --expect-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
    --expect-contract-fingerprint-sha256 \
      "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
)"
mapfile -t SPECS_IDENTITIES_BEFORE_BUILD < <(
  python3 -c 'import json, sys
value = json.load(sys.stdin)
print(json.dumps(value["source_identity"], sort_keys=True, separators=(",", ":")))
print(json.dumps(value["specs_contract_identity"], sort_keys=True, separators=(",", ":")))' \
    <<<"${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}"
)
if [[ ${#SPECS_IDENTITIES_BEFORE_BUILD[@]} -ne 2 ]]; then
  echo "unable to resolve full Specs and Specs-contract identities" >&2
  exit 1
fi
SPECS_SOURCE_IDENTITY_BEFORE_BUILD="${SPECS_IDENTITIES_BEFORE_BUILD[0]}"
SPECS_CONTRACT_IDENTITY_BEFORE_BUILD="${SPECS_IDENTITIES_BEFORE_BUILD[1]}"
IMAGE_ID="$(resolve_dev_image_id)"
if [[ ! "${IMAGE_ID}" =~ ^[0-9a-f]{64}$ ]]; then
  echo "unable to resolve exact Compose dev image id" >&2
  exit 1
fi
EXPECTED_IMAGE_ID="${MANYFORGE_QUAL_IMAGE_ID:-}"
EXPECTED_IMAGE_ID="${EXPECTED_IMAGE_ID#sha256:}"
if [[ -n "${EXPECTED_IMAGE_ID}" && "${EXPECTED_IMAGE_ID}" != "${IMAGE_ID}" ]]; then
  echo "MANYFORGE_QUAL_IMAGE_ID differs from the actual Compose dev image" >&2
  exit 1
fi
COMPILER_VERSION="$(
  docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev \
    bash -lc 'c++ -dumpfullversion -dumpversion'
)"
if [[ -z "${COMPILER_VERSION}" || "${COMPILER_VERSION}" == *$'\n'* ]]; then
  echo "unable to resolve exact compiler version from the Compose dev image" >&2
  exit 1
fi
PROCESS_INSTANCE_ID="$(python3 -c 'import uuid; print(uuid.uuid4())')"
RUN_ID="${MANYFORGE_QUAL_RUN_ID:-$(date -u +%Y%m%dT%H%M%S%NZ)-${CELL}-${ARM:-current}-${PROCESS_INSTANCE_ID}}"

BUILD_DIR="$(realpath -m "${BUILD_DIR}")"
case "${BUILD_DIR}" in
  "${REPO_ROOT}/build/"*) ;;
  *) echo "proof build directory must be below ${REPO_ROOT}/build" >&2; exit 2 ;;
esac
BUILD_CONTAINER="/workspace/${BUILD_DIR#"${REPO_ROOT}/"}"
BUILD_SCRIPT="
set -euo pipefail
export CMAKE_PREFIX_PATH=/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix:\${CMAKE_PREFIX_PATH:-}
export LD_LIBRARY_PATH=/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix/lib:\${LD_LIBRARY_PATH:-}
taskset -c '${CPUSET}' cmake -S /workspace/manyforge_roboplan -B '${BUILD_CONTAINER}' -DCMAKE_BUILD_TYPE='${BUILD_TYPE}' -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS='${MATCHED_TOOLCHAIN_CXX_FLAGS}'
taskset -c '${CPUSET}' cmake --build '${BUILD_CONTAINER}' --target '${TARGET}' --parallel '${BUILD_JOBS}'
"
BUILD_COMMAND=(docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev bash -lc "${BUILD_SCRIPT}")
"${BUILD_COMMAND[@]}" >&2

SOURCE_IDENTITY_AFTER_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${REPO_ROOT}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_AFTER_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_ROOT}" \
    --include-specs-contract-identity \
    --expect-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
    --expect-contract-fingerprint-sha256 \
      "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
)"
IMAGE_ID_AFTER_BUILD="$(resolve_dev_image_id)"
COMPILER_VERSION_AFTER_BUILD="$(
  docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev \
    bash -lc 'c++ -dumpfullversion -dumpversion'
)"
if [[ "${SOURCE_IDENTITY_AFTER_BUILD}" != "${SOURCE_IDENTITY_BEFORE_BUILD}" ||
      "${SPECS_IDENTITY_BUNDLE_AFTER_BUILD}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}" ||
      "${IMAGE_ID_AFTER_BUILD}" != "${IMAGE_ID}" ||
      "${COMPILER_VERSION_AFTER_BUILD}" != "${COMPILER_VERSION}" ]]; then
  echo "source, Specs, image, or compiler identity changed during proof build" >&2
  exit 1
fi

BINARY="${BUILD_DIR}/${BINARY_REL}"
if [[ ! -x "${BINARY}" ]]; then
  echo "proof binary is missing after build: ${BINARY}" >&2
  exit 1
fi
BINARY_SHA256="$(sha256sum "${BINARY}" | awk '{print $1}')"
CONTAINER_BINARY="/workspace/${BINARY#"${REPO_ROOT}/"}"
if [[ "${CELL}" == "lightweight-collision-only" ]]; then
  FORWARD_ARGS=(
    --repo-root /workspace
    --corpus /workspace/benchmarks/roboplan_05/corpus/corpus-v1.json
    --fixture ur5_support_contact_release
    --query-class "${ARM}"
    --warmup "${WARMUP}"
    --queries "${MEASURED}"
    --batch-size "${WAYPOINTS}"
  )
elif [[ "${CELL}" == "policy-support-contact" ]]; then
  FORWARD_ARGS=(
    --arm "${ARM}"
    --waypoints "${WAYPOINTS}"
    --workers "${WORKERS}"
    --warmup-runs "${WARMUP}"
    --measured-runs "${MEASURED}"
  )
elif [[ "${CELL}" == "precise-reference-parity" ]]; then
  FORWARD_ARGS=(
    --precise-reference-only
    --warmup-runs "${WARMUP}"
    --measured-runs "${MEASURED}"
  )
elif [[ "${CELL}" == "contact-grasp" ]]; then
  FORWARD_ARGS=(
    --waypoints "${WAYPOINTS}"
    --workers "${WORKERS}"
    --warmup-runs "${WARMUP}"
    --measured-runs "${MEASURED}"
  )
else
  FORWARD_ARGS=(
    --warmup-runs "${WARMUP}"
    --measured-runs "${MEASURED}"
  )
fi

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
  -e "MANYFORGE_SOURCE_ROLE=current_candidate"
  -e "MANYFORGE_CLEAN_CHECKOUT_MANIFEST_SHA256="
  -e "MANYFORGE_QUALIFICATION_PATCH_SHA256="
  dev bash -lc 'set -euo pipefail
   export CMAKE_PREFIX_PATH="/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix:${CMAKE_PREFIX_PATH:-}"
   export LD_LIBRARY_PATH="/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix/lib:${LD_LIBRARY_PATH:-}"
   exec "$@"' current-proof "${COMMAND[@]}")
"${OUTER_COMMAND[@]}" >"${RAW_OUTPUT}"

SOURCE_IDENTITY_AFTER_MEASUREMENT="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${REPO_ROOT}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_AFTER_MEASUREMENT="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_ROOT}" \
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
if [[ "${SOURCE_IDENTITY_AFTER_MEASUREMENT}" != "${SOURCE_IDENTITY_BEFORE_BUILD}" ||
      "${SPECS_IDENTITY_BUNDLE_AFTER_MEASUREMENT}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}" ||
      "${IMAGE_ID_AFTER_MEASUREMENT}" != "${IMAGE_ID}" ||
      "${COMPILER_VERSION_AFTER_MEASUREMENT}" != "${COMPILER_VERSION}" ]]; then
  echo "source, Specs, image, or compiler identity changed during proof measurement" >&2
  exit 1
fi

argv_json() {
  python3 -c 'import json, sys; print(json.dumps(sys.argv[1:], separators=(",", ":")))' "$@"
}
RUNNER_COMMAND_JSON="$(argv_json "${ORIGINAL_RUNNER_ARGV[@]}")"
BUILD_COMMAND_JSON="$(argv_json "${BUILD_COMMAND[@]}")"
OUTER_COMMAND_JSON="$(argv_json "${OUTER_COMMAND[@]}")"
RUNNER_ENVIRONMENT_JSON="$(python3 - \
  "${CPUSET}" "${IMAGE_ID}" "${MATCHED_TOOLCHAIN_CXX_FLAGS}" \
  "${COMPILER_VERSION}" "${BUILD_TYPE}" "${CPU_GOVERNOR_SET}" \
  "${CPU_FREQUENCY_DRIVER_SET}" "${HOST_KERNEL_ID}" "${CORPUS_SHA256}" \
  "${BUILD_JOBS}" "${REPO_ROOT}" "${SPECS_ROOT}" \
  "${SOURCE_IDENTITY_HELPER_SHA256}" "${WARMUP}" "${MEASURED}" <<'PY'
import json
import sys
keys = (
    "requested_cpu_affinity", "image_id", "compiler_flags",
    "compiler", "build_type", "cpu_governor_set",
    "cpu_frequency_driver_set", "kernel_id",
    "policy_support_corpus_sha256", "build_jobs", "source_root",
    "specs_root", "source_identity_helper_sha256", "warmup_runs",
    "measured_runs",
)
values = dict(zip(keys, sys.argv[1:]))
for name in ("build_jobs", "warmup_runs", "measured_runs"):
    values[name] = int(values[name])
print(json.dumps(values, sort_keys=True, separators=(",", ":")))
PY
)"
WRAP_COMMAND=(python3 "${WRAPPER}"
  --input "${RAW_OUTPUT}"
  --cell "${ENVELOPE_CELL}"
  --run-id "${RUN_ID}"
  --process-instance-id "${PROCESS_INSTANCE_ID}"
  --source-role current_candidate
  --source-identity-json "${SOURCE_IDENTITY_BEFORE_BUILD}"
  --specs-source-identity-json "${SPECS_SOURCE_IDENTITY_BEFORE_BUILD}"
  --specs-contract-identity-json "${SPECS_CONTRACT_IDENTITY_BEFORE_BUILD}"
  --expected-source-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
  --expected-specs-source-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}"
  --expected-specs-contract-fingerprint-sha256 "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
  --image-id "${IMAGE_ID}"
  --binary-sha256 "${BINARY_SHA256}"
  --requested-cpu-affinity "${CPUSET}"
  --warmup-runs "${WARMUP}"
  --measured-runs "${MEASURED}"
  --checkout-base-clean 0
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
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${REPO_ROOT}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_AFTER_WRAPPING="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_ROOT}" \
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
if [[ "${SOURCE_IDENTITY_AFTER_WRAPPING}" != "${SOURCE_IDENTITY_BEFORE_BUILD}" ||
      "${SPECS_IDENTITY_BUNDLE_AFTER_WRAPPING}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}" ||
      "${IMAGE_ID_AFTER_WRAPPING}" != "${IMAGE_ID}" ||
      "${COMPILER_VERSION_AFTER_WRAPPING}" != "${COMPILER_VERSION}" ||
      "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" != "${RUNNER_SCRIPT_SHA256}" ||
      "$(sha256sum "${WRAPPER}" | awk '{print $1}')" != "${WRAPPER_SCRIPT_SHA256}" ||
      "$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')" != "${SOURCE_IDENTITY_HELPER_SHA256}" ]]; then
  echo "source, Specs, image, or compiler identity changed during proof wrapping" >&2
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
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${REPO_ROOT}" \
    --expect-fingerprint-sha256 "${EXPECTED_SOURCE_FINGERPRINT_SHA256}"
)"
SPECS_IDENTITY_BUNDLE_FINAL="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_ROOT}" \
    --include-specs-contract-identity \
    --expect-fingerprint-sha256 "${EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
    --expect-contract-fingerprint-sha256 \
      "${EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}"
)"
if [[ "${SOURCE_IDENTITY_FINAL}" != "${SOURCE_IDENTITY_BEFORE_BUILD}" ||
      "${SPECS_IDENTITY_BUNDLE_FINAL}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}" ||
      "$(resolve_dev_image_id)" != "${IMAGE_ID}" ||
      "$(docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev bash -lc 'c++ -dumpfullversion -dumpversion')" != "${COMPILER_VERSION}" ||
      "$(sha256sum "${BINARY}" | awk '{print $1}')" != "${BINARY_SHA256}" ||
      "$(sha256sum "${CORPUS_FILE}" | awk '{print $1}')" != "${CORPUS_SHA256}" ||
      "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" != "${RUNNER_SCRIPT_SHA256}" ||
      "$(sha256sum "${WRAPPER}" | awk '{print $1}')" != "${WRAPPER_SCRIPT_SHA256}" ||
      "$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')" != "${SOURCE_IDENTITY_HELPER_SHA256}" ]]; then
  echo "source, Specs, binary, image, compiler, or qualification input changed before proof archival completed" >&2
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
