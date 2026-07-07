#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/asan-core}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
GTEST_TIMEOUT_SECONDS="${MANYFORGE_ASAN_GTEST_TIMEOUT_SECONDS:-300}"

ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1 detect_leaks=1}"
if [[ "${ASAN_OPTIONS}" != *"exitcode="* ]]; then
  ASAN_OPTIONS="${ASAN_OPTIONS}:exitcode=66"
fi
export ASAN_OPTIONS

UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1 print_stacktrace=1}"
if [[ "${UBSAN_OPTIONS}" != *"exitcode="* ]]; then
  UBSAN_OPTIONS="${UBSAN_OPTIONS}:exitcode=66"
fi
export UBSAN_OPTIONS

cmake \
  -S "${REPO_ROOT}/manyforge_core" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${GTEST_TIMEOUT_SECONDS}" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -Wno-error=maybe-uninitialized -Wno-error=array-bounds" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
