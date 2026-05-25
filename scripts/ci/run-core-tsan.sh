#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/tsan-manyforge-core}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

# `-Wno-error=array-bounds` works around a known gcc-13 false positive on
# `std::vector<T>::push_back` where the inner element is itself a small
# `std::vector<>` (here: `TrajectoryDTO` holding `TrajectoryWaypointDTO`
# holding `vector<double>`). Under TSan's `RelWithDebInfo` flags the
# optimizer's inlined memmove path triggers a spurious "forming offset N
# is out of the bounds [0, N]" warning. The warning stays visible in CI
# logs (no `-Wno-array-bounds`) but is not promoted to error.
cmake \
  -S "${REPO_ROOT}/manyforge_core" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -Wno-error=array-bounds" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
