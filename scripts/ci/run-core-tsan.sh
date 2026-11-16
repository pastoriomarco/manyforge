#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/tsan-manyforge-core}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
GTEST_TIMEOUT_SECONDS="${MANYFORGE_TSAN_GTEST_TIMEOUT_SECONDS:-300}"
TSAN_SUPPRESSIONS="${TSAN_SUPPRESSIONS:-${REPO_ROOT}/scripts/ci/tsan.supp}"

if [[ ! -f "${TSAN_SUPPRESSIONS}" ]]; then
  echo "TSan suppression file not found: ${TSAN_SUPPRESSIONS}" >&2
  exit 2
fi

TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1 second_deadlock_stack=1 history_size=7}"
if [[ "${TSAN_OPTIONS}" != *"exitcode="* ]]; then
  TSAN_OPTIONS="${TSAN_OPTIONS} exitcode=66"
fi
if [[ "${TSAN_OPTIONS}" != *"suppressions="* ]]; then
  TSAN_OPTIONS="${TSAN_OPTIONS} suppressions=${TSAN_SUPPRESSIONS}"
fi
export TSAN_OPTIONS

cmake \
  -S "${REPO_ROOT}/manyforge_core" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g1 -DNDEBUG" \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${GTEST_TIMEOUT_SECONDS}" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -Wno-error=maybe-uninitialized" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

# gcc 13's -O1 sanitizer build reports false "may be used uninitialized" warnings for values
# inside std::optional (OD-262); they still print but do not fail the build.
# The test binaries run with address randomisation off: gcc 13's ThreadSanitizer aborts at start
# ("unexpected memory mapping") under the kernel's mmap randomisation (OD-262). In a container
# this needs the personality(ADDR_NO_RANDOMIZE) call allowed by its seccomp profile. The build runs
# under it too, because test discovery runs each test binary while building.
setarch "$(uname -m)" -R cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}"
setarch "$(uname -m)" -R python3 "${SCRIPT_DIR}/run-ctest-gtest-binaries-grouped.py" \
  --build-dir "${BUILD_DIR}" \
  --summary-prefix CORE_TSAN
