#!/bin/bash
set -e

# Add Python packages to PYTHONPATH
# manyforge_behavior lives at /workspace/manyforge_behavior/manyforge_behavior/
# manyforge_ros lives at /workspace/manyforge_ros/manyforge_ros/
# manyforge_hmi lives at /workspace/manyforge_hmi/ (flat layout)
# manyforge_composer lives at /workspace/manyforge_composer/ (flat layout)
export PYTHONPATH="/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace:${PYTHONPATH:-}"

# Use container-specific build directories (avoids cmake cache conflicts
# when the same source tree is also built on the host)
export MANYFORGE_BUILD_DIR="/tmp/manyforge-build"

# Fix ownership if the build-cache volume was created by an older root-based
# image.  The Dockerfile now runs as uid 1000 (manyforge), but Docker named
# volumes preserve the ownership from their first creation.
if [ -d "$MANYFORGE_BUILD_DIR" ] && [ ! -w "$MANYFORGE_BUILD_DIR" ]; then
    echo "Build cache not writable by $(whoami) — clearing stale root-owned files..."
    echo "Run:  docker compose down -v  to remove the stale volume, then retry."
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

# Make cmeel-installed CMake packages discoverable for standalone native builds.
# Pinocchio/coal/eigenpy install under a Python-managed prefix rather than a
# standard system prefix, so plain `cmake -S manyforge_roboplan` needs help.
PINOCCHIO_CMAKE_DIR=$(find /usr -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1 || true)
if [ -n "${PINOCCHIO_CMAKE_DIR:-}" ]; then
    PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "$PINOCCHIO_CMAKE_DIR")")")
    export CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH:-}"
    if [ -f "${PINOCCHIO_PREFIX}/include/boost/version.hpp" ]; then
        export BOOST_ROOT="${PINOCCHIO_PREFIX}"
        export BOOST_INCLUDEDIR="${PINOCCHIO_PREFIX}/include"
        export BOOST_LIBRARYDIR="${PINOCCHIO_PREFIX}/lib"
    fi
fi

# Build pybind11 module if manyforge_core library exists
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

# Add pybind11 module to Python path
if [ -d "$PYBIND_BUILD" ]; then
    export PYTHONPATH="$PYBIND_BUILD:${PYTHONPATH}"
fi

# Add roboplan pybind11 module + Python wrappers to Python path
if [ -d "$ROBOPLAN_BUILD" ]; then
    export PYTHONPATH="$ROBOPLAN_BUILD:/workspace/manyforge_roboplan/python:${PYTHONPATH}"
fi

exec "$@"
