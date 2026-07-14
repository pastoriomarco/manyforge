#!/usr/bin/env bash
# Build the ManyForge native RoboPlan module with the C++ ros2_control backend.
#
# Composer's ROS execution deployment imports manyforge_roboplan_py from the
# shared build cache.  This script keeps that module built with
# MANYFORGE_ENABLE_ROS2_CONTROL=ON so the runtime cannot silently start
# without the C++ controller-backed execution surface.

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
ROS_DISTRO="${ROS_DISTRO:-jazzy}"
MANYFORGE_VENV="${MANYFORGE_VENV:-/opt/manyforge/venv}"
BUILD_DIR="${MANYFORGE_ROBOPLAN_BUILD_DIR:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/roboplan}"

set +u
source "/opt/ros/${ROS_DISTRO}/setup.bash"
source "/opt/manyforge/topic_based_ros2_control/install/setup.bash" 2>/dev/null || true
if [ -d "${MANYFORGE_VENV}" ]; then
    source "${MANYFORGE_VENV}/bin/activate"
fi
set -u

PINOCCHIO_CMAKE_DIR=$(
    find "${MANYFORGE_VENV}" /usr -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1 || true
)
if [ -z "${PINOCCHIO_CMAKE_DIR}" ]; then
    echo "pinocchioConfig.cmake not found; cannot build manyforge_roboplan." >&2
    exit 1
fi
PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "${PINOCCHIO_CMAKE_DIR}")")")
export CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}"

PYBIND11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")

cache_file="${BUILD_DIR}/CMakeCache.txt"
if [ -f "${cache_file}" ] && ! grep -q '^MANYFORGE_ENABLE_ROS2_CONTROL:BOOL=ON$' "${cache_file}"; then
    echo "Rebuilding ${BUILD_DIR}: existing cache was not ROS-enabled." >&2
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${WORKSPACE}/manyforge_roboplan" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMANYFORGE_ENABLE_ROS2_CONTROL=ON \
    -DMANYFORGE_ENABLE_QUALIFICATION_PROBES="${MANYFORGE_ENABLE_QUALIFICATION_PROBES:-OFF}" \
    -DBUILD_TESTING=OFF \
    -Dpybind11_DIR="${PYBIND11_DIR}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

if ! ls "${BUILD_DIR}"/manyforge_roboplan_py*.so >/dev/null 2>&1; then
    echo "manyforge_roboplan_py was not produced in ${BUILD_DIR}." >&2
    exit 1
fi

# Loading the extension is the runtime ABI gate. Merely producing the file can
# hide a stale dependency image until the first deployment factory imports it.
PYTHONPATH="${BUILD_DIR}:${WORKSPACE}/manyforge_roboplan/python:${PYTHONPATH:-}" \
    python -c "import manyforge_roboplan_py"

if [[ "${MANYFORGE_ENABLE_QUALIFICATION_PROBES:-OFF}" == "ON" ]] &&
   [[ ! -f "${BUILD_DIR}/libroboplan_collision_counter_probe.so" ]]; then
    echo "RP5 qualification collision probe was not produced in ${BUILD_DIR}." >&2
    exit 1
fi

echo "ROS-enabled manyforge_roboplan_py ready in ${BUILD_DIR}."
