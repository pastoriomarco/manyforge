# ManyForge v0.0 Development Environment
#
# Provides the complete build/test toolchain:
#   - C++17 (g++, cmake, gtest, eigen3)
#   - Python 3.11 (pytest, py_trees, pybind11, numpy, viser, pyyaml)
#   - Pinocchio (robot kinematics/dynamics/collision)
#   - RoboPlan (motion planning: RRT, TOPPRA, IK, collision checking)
#   - Node.js 22 (React frontend build)
#
# Build:
#   # One-time: pre-clone RoboPlan (recursive submodule clone fails inside Docker)
#   ./scripts/setup-docker.sh
#
#   # Or manually:
#   docker compose build
#
# Usage:
#   docker compose run --rm dev bash
#   # Inside container:
#   cd /workspace/manyforge_core/build && cmake .. && make -j$(nproc) && ctest
#   cd /workspace && pytest manyforge_behavior/tests/

# --- Stage 1: Build React frontend ---
FROM node:22-slim AS frontend-builder
WORKDIR /build
COPY manyforge_composer/frontend/package.json manyforge_composer/frontend/package-lock.json* ./
RUN npm ci --ignore-scripts
COPY manyforge_composer/frontend/ ./
RUN npm run build

# --- Stage 2: Python/C++ runtime ---
FROM python:3.11-slim-bookworm

ENV DEBIAN_FRONTEND=noninteractive
# Pinocchio (cmeel package) installs shared libs under site-packages/cmeel.prefix.
# We add both site-packages and dist-packages paths for portability.
ENV LD_LIBRARY_PATH=/usr/local/lib/python3.11/site-packages/cmeel.prefix/lib:/usr/local/lib/python3.11/dist-packages/cmeel.prefix/lib:/usr/local/lib

# C++ build toolchain
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build \
        libgtest-dev libgmock-dev \
        libeigen3-dev libyaml-cpp-dev liburdfdom-dev \
        git curl ca-certificates \
    && cd /usr/src/googletest && cmake . && make \
    && cp lib/*.a /usr/lib/ \
    && rm -rf /var/lib/apt/lists/*

# Python packages: testing, behavior trees, bindings, visualization
RUN pip install --no-cache-dir --upgrade pip setuptools && \
    pip install --no-cache-dir \
        pytest numpy py_trees pybind11 pyyaml \
        nanobind scikit-build-core \
        pin==3.7.0 \
        "viser>=1.0.1" \
        fastapi "uvicorn[standard]" httpx websockets \
    && CMEEL_LIB=$(python3 -c "import site; print(site.getsitepackages()[0])")/cmeel.prefix/lib \
    && if [ -d "$CMEEL_LIB" ]; then echo "$CMEEL_LIB" > /etc/ld.so.conf.d/cmeel.conf && ldconfig; fi

# RoboPlan: C++ planning library with Python bindings (nanobind)
# Pre-cloned on host — see scripts/setup-docker.sh
COPY .docker/roboplan/ /tmp/roboplan/
RUN PINOCCHIO_CMAKE_DIR=$(find /usr -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1) \
    && PINOCCHIO_PREFIX=$(dirname $(dirname $(dirname $PINOCCHIO_CMAKE_DIR))) \
    && echo "Pinocchio prefix: ${PINOCCHIO_PREFIX}" \
    && cd /tmp/roboplan \
    && export CMAKE_PREFIX_PATH="${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH}" \
    && export LD_LIBRARY_PATH="${PINOCCHIO_PREFIX}/lib:${LD_LIBRARY_PATH}" \
    && ./scripts/build_cmake.bash \
    && ROBOPLAN_INSTALL=/tmp/roboplan/install \
    && mkdir -p /usr/local/lib/cmake /usr/local/include /usr/local/share \
    && for pkg in roboplan roboplan_rrt roboplan_simple_ik roboplan_toppra roboplan_oink roboplan_example_models toppra; do \
         if [ -d "${ROBOPLAN_INSTALL}/${pkg}/lib" ]; then \
           cp -a ${ROBOPLAN_INSTALL}/${pkg}/lib/*.so* /usr/local/lib/ 2>/dev/null || true; \
           cp -a ${ROBOPLAN_INSTALL}/${pkg}/lib/*.a /usr/local/lib/ 2>/dev/null || true; \
         fi; \
         if [ -d "${ROBOPLAN_INSTALL}/${pkg}/include" ]; then \
           cp -a ${ROBOPLAN_INSTALL}/${pkg}/include/* /usr/local/include/; \
         fi; \
         if [ -d "${ROBOPLAN_INSTALL}/${pkg}/lib/cmake" ]; then \
           cp -a ${ROBOPLAN_INSTALL}/${pkg}/lib/cmake/* /usr/local/lib/cmake/; \
         fi; \
         if [ -d "${ROBOPLAN_INSTALL}/${pkg}/share" ]; then \
           cp -a ${ROBOPLAN_INSTALL}/${pkg}/share/* /usr/local/share/; \
         fi; \
       done \
    && ldconfig \
    && cd /tmp/roboplan/bindings \
    && CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH}" \
       pip install --no-build-isolation --no-cache-dir -v . \
    && rm -rf /tmp/roboplan

WORKDIR /workspace

# Copy built frontend from Stage 1
COPY --from=frontend-builder /build/dist/ /opt/manyforge-frontend/dist/

# Install manyforge_behavior in editable mode (mounted at runtime)
# This is done via an entrypoint script so the volume mount exists
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["bash"]
