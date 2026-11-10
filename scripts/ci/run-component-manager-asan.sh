#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${COMPONENT_MANAGER_BUILD_DIR:-${REPO_ROOT}/build/asan-component-manager}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
GTEST_TIMEOUT_SECONDS="${MANYFORGE_ASAN_GTEST_TIMEOUT_SECONDS:-300}"

# ASan and LeakSanitizer can recurse in their signal handler during CMake's
# short post-link GoogleTest discovery when PIE mappings collide with ASLR.
# The binding sanitizer already fixes the address layout this way; keep the
# native retained-product lane deterministic for the same reason.
expected_personality="00040000"
current_personality="$(cat /proc/self/personality)"
if [[ "${current_personality}" != "${expected_personality}" ]]; then
  if [[ "${MANYFORGE_NATIVE_ASAN_SETARCH_REEXEC:-0}" == "1" ]]; then
    echo "native ASan/UBSan ADDR_NO_RANDOMIZE re-exec did not take effect" >&2
    exit 2
  fi
  export MANYFORGE_NATIVE_ASAN_SETARCH_REEXEC=1
  exec /usr/bin/setarch "$(uname -m)" -R "$0" "$@"
fi
unset MANYFORGE_NATIVE_ASAN_SETARCH_REEXEC

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

cmake \
  -S "${REPO_ROOT}/manyforge_component_manager" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g1 -DNDEBUG" \
  -DMANYFORGE_CORE_INCLUDE_DIR="${REPO_ROOT}/manyforge_core/include" \
  -DMANYFORGE_DEPLOYMENT_SOURCE_DIR="${REPO_ROOT}/manyforge_deployment" \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${GTEST_TIMEOUT_SECONDS}" \
  -DCMAKE_CXX_FLAGS="${sanitizer_flags}" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}"

python3 "${SCRIPT_DIR}/run-ctest-gtest-binaries-grouped.py" \
  --build-dir "${BUILD_DIR}" \
  --summary-prefix COMPONENT_MANAGER_ASAN_UBSAN
