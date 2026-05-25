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
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -Wno-error=array-bounds" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}" \
  --target test_collision_engine_integration
ctest --test-dir "${BUILD_DIR}" --output-on-failure \
  -R "ManyForgeCollisionEngineTest"
