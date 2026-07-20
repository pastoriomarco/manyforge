#!/usr/bin/env bash
# Prepare genuine detached Step 18 timing and observation baseline lanes with
# exact v1 source identities before and after the qualification-only overlays.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE_SOURCE="643fe9c399182419d38d4478750d7e6f692dc9ad"
BASELINE_SPECS="5e50a4872bbb9a2167c6bc14b9b5963061f2f09e"
LANE_DIR="${MANYFORGE_BASELINE_LANE_DIR:-${REPO_ROOT}/build/step18-runtime-baseline-lane}"
SPECS_REPOSITORY="${MANYFORGE_BASELINE_SPECS_REPOSITORY:-}"
CPUSET="${MANYFORGE_RUNTIME_SCHEDULE_CPUSET:-}"
BUILD_JOBS="${MANYFORGE_BUILD_JOBS:-1}"
BASELINE_TOOLCHAIN_CXX_FLAGS="-Wno-error=maybe-uninitialized"
BASELINE_BUILD_TYPE="Release"
SOURCE_IDENTITY_CLI="${REPO_ROOT}/scripts/qualification/source_identity.py"
HARNESS_SOURCE="${REPO_ROOT}/tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp"
CMAKE_PATCH="${REPO_ROOT}/scripts/qualification/baseline/runtime_collision_schedule_cmake.patch"
BASELINE_COMPAT_PATCH="${REPO_ROOT}/scripts/qualification/baseline/runtime_collision_schedule_baseline_compat.patch"
OBSERVATION_PATCH="${REPO_ROOT}/scripts/qualification/baseline/runtime_collision_schedule_observation.patch"

if [[ -z "${SPECS_REPOSITORY}" || ! -d "${SPECS_REPOSITORY}" ]]; then
  echo "MANYFORGE_BASELINE_SPECS_REPOSITORY is required" >&2
  exit 2
fi
for required_file in "${SOURCE_IDENTITY_CLI}" "${HARNESS_SOURCE}" \
  "${CMAKE_PATCH}" "${BASELINE_COMPAT_PATCH}" "${OBSERVATION_PATCH}"; do
  [[ -f "${required_file}" ]] || { echo "baseline input missing: ${required_file}" >&2; exit 1; }
done
PREPARE_SCRIPT_SHA256="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"
SOURCE_IDENTITY_SCRIPT_SHA256="$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')"
HARNESS_SOURCE_SHA256="$(sha256sum "${HARNESS_SOURCE}" | awk '{print $1}')"
CMAKE_PATCH_SHA256="$(sha256sum "${CMAKE_PATCH}" | awk '{print $1}')"
BASELINE_COMPAT_PATCH_SHA256="$(sha256sum "${BASELINE_COMPAT_PATCH}" | awk '{print $1}')"
OBSERVATION_PATCH_SHA256="$(sha256sum "${OBSERVATION_PATCH}" | awk '{print $1}')"
[[ "${BUILD_JOBS}" == "1" ]] || {
  echo "formal runtime baseline preparation requires MANYFORGE_BUILD_JOBS=1" >&2; exit 2;
}

mapfile -t CPU_ENV < <(python3 - "${CPUSET}" <<'PY'
import sys
from pathlib import Path
cpus = set()
for part in sys.argv[1].split(","):
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

LANE_DIR="$(realpath -m "${LANE_DIR}")"
case "${LANE_DIR}" in
  "${REPO_ROOT}/build/"*) ;;
  *) echo "baseline lane must be a dedicated path below ${REPO_ROOT}/build" >&2; exit 2 ;;
esac
[[ ! -e "${LANE_DIR}" ]] || {
  echo "baseline lane already exists; refusing to overwrite: ${LANE_DIR}" >&2; exit 2;
}
[[ "$(git -C "${REPO_ROOT}" rev-parse "${BASELINE_SOURCE}^{commit}")" == "${BASELINE_SOURCE}" ]] || {
  echo "ManyForge baseline commit is unavailable" >&2; exit 1;
}
[[ "$(git -C "${SPECS_REPOSITORY}" rev-parse "${BASELINE_SPECS}^{commit}")" == "${BASELINE_SPECS}" ]] || {
  echo "Specs baseline commit is unavailable" >&2; exit 1;
}

TIMING_SOURCE="${LANE_DIR}/timing-source"
OBSERVATION_SOURCE="${LANE_DIR}/observation-source"
SPECS_SOURCE="${LANE_DIR}/specs-source"
ARTIFACT_DIR="${LANE_DIR}/artifacts"
mkdir -p "${ARTIFACT_DIR}"
git -C "${REPO_ROOT}" worktree add --detach "${TIMING_SOURCE}" "${BASELINE_SOURCE}"
git -C "${REPO_ROOT}" worktree add --detach "${OBSERVATION_SOURCE}" "${BASELINE_SOURCE}"
git -C "${SPECS_REPOSITORY}" worktree add --detach "${SPECS_SOURCE}" "${BASELINE_SPECS}"

TIMING_CLEAN_IDENTITY="$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${TIMING_SOURCE}")"
OBSERVATION_CLEAN_IDENTITY="$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${OBSERVATION_SOURCE}")"
SPECS_CLEAN_BUNDLE="$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_SOURCE}" --include-specs-contract-identity)"
SPECS_CLEAN_IDENTITY="$(python3 -c \
  'import json,sys; print(json.dumps(json.loads(sys.argv[1])["source_identity"], sort_keys=True, separators=(",", ":")))' "${SPECS_CLEAN_BUNDLE}")"
SPECS_CONTRACT_IDENTITY="$(python3 -c \
  'import json,sys; print(json.dumps(json.loads(sys.argv[1])["specs_contract_identity"], sort_keys=True, separators=(",", ":")))' "${SPECS_CLEAN_BUNDLE}")"
python3 - "${TIMING_CLEAN_IDENTITY}" "${OBSERVATION_CLEAN_IDENTITY}" \
  "${SPECS_CLEAN_IDENTITY}" "${SPECS_CONTRACT_IDENTITY}" \
  "${BASELINE_SOURCE}" "${BASELINE_SPECS}" <<'PY'
import json, sys
timing, observation, specs = (json.loads(value) for value in sys.argv[1:4])
specs_contract = json.loads(sys.argv[4])
source_commit, specs_commit = sys.argv[5:]
for label, identity, commit in (
    ("timing", timing, source_commit),
    ("observation", observation, source_commit),
    ("Specs", specs, specs_commit),
):
    if identity["head_commit"] != commit or identity["dirty"] is not False:
        raise SystemExit(f"{label} detached checkout is not the exact clean base")
if specs_contract["head_commit"] != specs_commit or specs_contract["contract_dirty"] is not False:
    raise SystemExit("Specs detached checkout does not have a clean contract identity")
PY

CLEAN_MANIFEST="${ARTIFACT_DIR}/clean-checkout-manifest.json"
python3 - "${CLEAN_MANIFEST}" "${TIMING_CLEAN_IDENTITY}" \
  "${OBSERVATION_CLEAN_IDENTITY}" "${SPECS_CLEAN_IDENTITY}" \
  "${SPECS_CONTRACT_IDENTITY}" <<'PY'
import json, sys
from pathlib import Path
output = Path(sys.argv[1])
manifest = {
    "schema_version": "manyforge.runtime_collision_baseline_clean_checkout.v2",
    "captured_before_patch": True,
    "repositories": {
        "manyforge_timing": json.loads(sys.argv[2]),
        "manyforge_observation": json.loads(sys.argv[3]),
        "specs": json.loads(sys.argv[4]),
    },
    "specs_contract_identity": json.loads(sys.argv[5]),
}
output.write_text(json.dumps(manifest, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
PY

install -D -m 0644 "${HARNESS_SOURCE}" \
  "${TIMING_SOURCE}/tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp"
git -C "${TIMING_SOURCE}" add -N -- \
  tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp
git -C "${TIMING_SOURCE}" apply "${CMAKE_PATCH}"
git -C "${TIMING_SOURCE}" apply "${BASELINE_COMPAT_PATCH}"

HARNESS_PATCH="${ARTIFACT_DIR}/baseline-harness.patch"
git -C "${TIMING_SOURCE}" diff --binary --full-index --no-ext-diff \
  --output="${HARNESS_PATCH}" HEAD -- \
  tests/cpp/CMakeLists.txt \
  tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp
if [[ "$(git -C "${TIMING_SOURCE}" diff --name-only HEAD --)" != \
      $'tests/cpp/CMakeLists.txt\ntests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp' ]]; then
  echo "timing worktree contains changes outside the harness-only scope" >&2
  exit 1
fi

git -C "${OBSERVATION_SOURCE}" apply "${HARNESS_PATCH}"
git -C "${OBSERVATION_SOURCE}" add -N -- \
  tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp
git -C "${OBSERVATION_SOURCE}" apply "${OBSERVATION_PATCH}"
if [[ "$(git -C "${OBSERVATION_SOURCE}" diff --name-only HEAD --)" != \
      $'manyforge_collision/include/manyforge_collision/manyforge_collision_engine.hpp\nmanyforge_collision/src/manyforge_collision_engine.cpp\ntests/cpp/CMakeLists.txt\ntests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp' ]]; then
  echo "observation worktree contains a change outside its declared scope" >&2
  exit 1
fi

TIMING_SOURCE_IDENTITY="$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${TIMING_SOURCE}")"
OBSERVATION_SOURCE_IDENTITY="$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${OBSERVATION_SOURCE}")"

relative_to_workspace() {
  printf '/workspace/%s' "${1#"${REPO_ROOT}/"}"
}
TIMING_SOURCE_CONTAINER="$(relative_to_workspace "${TIMING_SOURCE}")"
OBSERVATION_SOURCE_CONTAINER="$(relative_to_workspace "${OBSERVATION_SOURCE}")"
TIMING_BUILD_CONTAINER="$(relative_to_workspace "${LANE_DIR}/timing-build")"
OBSERVATION_BUILD_CONTAINER="$(relative_to_workspace "${LANE_DIR}/observation-build")"
BUILD_SCRIPT="
set -euo pipefail
export CMAKE_PREFIX_PATH=/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix:\${CMAKE_PREFIX_PATH:-}
export LD_LIBRARY_PATH=/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix/lib:\${LD_LIBRARY_PATH:-}
taskset -c '${CPUSET}' cmake -S '${TIMING_SOURCE_CONTAINER}/manyforge_roboplan' -B '${TIMING_BUILD_CONTAINER}' -DCMAKE_BUILD_TYPE='${BASELINE_BUILD_TYPE}' -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS='${BASELINE_TOOLCHAIN_CXX_FLAGS}'
taskset -c '${CPUSET}' cmake --build '${TIMING_BUILD_CONTAINER}' --target perf_runtime_collision_schedule_baseline_timing --parallel '${BUILD_JOBS}'
taskset -c '${CPUSET}' cmake -S '${OBSERVATION_SOURCE_CONTAINER}/manyforge_roboplan' -B '${OBSERVATION_BUILD_CONTAINER}' -DCMAKE_BUILD_TYPE='${BASELINE_BUILD_TYPE}' -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS='${BASELINE_TOOLCHAIN_CXX_FLAGS}'
taskset -c '${CPUSET}' cmake --build '${OBSERVATION_BUILD_CONTAINER}' --target perf_runtime_collision_schedule_baseline_observation --parallel '${BUILD_JOBS}'
"
BUILD_COMMAND=(docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm -T dev bash -lc "${BUILD_SCRIPT}")
BUILD_COMMANDS_JSON="$(python3 -c 'import json,sys; print(json.dumps([sys.argv[1:]], separators=(",", ":")))' "${BUILD_COMMAND[@]}")"

IMAGE_REFERENCE="$(docker compose -f "${REPO_ROOT}/docker-compose.yaml" images -q dev 2>/dev/null)"
[[ -n "${IMAGE_REFERENCE}" && "${IMAGE_REFERENCE}" != *$'\n'* ]] || {
  echo "Compose dev service does not resolve to exactly one image" >&2; exit 1;
}
IMAGE_ID="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
IMAGE_ID="${IMAGE_ID#sha256:}"
[[ "${IMAGE_ID}" =~ ^[0-9a-f]{64}$ ]] || {
  echo "unable to resolve exact Compose dev image id" >&2; exit 1;
}
"${BUILD_COMMAND[@]}"

[[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${TIMING_SOURCE}")" == "${TIMING_SOURCE_IDENTITY}" &&
   "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${OBSERVATION_SOURCE}")" == "${OBSERVATION_SOURCE_IDENTITY}" &&
   "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_SOURCE}" --include-specs-contract-identity)" == "${SPECS_CLEAN_BUNDLE}" ]] || {
  echo "baseline source identity changed during compilation" >&2; exit 1;
}
IMAGE_AFTER_BUILD="$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null || true)"
IMAGE_AFTER_BUILD="${IMAGE_AFTER_BUILD#sha256:}"
[[ "${IMAGE_AFTER_BUILD}" == "${IMAGE_ID}" ]] || {
  echo "Compose dev image changed during baseline compilation" >&2; exit 1;
}

TIMING_BINARY="${LANE_DIR}/timing-build/manyforge_integration_tests/perf_runtime_collision_schedule_baseline_timing"
OBSERVATION_BINARY="${LANE_DIR}/observation-build/manyforge_integration_tests/perf_runtime_collision_schedule_baseline_observation"
[[ -x "${TIMING_BINARY}" && -x "${OBSERVATION_BINARY}" ]] || {
  echo "baseline binary missing after build" >&2; exit 1;
}
CLEAN_MANIFEST_SHA256="$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')"
HARNESS_PATCH_SHA256="$(sha256sum "${HARNESS_PATCH}" | awk '{print $1}')"
TIMING_BINARY_SHA256="$(sha256sum "${TIMING_BINARY}" | awk '{print $1}')"
OBSERVATION_BINARY_SHA256="$(sha256sum "${OBSERVATION_BINARY}" | awk '{print $1}')"

METADATA="${ARTIFACT_DIR}/lane-metadata.json"
python3 - "${METADATA}" "${TIMING_SOURCE_IDENTITY}" \
  "${OBSERVATION_SOURCE_IDENTITY}" "${SPECS_CLEAN_IDENTITY}" \
  "${SPECS_CONTRACT_IDENTITY}" \
  "${IMAGE_ID}" "${CPUSET}" "${CPU_GOVERNOR_SET}" \
  "${CPU_FREQUENCY_DRIVER_SET}" "${HOST_KERNEL_ID}" \
  "${BASELINE_TOOLCHAIN_CXX_FLAGS}" "${BASELINE_BUILD_TYPE}" \
  "${CLEAN_MANIFEST_SHA256}" \
  "${HARNESS_PATCH_SHA256}" \
  "${OBSERVATION_PATCH_SHA256}" \
  "${TIMING_BINARY_SHA256}" \
  "${OBSERVATION_BINARY_SHA256}" \
  "${BUILD_COMMANDS_JSON}" \
  "${PREPARE_SCRIPT_SHA256}" \
  "${SOURCE_IDENTITY_SCRIPT_SHA256}" \
  "${HARNESS_SOURCE_SHA256}" "${CMAKE_PATCH_SHA256}" \
  "${BASELINE_COMPAT_PATCH_SHA256}" <<'PY'
import json, sys
from pathlib import Path
(
    output, timing_identity, observation_identity, specs_identity,
    specs_contract_identity, image_id,
    cpuset, governors, drivers, kernel_id, cxx_flags, build_type,
    clean_hash, harness_hash, observation_hash, timing_binary_hash,
    observation_binary_hash, build_commands_json, prepare_hash, identity_cli_hash,
    harness_source_hash, cmake_patch_hash, baseline_compat_patch_hash,
) = sys.argv[1:]
metadata = {
    "schema_version": "manyforge.runtime_collision_baseline_lane.v2",
    "timing_source_identity": json.loads(timing_identity),
    "observation_source_identity": json.loads(observation_identity),
    "specs_source_identity": json.loads(specs_identity),
    "specs_contract_identity": json.loads(specs_contract_identity),
    "image_id": image_id,
    "requested_cpu_affinity": cpuset,
    "cpu_governor_set": governors,
    "cpu_frequency_driver_set": drivers,
    "kernel_id": kernel_id,
    "compiler_flags": cxx_flags,
    "build_type": build_type,
    "clean_checkout_manifest_sha256": clean_hash,
    "qualification_patch_sha256": harness_hash,
    "observation_patch_sha256": observation_hash,
    "timing_binary_sha256": timing_binary_hash,
    "observation_binary_sha256": observation_binary_hash,
    "build_commands_argv": json.loads(build_commands_json),
    "prepare_script_sha256": prepare_hash,
    "source_identity_script_sha256": identity_cli_hash,
    "harness_source_sha256": harness_source_hash,
    "cmake_patch_sha256": cmake_patch_hash,
    "baseline_compat_patch_sha256": baseline_compat_patch_hash,
}
Path(output).write_text(json.dumps(metadata, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
PY

if [[ "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${TIMING_SOURCE}")" != "${TIMING_SOURCE_IDENTITY}" ||
      "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${OBSERVATION_SOURCE}")" != "${OBSERVATION_SOURCE_IDENTITY}" ||
      "$(python3 "${SOURCE_IDENTITY_CLI}" --repo-root "${SPECS_SOURCE}" --include-specs-contract-identity)" != "${SPECS_CLEAN_BUNDLE}" ||
      "$(sha256sum "${TIMING_BINARY}" | awk '{print $1}')" != "${TIMING_BINARY_SHA256}" ||
      "$(sha256sum "${OBSERVATION_BINARY}" | awk '{print $1}')" != "${OBSERVATION_BINARY_SHA256}" ||
      "$(sha256sum "${CLEAN_MANIFEST}" | awk '{print $1}')" != "${CLEAN_MANIFEST_SHA256}" ||
      "$(sha256sum "${HARNESS_PATCH}" | awk '{print $1}')" != "${HARNESS_PATCH_SHA256}" ||
      "$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')" != "${PREPARE_SCRIPT_SHA256}" ||
      "$(sha256sum "${SOURCE_IDENTITY_CLI}" | awk '{print $1}')" != "${SOURCE_IDENTITY_SCRIPT_SHA256}" ||
      "$(sha256sum "${HARNESS_SOURCE}" | awk '{print $1}')" != "${HARNESS_SOURCE_SHA256}" ||
      "$(sha256sum "${CMAKE_PATCH}" | awk '{print $1}')" != "${CMAKE_PATCH_SHA256}" ||
      "$(sha256sum "${BASELINE_COMPAT_PATCH}" | awk '{print $1}')" != "${BASELINE_COMPAT_PATCH_SHA256}" ||
      "$(sha256sum "${OBSERVATION_PATCH}" | awk '{print $1}')" != "${OBSERVATION_PATCH_SHA256}" ||
      "$(docker image inspect "${IMAGE_REFERENCE}" --format '{{.Id}}' 2>/dev/null | sed 's/^sha256://')" != "${IMAGE_ID}" ]]; then
  echo "baseline source, Specs, binary, image, or preparation input changed before metadata completed" >&2
  exit 1
fi

echo "prepared detached Step 18 runtime baseline lane: ${LANE_DIR}"
echo "metadata: ${METADATA}"
