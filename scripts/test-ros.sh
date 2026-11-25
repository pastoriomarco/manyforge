#!/usr/bin/env bash
# Run ROS adapter tests in the official full ROS Lyrical image.
#
# Intended usage:
#   docker compose --profile ros run --rm ros-jazzy ./scripts/test-ros.sh

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
ROS_DISTRO="${ROS_DISTRO:-lyrical}"

if [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
    # shellcheck disable=SC1090
    set +u
    source "/opt/ros/${ROS_DISTRO}/setup.bash"
    set -u
fi

if env | grep -Fq "${WORKSPACE}/install"; then
    echo "Canonical ROS tests refuse the source-mounted workspace install overlay." >&2
    env | grep -F "${WORKSPACE}/install" >&2
    exit 2
fi

export PYTHONPYCACHEPREFIX="${PYTHONPYCACHEPREFIX:-/tmp/manyforge-pycache}"
export PYTHONPATH="${WORKSPACE}/manyforge_ros:${WORKSPACE}/manyforge_behavior:${WORKSPACE}/manyforge_medkit_adapter:${WORKSPACE}:${PYTHONPATH:-}"

# The ROS adapters reach the owners through the gateway core binding
# (manyforge_headless_py): build just that module, as scripts/test-all.sh
# does, and put it on PYTHONPATH.
BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"
BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
BUILD_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
headless_build="$BUILD_DIR/headless"
if [ ! -f "$headless_build/CMakeCache.txt" ]; then
    cmake -S "$WORKSPACE/manyforge_headless" -B "$headless_build" -G "$BUILD_GENERATOR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DMANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL=ON \
        -DMANYFORGE_HEADLESS_BUILD_PYTHON=ON \
        -Dpybind11_DIR="$(python3 -c 'import pybind11; print(pybind11.get_cmake_dir())')"
fi
cmake --build "$headless_build" --parallel "$BUILD_PARALLEL_LEVEL" --target manyforge_headless_py
export PYTHONPATH="$headless_build:${PYTHONPATH}"

cd "$WORKSPACE"
# The run gets its own ROS domain (Fast DDS #6599).
ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST \
    "/opt/ros/${ROS_DISTRO}/lib/rmw_test_fixture_implementation/run_rmw_isolated" \
    pytest manyforge_ros/tests/ manyforge_medkit_adapter/tests/ -v
