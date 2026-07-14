#!/usr/bin/env bash
# Strict sanitizer lane for the patched RoboPlan Scene/CollisionContext
# generation contract. Run inside the native repository container.

set -euo pipefail

if [[ $# -ne 1 || ( "$1" != "address" && "$1" != "thread" ) ]]; then
  echo "usage: $0 <address|thread>" >&2
  exit 2
fi

SANITIZER="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROBOPLAN_SOURCE="${ROBOPLAN_SOURCE:-${REPO_ROOT}/.docker/roboplan}"
MANIFEST_DIR="${REPO_ROOT}/third_party/roboplan"
BUILD_ROOT="${BUILD_ROOT:-/tmp/manyforge-roboplan-scene-${SANITIZER}}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

expected_commit="$(
  sed -n 's/^- Commit: `\([0-9a-f]\{40\}\)`$/\1/p' \
    "${MANIFEST_DIR}/README.md" | head -n 1
)"
if [[ -z "${expected_commit}" ]]; then
  echo "RoboPlan manifest does not contain one pinned commit" >&2
  exit 2
fi
if [[ ! -d "${ROBOPLAN_SOURCE}/.git" ]]; then
  echo "RoboPlan source checkout is missing: ${ROBOPLAN_SOURCE}" >&2
  exit 2
fi
actual_commit="$(git -C "${ROBOPLAN_SOURCE}" rev-parse HEAD)"
source_status="$(git -C "${ROBOPLAN_SOURCE}" status --porcelain=v1 \
  --untracked-files=all --ignore-submodules=none)"
if [[ "${actual_commit}" != "${expected_commit}" || -n "${source_status}" ]]; then
  echo "RoboPlan source must be the exact clean pinned generation" >&2
  echo "expected=${expected_commit} actual=${actual_commit}" >&2
  exit 2
fi

(cd "${MANIFEST_DIR}" && sha256sum -c patches.sha256)

rm -rf "${BUILD_ROOT}"
mkdir -p "${BUILD_ROOT}/source"
tar -C "${ROBOPLAN_SOURCE}" --exclude=.git --exclude=build \
  --exclude=install -cf - . | tar -C "${BUILD_ROOT}/source" -xf -
git -C "${BUILD_ROOT}/source" init -q
while read -r _digest patch; do
  case "${patch}" in
    patches/*.patch) ;;
    *) echo "invalid RoboPlan patch path: ${patch}" >&2; exit 2 ;;
  esac
  git -C "${BUILD_ROOT}/source" apply --check \
    "${MANIFEST_DIR}/${patch}"
  git -C "${BUILD_ROOT}/source" apply "${MANIFEST_DIR}/${patch}"
done < "${MANIFEST_DIR}/patches.sha256"

case "${SANITIZER}" in
  address)
    # Pinocchio is supplied as a prebuilt, non-ASan shared library. Eigen
    # otherwise changes from the system allocator to its handmade aligned
    # allocator when __SANITIZE_ADDRESS__ is defined, making inline destructors
    # ABI-incompatible with allocations performed inside that library. Keep the
    # allocator contract identical across the DSO boundary; ASan still
    # intercepts and validates the matching malloc/free calls.
    sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g -DEIGEN_MALLOC_ALREADY_ALIGNED=1"
    # Required options are appended last so an inherited developer setting
    # cannot turn a sanitizer report into a successful strict-lane result.
    export ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}halt_on_error=1:detect_leaks=1:exitcode=66"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:+${UBSAN_OPTIONS}:}halt_on_error=1:print_stacktrace=1:exitcode=66"
    ;;
  thread)
    sanitizer_flags="-fsanitize=thread -fno-omit-frame-pointer -g"
    tsan_suppressions="${TSAN_SUPPRESSIONS:-${REPO_ROOT}/scripts/ci/tsan.supp}"
    if [[ ! -f "${tsan_suppressions}" ]]; then
      echo "TSan suppression file not found: ${tsan_suppressions}" >&2
      exit 2
    fi
    export TSAN_OPTIONS="${TSAN_OPTIONS:+${TSAN_OPTIONS} }halt_on_error=1 second_deadlock_stack=1 history_size=7 exitcode=66 suppressions=${tsan_suppressions}"
    ;;
esac

cmeel_prefix="$(python3 - <<'PY'
import pathlib
import site

for root in map(pathlib.Path, site.getsitepackages()):
    candidate = root / "cmeel.prefix"
    if (candidate / "lib/cmake/pinocchio/pinocchioConfig.cmake").exists():
        print(candidate)
        raise SystemExit(0)
raise SystemExit(1)
PY
)"

common_cmake_args=(
  -DBUILD_TESTING=OFF
  -DBUILD_PYTHON_BINDINGS=OFF
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  "-DCMAKE_CXX_FLAGS=${sanitizer_flags}"
  "-DCMAKE_EXE_LINKER_FLAGS=${sanitizer_flags}"
  "-DCMAKE_SHARED_LINKER_FLAGS=${sanitizer_flags}"
)

install_project() {
  local project="$1"
  local install_prefix="${BUILD_ROOT}/install/${project}"
  cmake -S "${BUILD_ROOT}/source/${project}" \
    -B "${BUILD_ROOT}/build/${project}" \
    -DCMAKE_INSTALL_PREFIX="${install_prefix}" \
    "${common_cmake_args[@]}"
  cmake --build "${BUILD_ROOT}/build/${project}" --parallel "${PARALLEL}"
  cmake --install "${BUILD_ROOT}/build/${project}"
  export CMAKE_PREFIX_PATH="${install_prefix}:${CMAKE_PREFIX_PATH:-}"
  export LD_LIBRARY_PATH="${install_prefix}/lib:${LD_LIBRARY_PATH:-}"
}

export CMAKE_PREFIX_PATH="${cmeel_prefix}:/usr/local:${CMAKE_PREFIX_PATH:-}"
export LD_LIBRARY_PATH="${cmeel_prefix}/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}"
install_project roboplan_example_models
install_project roboplan

manyforge_build="${BUILD_ROOT}/build/manyforge_roboplan"
cmake -S "${REPO_ROOT}/manyforge_roboplan" -B "${manyforge_build}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  "-DCMAKE_CXX_FLAGS=${sanitizer_flags}" \
  "-DCMAKE_EXE_LINKER_FLAGS=${sanitizer_flags}" \
  "-DCMAKE_SHARED_LINKER_FLAGS=${sanitizer_flags}"
cmake --build "${manyforge_build}" --parallel "${PARALLEL}" \
  --target test_roboplan_scene_generation
ctest --test-dir "${manyforge_build}" --output-on-failure \
  -R '^RoboPlanSceneGenerationTest\.'

if [[ "$(git -C "${ROBOPLAN_SOURCE}" rev-parse HEAD)" != "${expected_commit}" \
      || -n "$(git -C "${ROBOPLAN_SOURCE}" status --porcelain=v1 \
        --untracked-files=all --ignore-submodules=none)" ]]; then
  echo "sanitizer lane modified the pinned RoboPlan checkout" >&2
  exit 2
fi
