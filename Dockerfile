# ManyForge v0.0 Development Environment
#
# Provides the complete build/test toolchain:
#   - C++20 (g++, cmake, gtest, eigen3)
#   - Python 3.12 (pytest, py_trees, pybind11, numpy, pyyaml)
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
        libeigen3-dev libyaml-cpp-dev libyaml-dev liburdfdom-dev \
        libdouble-conversion-dev libexpat1-dev libssl-dev libutf8proc-dev \
        liboctomap-dev libsqlite3-dev \
        git curl ca-certificates \
    && cd /usr/src/googletest && cmake . && make \
    && cp lib/*.a /usr/lib/ \
    && rm -rf /var/lib/apt/lists/*

# Python packages: testing, behavior trees, bindings, visualization
# Pin the cmeel/pinocchio dependency set to a self-consistent Pinocchio 4 stack.
# `pin==4.1.0` and `coal==3.0.3` link urdfdom 6 (liburdfdom_*.so.6), so
# `cmeel-urdfdom` is pinned to 6.0.0 to keep the whole stack on one urdfdom
# soname; an older/unpinned urdfdom would reintroduce a `liburdfdom_*.so.X`
# ImportError at the RoboPlan bindings build. urdfdom 6 in turn links tinyxml2
# 11 (libtinyxml2.so.11), so `cmeel-tinyxml2` is pinned to 11.0.0 to match
# (10.0.0 ships only .so.10 and breaks urdfdom 6 at load time). Pinocchio 4
# also splits its C++
# libraries into `libpinocchio`/`libcoal`, pinned explicitly so they cannot
# float (they ship the pinocchio/coal headers + .so the C++ build links
# against). A pip constraints file enforces the compatible set regardless of
# resolution order.
RUN pip install --no-cache-dir --upgrade pip setuptools && \
    printf '%s\n' \
        'pin==4.1.0' \
        'libpinocchio==4.1.0' \
        'coal==3.0.3' \
        'libcoal==3.0.3' \
        'cmeel-urdfdom==6.0.0' \
        'eigenpy==3.13.0' \
        'cmeel==0.60.0' \
        'cmeel-assimp==6.0.5' \
        'cmeel-boost==1.90.0' \
        'cmeel-console-bridge==1.0.2.3' \
        'cmeel-octomap==1.10.0' \
        'cmeel-qhull==8.0.2.1' \
        'cmeel-tinyxml2==11.0.0' \
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
        pin==4.1.0 \
        fastapi==0.135.3 \
        "uvicorn[standard]==0.44.0" \
        httpx==0.28.1 \
        websockets==16.0 \
        mcp==1.27.0 \
    && pip check \
    && CMEEL_LIB=$(python3 -c "import site; print(site.getsitepackages()[0])")/cmeel.prefix/lib \
    && if [ -d "$CMEEL_LIB" ]; then echo "$CMEEL_LIB" > /etc/ld.so.conf.d/cmeel.conf && ldconfig; fi

# RoboPlan: C++ planning library with Python bindings (nanobind)
# Pre-cloned on host — see scripts/setup-docker.sh
COPY .docker/roboplan/ /tmp/roboplan/
COPY third_party/roboplan/ /tmp/roboplan-manifest/
COPY scripts/ci/check-roboplan-dependency-stack.py /tmp/check-roboplan-dependency-stack.py
RUN PINOCCHIO_CMAKE_DIR=$(find /usr -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1) \
    && PINOCCHIO_PREFIX=$(dirname $(dirname $(dirname $PINOCCHIO_CMAKE_DIR))) \
    && echo "Pinocchio prefix: ${PINOCCHIO_PREFIX}" \
    && cd /tmp/roboplan-manifest \
    && sha256sum -c patches.sha256 \
    && while read -r digest patch; do \
         case "$patch" in patches/*.patch) ;; *) echo "Invalid RoboPlan patch path: $patch" >&2; exit 1;; esac; \
         git -C /tmp/roboplan apply --check "/tmp/roboplan-manifest/$patch"; \
         git -C /tmp/roboplan apply "/tmp/roboplan-manifest/$patch"; \
       done < patches.sha256 \
    && cd /tmp/roboplan \
    && export CMAKE_PREFIX_PATH="${PINOCCHIO_PREFIX}:${CMAKE_PREFIX_PATH}" \
    && export LD_LIBRARY_PATH="${PINOCCHIO_PREFIX}/lib:${LD_LIBRARY_PATH}" \
    && ROBOPLAN_CMAKE_ARGS=-DBUILD_TESTING=OFF ./scripts/build_cmake.bash \
    && ROBOPLAN_INSTALL=/tmp/roboplan/install \
    && mkdir -p /usr/local/lib/cmake /usr/local/include /usr/local/share \
    && for pkg in roboplan roboplan_rrt roboplan_simple_ik roboplan_toppra roboplan_oink roboplan_cartesian_planning roboplan_example_models toppra; do \
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
    && for py_pkg in roboplan roboplan_example_models roboplan_simple_ik roboplan_oink roboplan_toppra roboplan_rrt roboplan_cartesian_planning; do \
         cd "/tmp/roboplan/${py_pkg}"; \
         pip install --no-build-isolation --no-deps --no-cache-dir -v .; \
       done \
    && pip check \
    && python3 /tmp/check-roboplan-dependency-stack.py \
    && rm -rf /tmp/roboplan /tmp/check-roboplan-dependency-stack.py

WORKDIR /workspace

# Source-mounted development images expose the same CLI name as an installed
# manyforge_behavior package. ENV (rather than entrypoint-only export) keeps it
# available to `docker exec` operator workflows as well as PID 1.
ENV PATH="/workspace/scripts:${PATH}"

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

# The source-mounted behavior package is exposed through PYTHONPATH by the
# entrypoint; /workspace/scripts/manyforge is its container CLI shim.
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

USER manyforge

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["bash"]
