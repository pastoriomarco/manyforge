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
# Pin the cmeel/pinocchio dependency set to the versions verified in the
# working Thor manyforge-dev image. `pin==3.9.0`'s wheel links against
# urdfdom 4.0 (liburdfdom_*.so.4.0); leaving the transitive `cmeel-urdfdom`
# unpinned lets pip pull v6 (liburdfdom_*.so.6), which breaks the RoboPlan
# bindings wheel build with `ImportError: liburdfdom_sensor.so.4.0`. A pip
# constraints file enforces the compatible set regardless of resolution order.
RUN pip install --no-cache-dir --upgrade pip setuptools && \
    printf '%s\n' \
        'pin==3.9.0' \
        'cmeel-urdfdom==4.0.1' \
        'coal==3.0.2' \
        'eigenpy==3.12.0' \
        'cmeel==0.60.0' \
        'cmeel-assimp==6.0.2' \
        'cmeel-boost==1.89.0' \
        'cmeel-console-bridge==1.0.2.3' \
        'cmeel-octomap==1.10.0' \
        'cmeel-qhull==8.0.2.1' \
        'cmeel-tinyxml2==10.0.0' \
        'cmeel-zlib==1.3.1' \
        > /tmp/manyforge-cmeel-constraints.txt && \
    pip install --no-cache-dir -c /tmp/manyforge-cmeel-constraints.txt \
        pytest==9.0.3 \
        numpy==2.3.5 \
        py_trees==2.4.0 \
        pybind11==3.0.3 \
        pyyaml==6.0.3 \
        pillow==10.2.0 \
        trimesh==4.11.5 \
        yourdfpy==0.0.60 \
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
    && sed -i 's#-DCMAKE_INSTALL_PREFIX#-DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX#' scripts/build_cmake.bash \
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
    && export CMAKE_PREFIX_PATH="/usr/local:${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH}" \
    && for py_pkg in roboplan roboplan_example_models roboplan_simple_ik roboplan_oink roboplan_toppra roboplan_rrt; do \
         cd "/tmp/roboplan/${py_pkg}"; \
         pip install --no-build-isolation --no-deps --no-cache-dir -v .; \
       done \
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
