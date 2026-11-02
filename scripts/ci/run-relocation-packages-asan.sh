#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_ROOT="${RELOCATION_PACKAGES_BUILD_ROOT:-${REPO_ROOT}/build/asan-relocation-packages}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
TEST_TIMEOUT_SECONDS="${MANYFORGE_ASAN_GTEST_TIMEOUT_SECONDS:-300}"

ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:detect_leaks=1}"
if [[ "${ASAN_OPTIONS}" != *"exitcode="* ]]; then
  ASAN_OPTIONS="${ASAN_OPTIONS}:exitcode=66"
fi
export ASAN_OPTIONS

UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
if [[ "${UBSAN_OPTIONS}" != *"exitcode="* ]]; then
  UBSAN_OPTIONS="${UBSAN_OPTIONS}:exitcode=66"
fi
export UBSAN_OPTIONS

sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

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
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
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
  manyforge_scene \
  DBUILD_TESTING \
  -DMANYFORGE_SCENE_GTEST_TIMEOUT_SECONDS="${TEST_TIMEOUT_SECONDS}"
