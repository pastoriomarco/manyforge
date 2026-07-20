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

if [[ -f /opt/ros/jazzy/setup.bash ]]; then
  # CMake's ament package templates import ament_package from the ROS Python
  # tree. Source ROS here so the TSan lane is reproducible under docker exec and
  # not dependent on an interactive shell profile.
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

# `-Wno-error=array-bounds` works around a known gcc-13 false positive on
# `std::vector<T>::push_back` of small-buffer-optimized inner vectors
# (TrajectoryWaypointDTO holding vector<double>). Under
# RelWithDebInfo + TSan flags the optimizer's inlined memmove path emits a
# spurious "forming offset N is out of the bounds [0, N]" warning that the
# default -Werror promotion converts to a hard build failure. Mirrors the
# same fix in scripts/ci/run-core-tsan.sh.
cmake \
  -S "${REPO_ROOT}/manyforge_roboplan" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${GTEST_TIMEOUT_SECONDS}" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -Wno-error=array-bounds" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}" \
  --target test_collision_engine_integration
python3 "${SCRIPT_DIR}/run-ctest-gtest-binaries-grouped.py" \
  --build-dir "${BUILD_DIR}" \
  --test-regex "ManyForgeCollisionEngineTest" \
  --summary-prefix COLLISION_TSAN
