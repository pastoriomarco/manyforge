#!/usr/bin/env bash
# AddressSanitizer/UndefinedBehaviorSanitizer lane for the collision-engine
# integration suite. The RoboPlan composition package is the CMake entry point
# that wires manyforge_collision to its cross-package integration tests.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/asan-collision-engine}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
GTEST_TIMEOUT_SECONDS="${MANYFORGE_ASAN_GTEST_TIMEOUT_SECONDS:-300}"

# Append the strict settings last so inherited developer options cannot turn a
# sanitizer report into a successful lane result.
export ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}halt_on_error=1:detect_leaks=1:exitcode=66"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+${UBSAN_OPTIONS}:}halt_on_error=1:print_stacktrace=1:exitcode=66"

if [[ -f /opt/ros/jazzy/setup.bash ]]; then
  # CMake's ament package templates import ament_package from the ROS Python
  # tree. Keep this reproducible under docker exec and non-interactive shells.
  # shellcheck disable=SC1091
  set +u
  source /opt/ros/jazzy/setup.bash
  set -u
fi

cmeel_prefix="${MANYFORGE_CMEEL_PREFIX:-/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix}"
if [[ -d "${cmeel_prefix}" ]]; then
  export CMAKE_PREFIX_PATH="${cmeel_prefix}:${CMAKE_PREFIX_PATH:-}"
  export LD_LIBRARY_PATH="${cmeel_prefix}/lib:${LD_LIBRARY_PATH:-}"
  export PKG_CONFIG_PATH="${cmeel_prefix}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
fi

# Pinocchio is supplied as a prebuilt, non-ASan shared library. Keep Eigen's
# allocator contract identical across that DSO boundary while ASan continues
# to intercept and validate the matching malloc/free calls.
sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g -DEIGEN_MALLOC_ALREADY_ALIGNED=1 -Wno-error=maybe-uninitialized -Wno-error=array-bounds"

cmake \
  -S "${REPO_ROOT}/manyforge_roboplan" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${GTEST_TIMEOUT_SECONDS}" \
  "-DCMAKE_CXX_FLAGS=${sanitizer_flags}" \
  "-DCMAKE_EXE_LINKER_FLAGS=${sanitizer_flags}" \
  "-DCMAKE_SHARED_LINKER_FLAGS=${sanitizer_flags}"

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}" \
  --target test_collision_engine_integration
python3 "${SCRIPT_DIR}/run-ctest-gtest-binaries-grouped.py" \
  --build-dir "${BUILD_DIR}" \
  --test-regex "ManyForgeCollisionEngineTest" \
  --summary-prefix COLLISION_ASAN_UBSAN
