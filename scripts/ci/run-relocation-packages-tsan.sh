#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_ROOT="${RELOCATION_PACKAGES_BUILD_ROOT:-${REPO_ROOT}/build/tsan-relocation-packages}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
TEST_TIMEOUT_SECONDS="${MANYFORGE_TSAN_GTEST_TIMEOUT_SECONDS:-300}"
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

sanitizer_flags="-fsanitize=thread -fno-omit-frame-pointer -g -Wno-error=array-bounds"

run_package() {
  local package="$1"
  local testing_option="$2"
  local build_dir="${BUILD_ROOT}/${package}"
  shift 2

  cmake \
    -S "${REPO_ROOT}/${package}" \
    -B "${build_dir}" \
    "-${testing_option}=ON" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g1 -DNDEBUG" \
    -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST \
    -DCMAKE_CXX_FLAGS="${sanitizer_flags}" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread" \
    "$@"
  cmake --build "${build_dir}" --parallel "${PARALLEL}"
  ctest \
    --test-dir "${build_dir}" \
    --output-on-failure \
    --no-tests=error \
    --timeout "${TEST_TIMEOUT_SECONDS}"
}

run_package \
  manyforge_content_identity \
  DMANYFORGE_CONTENT_IDENTITY_BUILD_TESTING
run_package \
  manyforge_persistence \
  DMANYFORGE_PERSISTENCE_BUILD_TESTING
run_package \
  manyforge_component_manager \
  DBUILD_TESTING \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${TEST_TIMEOUT_SECONDS}"
run_package \
  manyforge_scene \
  DBUILD_TESTING \
  -DMANYFORGE_SCENE_GTEST_TIMEOUT_SECONDS="${TEST_TIMEOUT_SECONDS}"
