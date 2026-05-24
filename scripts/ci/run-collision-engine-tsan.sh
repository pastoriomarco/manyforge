#!/usr/bin/env bash
# ThreadSanitizer lane for the Phase 4 collision-engine qualification path.
#
# This builds the RoboPlan composition package because the cross-package
# integration tests need both manyforge_roboplan (snapshot publisher / shadow
# comparator) and manyforge_collision (authoritative engine). The production
# engine code remains in manyforge_collision; this script only chooses the
# CMake entry point that wires the integration tests.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/tsan-collision-engine}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

cmake \
  -S "${REPO_ROOT}/manyforge_roboplan" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}" \
  --target test_collision_engine_integration
ctest --test-dir "${BUILD_DIR}" --output-on-failure \
  -R "ManyForgeCollisionEngineTest"
