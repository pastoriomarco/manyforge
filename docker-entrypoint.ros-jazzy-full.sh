#!/bin/bash
set -e

ROS_DISTRO="${ROS_DISTRO:-jazzy}"
MANYFORGE_VENV="${MANYFORGE_VENV:-/opt/manyforge/venv}"

if [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
    # shellcheck disable=SC1090
    set +u
    source "/opt/ros/${ROS_DISTRO}/setup.bash"
    set -u
fi

if [ -d "$MANYFORGE_VENV" ]; then
    # shellcheck disable=SC1091
    set +u
    source "${MANYFORGE_VENV}/bin/activate"
    set -u
fi

if [ -f "/workspace/install/setup.bash" ]; then
    # shellcheck disable=SC1091
    set +u
    source "/workspace/install/setup.bash"
    set -u
fi

export PYTHONPATH="/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace:${PYTHONPATH:-}"
export MANYFORGE_BUILD_DIR="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"

if [ -d "$MANYFORGE_BUILD_DIR" ] && [ ! -w "$MANYFORGE_BUILD_DIR" ]; then
    echo "Build cache not writable by $(whoami). Run: docker compose down -v" >&2
    exit 1
fi

reset_stale_python_build_dir() {
    local build_dir="$1"
    local cache_file="$build_dir/CMakeCache.txt"
    local current_site
    current_site=$(python3 -c "import site; print(site.getsitepackages()[0])")

    if [ -f "$cache_file" ] && grep -q "/usr/local/lib/python3\\." "$cache_file" && ! grep -q "$current_site" "$cache_file"; then
        rm -rf "$build_dir"
    fi
}

PINOCCHIO_CMAKE_DIR=$(find "$MANYFORGE_VENV" /usr -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1 || true)
if [ -n "${PINOCCHIO_CMAKE_DIR:-}" ]; then
    PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "$PINOCCHIO_CMAKE_DIR")")")
    export CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}"
    if [ -f "${PINOCCHIO_PREFIX}/include/boost/version.hpp" ]; then
        export BOOST_ROOT="${PINOCCHIO_PREFIX}"
        export BOOST_INCLUDEDIR="${PINOCCHIO_PREFIX}/include"
        export BOOST_LIBRARYDIR="${PINOCCHIO_PREFIX}/lib"
    fi
fi

CORE_BUILD="$MANYFORGE_BUILD_DIR/core"
PYBIND_BUILD="$MANYFORGE_BUILD_DIR/pybind"
ROBOPLAN_BUILD="$MANYFORGE_BUILD_DIR/roboplan"

reset_stale_python_build_dir "$CORE_BUILD"
reset_stale_python_build_dir "$PYBIND_BUILD"
reset_stale_python_build_dir "$ROBOPLAN_BUILD"

if [ -f "$CORE_BUILD/libmanyforge_core.a" ]; then
    if ! ls "$PYBIND_BUILD"/manyforge_core_py*.so 1>/dev/null 2>&1; then
        PYBIND11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())" 2>/dev/null || echo "")
        echo "Building pybind11 bindings..."
        mkdir -p "$PYBIND_BUILD" && cd "$PYBIND_BUILD"
        cmake /workspace/manyforge_core/python \
            -DMANYFORGE_CORE_DIR="$CORE_BUILD" \
            -DMANYFORGE_CORE_INCLUDE_DIR="/workspace/manyforge_core/include" \
            ${PYBIND11_DIR:+-Dpybind11_DIR="$PYBIND11_DIR"} 2>/dev/null && \
        make -j"$(nproc)" 2>/dev/null && \
        echo "pybind11 bindings built." || \
        echo "pybind11 build skipped (run: cd $CORE_BUILD && cmake /workspace/manyforge_core && make)"
        cd /workspace
    fi
fi

if [ -d "$PYBIND_BUILD" ]; then
    export PYTHONPATH="$PYBIND_BUILD:${PYTHONPATH}"
fi

if [ -d "$ROBOPLAN_BUILD" ]; then
    export PYTHONPATH="$ROBOPLAN_BUILD:/workspace/manyforge_roboplan/python:${PYTHONPATH}"
fi

exec "$@"
