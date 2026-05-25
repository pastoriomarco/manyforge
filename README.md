# ManyForge

Modular robot execution framework: behavior trees, motion planning, scene
authoring, and an LLM-assisted Composer UI. It currently supports
Composer-only authoring and Isaac Sim commanded simulation, and is structured
for qualified real-robot deployments as that lane matures.

> **For LLMs** (Claude, ChatGPT, agentic IDEs): read
> [`AGENTS.md`](AGENTS.md) before making any change. It tells you what
> this repo is, how to operate it at runtime, and the branch + commit
> workflow that applies to LLM contributors. The full contributor
> reference is [`CONTRIBUTING.md`](CONTRIBUTING.md).

Internal docs entrypoint: [`docs/INDEX.md`](docs/INDEX.md)

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

**Bringing up the stack?** Just run the launcher with no arguments
and follow the prompts — same shape as the NemoClaw onboard wizard:

```bash
./scripts/launch.sh
```

A 7-step interactive menu walks you through lane (manyforge-only /
isaac-sim), assistant (on / off), and scenario, then brings up the
stack. See [`docs/operations/LAUNCHER_QUICKSTART.md`](docs/operations/LAUNCHER_QUICKSTART.md)
for the full guide — interactive wizard first, then scripted /
non-interactive use for CI, plus the four-combination smoke-test
matrix.

**Adding a new robot?** See
[`docs/operations/ADDING_A_ROBOT.md`](docs/operations/ADDING_A_ROBOT.md) —
URDF/SRDF requirements, deployment-YAML schema, capability adapter
selection, and smoke-test curls. All operator docs are catalogued at
[`docs/INDEX.md`](docs/INDEX.md).

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
reproducible. `test-ros-full.sh` rebuilds and exports the ROS-enabled native
RoboPlan module before running tests, so it is the canonical local command for
ros2_control contract coverage.

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
- **manyforge_roboplan** provides motion planning via RoboPlan (RRT + TOPPRA + IK).
- Python code calls C++ through pybind11 bindings -- no duplicate state machines.
- The Docker container includes all dependencies (Pinocchio, RoboPlan, Viser, pybind11).

## Pick-and-Place Demo

Run the validated end-to-end demo with RoboPlan motion planning, Composer, and Viser 3D visualization:

```bash
./scripts/launch.sh --lane manyforge-only --assistant off --scenario ur5-pick-place --non-interactive --yes
```

Then open:
- `http://localhost:9000` for the Composer UI
- `http://localhost:8081` for the Viser 3D view

The pick-and-place demo exercises the full program load, native planning, scene-edit, run, reset, and rerun flow through the supported container path.

`./scripts/demo.sh` is a backwards-compat shim that forwards to the
command above (with `--simple` switching to the `ur5-simple-pick-place`
scenario). New automation should call `launch.sh` directly; see
[`docs/operations/LAUNCHER_QUICKSTART.md`](docs/operations/LAUNCHER_QUICKSTART.md)
for all scenarios and the `--list-scenarios` verb.

## LLM Operability

ManyForge exposes a full API surface for autonomous LLM agent operation:

- **REST API** — FastAPI backend at `http://localhost:9000/api/`. Operator-
  facing reference: [`docs/reference/REST_API.md`](docs/reference/REST_API.md).
  The live OpenAPI schema is also available at `http://localhost:9000/docs`
  and `http://localhost:9000/openapi.json` once Composer is running.
- **MCP Server** — Model Context Protocol interface over stdio. Operator-
  facing tool contract:
  [`docs/reference/MCP_RUNTIME_CONTRACT.md`](docs/reference/MCP_RUNTIME_CONTRACT.md).
- **Assistant Provider Bridge** — HTTP shim that mediates between Composer
  and an LLM endpoint. Wire-level contract:
  [`docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md).

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

## Versioning, releases, security

- **Current version**: see [`VERSION`](VERSION) (single line, SemVer).
  Sub-packages match this version; CI enforces with
  `./scripts/ci/verify-version-sync.sh`.
- **What changed**: [`CHANGELOG.md`](CHANGELOG.md) (Keep-a-Changelog format).
- **What's next**: [`ROADMAP.md`](ROADMAP.md).
- **License**: Apache 2.0 — see [`LICENSE`](LICENSE) and
  [`NOTICE`](NOTICE) for third-party attribution.
- **Reporting a security vulnerability**: see
  [`SECURITY.md`](SECURITY.md). Do NOT use public issues for security
  topics.

## Contributing

Both human contributors and LLM agents are first-class. The branch
workflow (`main` = released, `dev` = integration where work lands,
squash-merge `dev → main` on release) and the LLM-specific commit/push
rules are documented in [`CONTRIBUTING.md`](CONTRIBUTING.md) and
reinforced in [`AGENTS.md`](AGENTS.md).

ManyForge development is spec-driven by private maintainer specs. Those
specs are not needed to install, run, or operate ManyForge, but
non-trivial behavior changes need maintainer audit against them.
Without that context, contributions should stay to bounded fixes,
tests, docs, or proposals that explicitly call out the audit need.

Tested with **NemoClaw-Thor 0.1.x** for the assistant lane on Jetson
Thor — see the companion repo at
<https://github.com/pastoriomarco/NemoClaw-Thor>.
