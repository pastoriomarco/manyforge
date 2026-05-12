# ManyForge v0.0 Development Environment
#
# Provides the complete build/test toolchain:
#   - C++20 (g++, cmake, gtest, eigen3)
#   - Python 3.12 (pytest, py_trees, pybind11, numpy, viser, pyyaml)
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
#   cd /workspace && ./scripts/test-all.sh all

# --- Stage 1: Build React frontend ---
FROM node:22-slim AS frontend-builder
WORKDIR /build
COPY manyforge_composer/frontend/package.json manyforge_composer/frontend/package-lock.json* ./
RUN npm ci --ignore-scripts
COPY manyforge_composer/frontend/ ./
RUN npm run build

# --- Stage 2: Python/C++ runtime ---
FROM python:3.12-slim-bookworm

ENV DEBIAN_FRONTEND=noninteractive
# Pinocchio (cmeel package) installs shared libs under a Python-managed prefix.
# The cmeel lib dir is added to the dynamic linker cache below; keep LD_LIBRARY_PATH
# minimal so the image does not depend on a specific Python minor version in its paths.
ENV LD_LIBRARY_PATH=/usr/local/lib

# C++ build toolchain
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build \
        libgtest-dev libgmock-dev \
        libboost-filesystem-dev libboost-serialization-dev libboost-system-dev \
        libboost-python-dev libboost-chrono-dev libboost-thread-dev libboost-date-time-dev \
        libeigen3-dev libyaml-cpp-dev liburdfdom-dev \
        liboctomap-dev \
        git curl ca-certificates \
    && cd /usr/src/googletest && cmake . && make \
    && cp lib/*.a /usr/lib/ \
    && rm -rf /var/lib/apt/lists/*

# Python packages: testing, behavior trees, bindings, visualization
RUN pip install --no-cache-dir --upgrade pip setuptools && \
    pip install --no-cache-dir \
        pytest==9.0.3 \
        numpy==2.3.5 \
        py_trees==2.4.0 \
        pybind11==3.0.3 \
        pyyaml==6.0.3 \
        pillow==10.2.0 \
        trimesh==4.11.5 \
        nanobind==2.12.0 \
        scikit-build-core==0.12.2 \
        pin==3.9.0 \
        viser==1.0.26 \
        fastapi==0.135.3 \
        "uvicorn[standard]==0.44.0" \
        httpx==0.28.1 \
        websockets==16.0 \
        mcp==1.27.0 \
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

RUN useradd --create-home --shell /bin/bash --uid 1000 manyforge \
    && mkdir -p /tmp/manyforge-build /workspace \
    && chown -R manyforge:manyforge /tmp/manyforge-build /home/manyforge /workspace

# Node.js 22 for frontend rebuilds in the dev container
COPY --from=frontend-builder /usr/local/bin/node /usr/local/bin/node
COPY --from=frontend-builder /usr/local/lib/node_modules/npm /usr/local/lib/node_modules/npm
RUN ln -sf /usr/local/lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
    && ln -sf /usr/local/lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx

# Copy built frontend from Stage 1
COPY --from=frontend-builder --chown=manyforge:manyforge /build/dist/ /opt/manyforge-frontend/dist/

# Install manyforge_behavior in editable mode (mounted at runtime)
# This is done via an entrypoint script so the volume mount exists
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

USER manyforge

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["bash"]
