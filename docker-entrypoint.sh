#!/bin/bash
set -e

# Add Python packages to PYTHONPATH
# manyforge_behavior lives at /workspace/manyforge_behavior/manyforge_behavior/
# manyforge_hmi lives at /workspace/manyforge_hmi/ (flat layout)
export PYTHONPATH="/workspace/manyforge_behavior:/workspace:${PYTHONPATH:-}"

# Use container-specific build directories (avoids cmake cache conflicts
# when the same source tree is also built on the host)
export MANYFORGE_BUILD_DIR="/tmp/manyforge-build"

# Build pybind11 module if manyforge_core library exists
CORE_BUILD="$MANYFORGE_BUILD_DIR/core"
PYBIND_BUILD="$MANYFORGE_BUILD_DIR/pybind"

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
ROBOPLAN_BUILD="$MANYFORGE_BUILD_DIR/roboplan"
if [ -d "$ROBOPLAN_BUILD" ]; then
    export PYTHONPATH="$ROBOPLAN_BUILD:/workspace/manyforge_roboplan/python:${PYTHONPATH}"
fi

exec "$@"
