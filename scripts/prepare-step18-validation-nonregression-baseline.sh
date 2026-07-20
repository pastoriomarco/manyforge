#!/usr/bin/env bash
# Build pristine Step 18 contact-grasp and free-space timing binaries from the
# exact pre-change commits plus one archived qualification-only source overlay.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE_SOURCE="643fe9c399182419d38d4478750d7e6f692dc9ad"
BASELINE_SPECS="5e50a4872bbb9a2167c6bc14b9b5963061f2f09e"
LANE_DIR="${MANYFORGE_STEP18_NONREGRESSION_BASELINE_DIR:-${REPO_ROOT}/build/step18-validation-nonregression-baseline-lane}"
SPECS_REPOSITORY="${MANYFORGE_BASELINE_SPECS_REPOSITORY:-}"
CPUSET="${MANYFORGE_STEP18_NONREGRESSION_CPUSET:-}"
BUILD_JOBS="${MANYFORGE_BUILD_JOBS:-2}"
BASELINE_TOOLCHAIN_CXX_FLAGS="-Wno-error=maybe-uninitialized"
HARNESS_SOURCE="${REPO_ROOT}/tests/cpp/perf/perf_contact_grasp_validation_nonregression.cpp"
POLICY_HARNESS_SOURCE="${REPO_ROOT}/tests/cpp/perf/perf_policy_support_contact_qualification.cpp"
CMAKE_PATCH="${REPO_ROOT}/scripts/qualification/baseline/step18_validation_nonregression_cmake.patch"
POLICY_COMPAT_PATCH="${REPO_ROOT}/scripts/qualification/baseline/step18_policy_support_baseline_compat.patch"
FREE_SPACE_PROFILE_PATCH="${REPO_ROOT}/scripts/qualification/baseline/step18_free_space_profile.patch"
FREE_SPACE_QUERY_PROFILE_PATCH="${REPO_ROOT}/scripts/qualification/baseline/step18_free_space_query_profile.patch"
SOURCE_IDENTITY_HELPER="${REPO_ROOT}/scripts/qualification/source_identity.py"
CORPUS_FILE="${REPO_ROOT}/benchmarks/roboplan_05/corpus/corpus-v1.json"

if [[ -z "${SPECS_REPOSITORY}" ]]; then
  echo "MANYFORGE_BASELINE_SPECS_REPOSITORY is required" >&2
  exit 2
fi
if [[ -z "${CPUSET}" || ! "${CPUSET}" =~ ^[0-9,-]+$ ]]; then
  echo "MANYFORGE_STEP18_NONREGRESSION_CPUSET must be explicit" >&2
  exit 2
fi
CPUSET="$(python3 - "${CPUSET}" <<'PY'
import sys
cpus = set()
for part in sys.argv[1].split(","):
    if "-" in part:
        first, last = (int(value) for value in part.split("-", 1))
        if first > last:
            raise SystemExit("invalid descending CPU affinity range")
        cpus.update(range(first, last + 1))
    else:
        cpus.add(int(part))
ordered = sorted(cpus)
if len(ordered) != 4:
    raise SystemExit("Step 18 formal CPU affinity must select exactly four cores")
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
PY
)"
if [[ ! "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "MANYFORGE_BUILD_JOBS must be a positive integer" >&2
  exit 2
fi
if [[ ! -f "${HARNESS_SOURCE}" || ! -f "${POLICY_HARNESS_SOURCE}" ||
      ! -f "${CORPUS_FILE}" ||
      ! -f "${CMAKE_PATCH}" || ! -f "${POLICY_COMPAT_PATCH}" ||
      ! -f "${FREE_SPACE_PROFILE_PATCH}" ||
      ! -f "${FREE_SPACE_QUERY_PROFILE_PATCH}" ||
      ! -f "${SOURCE_IDENTITY_HELPER}" ]]; then
  echo "qualification overlay input is missing" >&2
  exit 1
fi
PREPARE_SCRIPT_SHA256="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"
SOURCE_IDENTITY_HELPER_SHA256="$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')"
HARNESS_SOURCE_SHA256="$(sha256sum "${HARNESS_SOURCE}" | awk '{print $1}')"
POLICY_HARNESS_SOURCE_SHA256="$(sha256sum "${POLICY_HARNESS_SOURCE}" | awk '{print $1}')"
CMAKE_PATCH_SHA256="$(sha256sum "${CMAKE_PATCH}" | awk '{print $1}')"
POLICY_COMPAT_PATCH_SHA256="$(sha256sum "${POLICY_COMPAT_PATCH}" | awk '{print $1}')"
FREE_SPACE_PROFILE_PATCH_SHA256="$(sha256sum "${FREE_SPACE_PROFILE_PATCH}" | awk '{print $1}')"
FREE_SPACE_QUERY_PROFILE_PATCH_SHA256="$(sha256sum "${FREE_SPACE_QUERY_PROFILE_PATCH}" | awk '{print $1}')"
CORPUS_SHA256="$(sha256sum "${CORPUS_FILE}" | awk '{print $1}')"

LANE_DIR="$(realpath -m "${LANE_DIR}")"
case "${LANE_DIR}" in
  "${REPO_ROOT}/build/"*) ;;
  *)
    echo "baseline lane must be a dedicated path below ${REPO_ROOT}/build" >&2
    exit 2
    ;;
esac
if [[ -e "${LANE_DIR}" ]]; then
  echo "baseline lane already exists; refusing to overwrite: ${LANE_DIR}" >&2
  exit 2
fi
if [[ "$(git -C "${REPO_ROOT}" rev-parse "${BASELINE_SOURCE}^{commit}")" != "${BASELINE_SOURCE}" ]]; then
  echo "ManyForge baseline commit is unavailable" >&2
  exit 1
fi
if [[ "$(git -C "${SPECS_REPOSITORY}" rev-parse "${BASELINE_SPECS}^{commit}")" != "${BASELINE_SPECS}" ]]; then
  echo "Specs baseline commit is unavailable" >&2
  exit 1
fi

mkdir -p "${LANE_DIR}/artifacts"
SOURCE_TREE="${LANE_DIR}/source"
SPECS_TREE="${LANE_DIR}/specs-source"
BUILD_DIR="${LANE_DIR}/build"
ARTIFACT_DIR="${LANE_DIR}/artifacts"

git -C "${REPO_ROOT}" worktree add --detach "${SOURCE_TREE}" "${BASELINE_SOURCE}"
git -C "${SPECS_REPOSITORY}" worktree add --detach "${SPECS_TREE}" "${BASELINE_SPECS}"

SOURCE_CLEAN_IDENTITY="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}"
)"
SPECS_CLEAN_BUNDLE="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" \
    --include-specs-contract-identity
)"
mapfile -t SPECS_CLEAN_IDENTITIES < <(python3 - "${SPECS_CLEAN_BUNDLE}" <<'PY'
import json
import sys

value = json.loads(sys.argv[1])
for field in ("source_identity", "specs_contract_identity"):
    print(json.dumps(value[field], sort_keys=True, separators=(",", ":")))
PY
)
if [[ ${#SPECS_CLEAN_IDENTITIES[@]} -ne 2 ]]; then
  echo "failed to capture clean Specs source and contract identities" >&2
  exit 1
fi
SPECS_CLEAN_IDENTITY="${SPECS_CLEAN_IDENTITIES[0]}"
SPECS_CLEAN_CONTRACT_IDENTITY="${SPECS_CLEAN_IDENTITIES[1]}"
python3 - "${SOURCE_CLEAN_IDENTITY}" "${SPECS_CLEAN_IDENTITY}" \
  "${BASELINE_SOURCE}" "${BASELINE_SPECS}" <<'PY'
import json
import sys

source, specs = (json.loads(value) for value in sys.argv[1:3])
source_commit, specs_commit = sys.argv[3:]
if source["head_commit"] != source_commit or source["dirty"]:
    raise SystemExit("ManyForge detached checkout is not the exact clean base")
if specs["head_commit"] != specs_commit or specs["dirty"]:
    raise SystemExit("Specs detached checkout is not the exact clean base")
PY

CLEAN_MANIFEST="${ARTIFACT_DIR}/clean-checkout-manifest.json"
python3 - "${CLEAN_MANIFEST}" "${SOURCE_CLEAN_IDENTITY}" \
  "${SPECS_CLEAN_IDENTITY}" "${SPECS_CLEAN_CONTRACT_IDENTITY}" <<'PY'
import json
import sys
from pathlib import Path

(
    output,
    source_identity_json,
    specs_source_identity_json,
    specs_contract_identity_json,
) = sys.argv[1:]
manifest = {
    "schema_version": "manyforge.step18.validation_nonregression_clean_checkout.v2",
    "captured_before_patch": True,
    "repositories": {
        "manyforge": {"source_identity": json.loads(source_identity_json)},
        "specs": {
            "source_identity": json.loads(specs_source_identity_json),
            "specs_contract_identity": json.loads(specs_contract_identity_json),
        },
    },
}
Path(output).write_text(
    json.dumps(manifest, indent=2, sort_keys=True, allow_nan=False) + "\n",
    encoding="utf-8",
)
PY

install -D -m 0644 "${HARNESS_SOURCE}" \
  "${SOURCE_TREE}/tests/cpp/perf/perf_contact_grasp_validation_nonregression.cpp"
install -D -m 0644 "${POLICY_HARNESS_SOURCE}" \
  "${SOURCE_TREE}/tests/cpp/perf/perf_policy_support_contact_qualification.cpp"
git -C "${SOURCE_TREE}" add -N -- \
  tests/cpp/perf/perf_contact_grasp_validation_nonregression.cpp \
  tests/cpp/perf/perf_policy_support_contact_qualification.cpp
git -C "${SOURCE_TREE}" apply "${POLICY_COMPAT_PATCH}"
git -C "${SOURCE_TREE}" apply "${FREE_SPACE_PROFILE_PATCH}"
git -C "${SOURCE_TREE}" apply "${FREE_SPACE_QUERY_PROFILE_PATCH}"
git -C "${SOURCE_TREE}" apply "${CMAKE_PATCH}"

QUALIFICATION_PATCH="${ARTIFACT_DIR}/qualification-overlay.patch"
git -C "${SOURCE_TREE}" diff --binary --output="${QUALIFICATION_PATCH}" HEAD -- \
  tests/cpp/CMakeLists.txt \
  tests/cpp/perf/perf_contact_grasp_validation_nonregression.cpp \
  tests/cpp/perf/perf_collision_engine_qualification.cpp \
  tests/cpp/perf/perf_policy_support_contact_qualification.cpp
if [[ "$(git -C "${SOURCE_TREE}" diff --name-only HEAD --)" != \
      $'tests/cpp/CMakeLists.txt\ntests/cpp/perf/perf_collision_engine_qualification.cpp\ntests/cpp/perf/perf_contact_grasp_validation_nonregression.cpp\ntests/cpp/perf/perf_policy_support_contact_qualification.cpp' ]]; then
  echo "baseline worktree contains a change outside qualification-only scope" >&2
  exit 1
fi

SOURCE_IDENTITY_BEFORE_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}"
)"
SPECS_IDENTITY_BUNDLE_BEFORE_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" \
    --include-specs-contract-identity
)"
mapfile -t SPECS_IDENTITIES_BEFORE_BUILD < <(python3 - \
  "${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}" <<'PY'
import json
import sys

value = json.loads(sys.argv[1])
for field in ("source_identity", "specs_contract_identity"):
    print(json.dumps(value[field], sort_keys=True, separators=(",", ":")))
PY
)
if [[ ${#SPECS_IDENTITIES_BEFORE_BUILD[@]} -ne 2 ]]; then
  echo "failed to capture baseline Specs identities before build" >&2
  exit 1
fi
SPECS_IDENTITY_BEFORE_BUILD="${SPECS_IDENTITIES_BEFORE_BUILD[0]}"
SPECS_CONTRACT_IDENTITY_BEFORE_BUILD="${SPECS_IDENTITIES_BEFORE_BUILD[1]}"

relative_to_workspace() {
  printf '/workspace/%s' "${1#"${REPO_ROOT}/"}"
}

SOURCE_CONTAINER="$(relative_to_workspace "${SOURCE_TREE}")"
BUILD_CONTAINER="$(relative_to_workspace "${BUILD_DIR}")"

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

BUILD_SCRIPT="
set -euo pipefail
export CMAKE_PREFIX_PATH=/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix:\${CMAKE_PREFIX_PATH:-}
export LD_LIBRARY_PATH=/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix/lib:\${LD_LIBRARY_PATH:-}
taskset -c '${CPUSET}' cmake -S '${SOURCE_CONTAINER}/manyforge_roboplan' -B '${BUILD_CONTAINER}' -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS='${BASELINE_TOOLCHAIN_CXX_FLAGS}'
taskset -c '${CPUSET}' cmake --build '${BUILD_CONTAINER}' --target perf_contact_grasp_validation_baseline perf_policy_support_contact_baseline perf_collision_engine_qualification --parallel '${BUILD_JOBS}'
"
BUILD_COMMAND=(docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev bash -lc "${BUILD_SCRIPT}")
"${BUILD_COMMAND[@]}"

SOURCE_IDENTITY_AFTER_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}"
)"
SPECS_IDENTITY_BUNDLE_AFTER_BUILD="$(
  python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" \
    --include-specs-contract-identity
)"
IMAGE_ID_AFTER_BUILD="$(resolve_dev_image_id)"
if [[ "${SOURCE_IDENTITY_AFTER_BUILD}" != "${SOURCE_IDENTITY_BEFORE_BUILD}" ||
      "${SPECS_IDENTITY_BUNDLE_AFTER_BUILD}" != "${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}" ||
      "${IMAGE_ID_AFTER_BUILD}" != "${IMAGE_ID}" ]]; then
  echo "source, Specs, or Compose image identity changed during baseline build" >&2
  exit 1
fi

CONTACT_BINARY="${BUILD_DIR}/manyforge_integration_tests/perf_contact_grasp_validation_baseline"
POLICY_BINARY="${BUILD_DIR}/manyforge_integration_tests/perf_policy_support_contact_baseline"
FREE_SPACE_BINARY="${BUILD_DIR}/manyforge_integration_tests/perf_collision_engine_qualification"
if [[ ! -x "${CONTACT_BINARY}" || ! -x "${POLICY_BINARY}" ||
      ! -x "${FREE_SPACE_BINARY}" ]]; then
  echo "baseline qualification binary is missing after build" >&2
  exit 1
fi
CONTACT_BINARY_SHA256="$(sha256sum "${CONTACT_BINARY}" | awk '{print $1}')"
POLICY_BINARY_SHA256="$(sha256sum "${POLICY_BINARY}" | awk '{print $1}')"
FREE_SPACE_BINARY_SHA256="$(sha256sum "${FREE_SPACE_BINARY}" | awk '{print $1}')"
CLEAN_MANIFEST_SHA256="$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')"
QUALIFICATION_PATCH_SHA256="$(sha256sum "${QUALIFICATION_PATCH}" | awk '{print $1}')"
COMPILER_VERSION="$(
  docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev \
    bash -lc 'c++ -dumpfullversion -dumpversion'
)"
if [[ -z "${COMPILER_VERSION}" || "${COMPILER_VERSION}" == *$'\n'* ]]; then
  echo "unable to resolve exact compiler version from the Compose dev image" >&2
  exit 1
fi

METADATA="${ARTIFACT_DIR}/lane-metadata.json"
python3 - "${METADATA}" "${SOURCE_IDENTITY_BEFORE_BUILD}" \
  "${SPECS_IDENTITY_BEFORE_BUILD}" \
  "${SPECS_CONTRACT_IDENTITY_BEFORE_BUILD}" \
  "${SOURCE_TREE}" "${SPECS_TREE}" \
  "${IMAGE_ID}" "${CPUSET}" "${BASELINE_TOOLCHAIN_CXX_FLAGS}" \
  "${COMPILER_VERSION}" "Release" "${BUILD_JOBS}" \
  "${CLEAN_MANIFEST_SHA256}" \
  "${QUALIFICATION_PATCH_SHA256}" \
  "${CONTACT_BINARY_SHA256}" \
  "${POLICY_BINARY_SHA256}" \
  "${FREE_SPACE_BINARY_SHA256}" \
  "${CORPUS_SHA256}" \
  "$(python3 -c 'import json, sys; print(json.dumps(sys.argv[1:], separators=(",", ":")))' "${BUILD_COMMAND[@]}")" \
  "${PREPARE_SCRIPT_SHA256}" \
  "${POLICY_COMPAT_PATCH_SHA256}" \
  "${FREE_SPACE_PROFILE_PATCH_SHA256}" \
  "${FREE_SPACE_QUERY_PROFILE_PATCH_SHA256}" \
  "${SOURCE_IDENTITY_HELPER_SHA256}" <<'PY'
import json
import sys
from pathlib import Path

(
    output,
    source_identity_json,
    specs_source_identity_json,
    specs_contract_identity_json,
    source_root,
    specs_root,
    image_id,
    cpuset,
    cxx_flags,
    compiler,
    build_type,
    build_jobs,
    clean_manifest_hash,
    qualification_patch_hash,
    contact_binary_hash,
    policy_binary_hash,
    free_space_binary_hash,
    corpus_hash,
    build_command_json,
    prepare_script_hash,
    policy_compat_patch_hash,
    free_space_profile_patch_hash,
    free_space_query_profile_patch_hash,
    source_identity_helper_hash,
) = sys.argv[1:]
source_identity = json.loads(source_identity_json)
specs_source_identity = json.loads(specs_source_identity_json)
specs_contract_identity = json.loads(specs_contract_identity_json)
metadata = {
    "schema_version": "manyforge.step18.validation_nonregression_baseline_lane.v2",
    "source_identity": source_identity,
    "specs_source_identity": specs_source_identity,
    "specs_contract_identity": specs_contract_identity,
    "expected_source_fingerprint_sha256": source_identity["fingerprint_sha256"],
    "expected_specs_source_fingerprint_sha256": specs_source_identity["fingerprint_sha256"],
    "expected_specs_contract_fingerprint_sha256": specs_contract_identity["contract_fingerprint_sha256"],
    "source_root": source_root,
    "specs_root": specs_root,
    "image_id": image_id,
    "requested_cpu_affinity": cpuset,
    "baseline_toolchain_cxx_flags": cxx_flags,
    "compiler": compiler,
    "build_type": build_type,
    "build_jobs": int(build_jobs),
    "clean_checkout_manifest_sha256": clean_manifest_hash,
    "qualification_patch_sha256": qualification_patch_hash,
    "contact_grasp_binary_sha256": contact_binary_hash,
    "policy_support_binary_sha256": policy_binary_hash,
    "free_space_binary_sha256": free_space_binary_hash,
    "policy_support_corpus_sha256": corpus_hash,
    "build_command_argv": json.loads(build_command_json),
    "prepare_script_sha256": prepare_script_hash,
    "policy_compat_patch_sha256": policy_compat_patch_hash,
    "free_space_profile_patch_sha256": free_space_profile_patch_hash,
    "free_space_query_profile_patch_sha256": free_space_query_profile_patch_hash,
    "source_identity_helper_sha256": source_identity_helper_hash,
}
Path(output).write_text(
    json.dumps(metadata, indent=2, sort_keys=True, allow_nan=False) + "\n",
    encoding="utf-8",
)
PY

if [[ "$(python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SOURCE_TREE}")" != "${SOURCE_IDENTITY_BEFORE_BUILD}" ||
      "$(python3 "${SOURCE_IDENTITY_HELPER}" --repo-root "${SPECS_TREE}" --include-specs-contract-identity)" != "${SPECS_IDENTITY_BUNDLE_BEFORE_BUILD}" ||
      "$(resolve_dev_image_id)" != "${IMAGE_ID}" ||
      "$(sha256sum "${CONTACT_BINARY}" | awk '{print $1}')" != "${CONTACT_BINARY_SHA256}" ||
      "$(sha256sum "${POLICY_BINARY}" | awk '{print $1}')" != "${POLICY_BINARY_SHA256}" ||
      "$(sha256sum "${FREE_SPACE_BINARY}" | awk '{print $1}')" != "${FREE_SPACE_BINARY_SHA256}" ||
      "$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')" != "${CLEAN_MANIFEST_SHA256}" ||
      "$(sha256sum "${QUALIFICATION_PATCH}" | awk '{print $1}')" != "${QUALIFICATION_PATCH_SHA256}" ||
      "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" != "${PREPARE_SCRIPT_SHA256}" ||
      "$(sha256sum "${SOURCE_IDENTITY_HELPER}" | awk '{print $1}')" != "${SOURCE_IDENTITY_HELPER_SHA256}" ||
      "$(sha256sum "${HARNESS_SOURCE}" | awk '{print $1}')" != "${HARNESS_SOURCE_SHA256}" ||
      "$(sha256sum "${POLICY_HARNESS_SOURCE}" | awk '{print $1}')" != "${POLICY_HARNESS_SOURCE_SHA256}" ||
      "$(sha256sum "${CMAKE_PATCH}" | awk '{print $1}')" != "${CMAKE_PATCH_SHA256}" ||
      "$(sha256sum "${POLICY_COMPAT_PATCH}" | awk '{print $1}')" != "${POLICY_COMPAT_PATCH_SHA256}" ||
      "$(sha256sum "${FREE_SPACE_PROFILE_PATCH}" | awk '{print $1}')" != "${FREE_SPACE_PROFILE_PATCH_SHA256}" ||
      "$(sha256sum "${FREE_SPACE_QUERY_PROFILE_PATCH}" | awk '{print $1}')" != "${FREE_SPACE_QUERY_PROFILE_PATCH_SHA256}" ||
      "$(sha256sum "${CORPUS_FILE}" | awk '{print $1}')" != "${CORPUS_SHA256}" ]]; then
  echo "baseline source, Specs, binary, image, or preparation input changed before metadata completed" >&2
  exit 1
fi

echo "prepared Step 18 validation non-regression baseline: ${LANE_DIR}"
echo "metadata: ${METADATA}"
