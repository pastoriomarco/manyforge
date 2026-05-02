# ManyForge

Internal docs entrypoint: [`docs/INDEX.md`](docs/INDEX.md)

Modular robot execution framework: behavior trees, motion planning, and visualization.

## Quick Start

Requires Docker (with BuildKit) and git.

```bash
# 1. Clone RoboPlan dependency and build the Docker image (~10 min first time)
./scripts/setup-docker.sh

# 2. Enter the dev container
docker compose run --rm dev bash

# 3. Run all tests (builds C++ automatically)
./scripts/test-all.sh
```

## Running Tests

All commands run inside the container (`docker compose run --rm dev bash`).

```bash
# Full suite (all supported test/build suites)
./scripts/test-all.sh all

# Individual suites
./scripts/test-all.sh cpp          # manyforge_core C++ tests
./scripts/test-all.sh roboplan     # manyforge_roboplan C++ tests
./scripts/test-all.sh bindings     # pybind11 bindings
./scripts/test-all.sh python       # manyforge_behavior + manyforge_hmi
./scripts/test-all.sh integration  # end-to-end integration
./scripts/test-all.sh composer     # Composer backend tests + frontend build
```

ROS adapter tests use the separate ROS Jazzy image:

```bash
docker compose --profile ros run --rm ros-jazzy ./scripts/test-ros.sh
```

The ROS image starts from `ros:jazzy-ros-base-noble` and keeps ROS Python
dependencies apt-managed. The default development image remains ROS-free; ROS
wrappers/adapters live in the `manyforge_ros` lane.

Full ManyForge + ROS integration checks use the heavier ROS Jazzy full image:

```bash
docker compose --profile ros-full run --rm ros-jazzy-full ./scripts/test-ros-full.sh
```

The full image also starts from `ros:jazzy-ros-base-noble`, but adds a pinned
ManyForge application venv with `--system-site-packages` so ROS Python remains
apt-managed while Composer/FastAPI/Pydantic/RoboPlan app dependencies are
reproducible.

To build C++ manually:

```bash
cd /tmp/manyforge-build/core
cmake /workspace/manyforge_core -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure
```

## Project Structure

```
manyforge_core/          C++ library: execution kernel, state machine, DTOs, ports
  python/                pybind11 bindings exposing the kernel to Python
manyforge_roboplan/      C++ RoboPlan adapters: planner, planning context, execution backend
  python/                pybind11 bindings + NativeKernelClient wrapper
manyforge_behavior/      Python behavior tree runner (py_trees), node catalog, program loader
manyforge_hmi/           Python Viser-based HMI: robot visualization, control panel
manyforge_composer/      Composer: FastAPI backend, React frontend, MCP server
  backend/               REST API routes, state management, security middleware
  frontend/              React + ReactFlow program editor UI
  tests/                 Backend + MCP server tests
manyforge_ros/           ROS adapters/wrappers and deterministic ROS test targets
tests/                   End-to-end integration and benchmark tests
scripts/                 Build/test/setup scripts
docs/                    Architecture reference
```

## Architecture

```
Program YAML --> Behavior Runner --> ExecutionKernel (C++) --> Planner/Execution backends
                      |                    |
                 Node Catalog        State Machine
                                    Trajectory Cache
                                    Collision Policy
```

- **manyforge_core** is ROS-free, pure C++20. All robot-framework-specific code lives in adapter packages.
- **manyforge_roboplan** provides real motion planning via RoboPlan (RRT + TOPPRA + IK).
- Python code calls C++ through pybind11 bindings -- no duplicate state machines.
- The Docker container includes all dependencies (Pinocchio, RoboPlan, Viser, pybind11).

## Pick-and-Place Demo

Run the validated end-to-end demo with real RoboPlan motion planning, Composer, and Viser 3D visualization:

```bash
./scripts/demo.sh
```

Then open:
- `http://localhost:9000` for the Composer UI
- `http://localhost:8081` for the Viser 3D view

The pick-and-place demo exercises the full program load, native planning, scene-edit, run, reset, and rerun flow through the supported container path.

## LLM Operability

ManyForge exposes a full API surface for autonomous LLM agent operation:

- **REST API** — FastAPI backend at `http://localhost:9000/api/`. Development
  guidance for LLM composition lives in
  `../manyforge_specs/docs/guides/LLM_GUIDE.md`.
- **MCP Server** — Model Context Protocol interface over stdio. The MCP tool
  contract lives in `../manyforge_specs/docs/reference/MCP_SCHEMA.md`.

```bash
# Start the MCP server
python -m manyforge_composer --mcp --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml
```

## Docker Details

The container runs as a non-root user (`manyforge`, uid 1000). The entrypoint
(`docker-entrypoint.sh`) automatically:
- Sets `PYTHONPATH` for `manyforge_behavior` and `manyforge_hmi`
- Builds pybind11 bindings if the C++ core library exists
- Uses `/tmp/manyforge-build/` for out-of-tree builds (avoids conflicts with host build dirs)
- Detects stale root-owned build cache volumes and asks you to remove them

To rebuild the image after Dockerfile changes:

```bash
docker compose build
```

To rebuild from scratch (e.g. after RoboPlan update):

```bash
rm -rf .docker/roboplan
./scripts/setup-docker.sh
```

### Upgrading from root-based images

Older images ran as root. After rebuilding with the current Dockerfile, the
`build-cache` Docker volume may still contain root-owned files that the new
non-root user cannot write to. If you see "Build cache not writable" on
startup, run:

```bash
docker compose down -v
```

This removes the stale volume. The next container start will recreate it with
the correct ownership. The C++ build cache will be rebuilt automatically (~2
min).
