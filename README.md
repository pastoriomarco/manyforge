# ManyForge

Modular robot execution framework: behavior trees, motion planning, scene
authoring, and an LLM-assisted Composer UI. It currently supports a
self-contained ManyForge-only native lane, Isaac Sim commanded simulation,
and an experimental supervised `real-robot` lane whose first qualified target
is Lite6. Physical support remains robot- and cell-specific: the common
controller/lifecycle contracts do not replace hardware qualification.

> **`dev` migration status:** the composable-cell destructive implementation
> window is active. The v2 node catalog and retained node-environment identity
> are inspectable, but Composer program execution and all three product lanes
> are intentionally unavailable while the old factory/runtime owner is
> removed. Headless resolution returns at I5/H0, native execution at I6/N,
> and Isaac/real operation at I9/I10. The launch recipes below describe the
> last qualified baseline; they are not valid bring-up instructions for the
> current `dev` branch.

> **For LLMs** (Claude, ChatGPT, agentic IDEs): read
> [`AGENTS.md`](AGENTS.md) before making any change. It tells you what
> this repo is, how to operate it at runtime, and the branch + commit
> workflow that applies to LLM contributors. The full contributor
> reference is [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Documentation map

| You want to…                                        | Start here                                                                                                                                         |
| --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Install or launch a supported scenario              | [`docs/operations/LAUNCHER_QUICKSTART.md`](docs/operations/LAUNCHER_QUICKSTART.md)                                                                 |
| Operate, diagnose, or recover a running stack       | [`docs/INDEX.md`](docs/INDEX.md) → operator runbooks                                                                                               |
| Understand architecture and authority boundaries    | [`docs/reference/ARCHITECTURE.md`](docs/reference/ARCHITECTURE.md)                                                                                 |
| Integrate through REST, MCP, or the viewer protocol | [`docs/INDEX.md`](docs/INDEX.md) → generated contracts                                                                                             |
| Add a robot                                         | [`docs/operations/ADDING_A_ROBOT.md`](docs/operations/ADDING_A_ROBOT.md)                                                                           |
| Contribute code or documentation                    | [`CONTRIBUTING.md`](CONTRIBUTING.md); LLM agents also read [`AGENTS.md`](AGENTS.md)                                                                |

The runtime documentation is self-contained in this repository. Private
maintainer specifications are not required to install, operate, or integrate
ManyForge.

## Disclaimer

This software is released under the Apache-2.0 license (see
[`LICENSE`](LICENSE) for details).

**Safety notice:** ManyForge does **not** implement functional safety. Its
fail-closed behaviors (command gating, bounded stops, intervention
acknowledgement) are software-level execution controls, not safety
functions. You **must** integrate appropriate safety measures compliant
with local regulations (hardware e-stops, safety-rated controllers, risk
assessment) before deploying on real hardware.

**Experimental:** the project is under active development; pre-1.0 APIs may
change between minor versions (see [`CHANGELOG.md`](CHANGELOG.md)).

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

A 7-step interactive menu walks you through lane (`manyforge-only`,
`isaac-sim`, or the experimental `real-robot` path), assistant (on / off), and
scenario, then brings up the
stack. See [`docs/operations/LAUNCHER_QUICKSTART.md`](docs/operations/LAUNCHER_QUICKSTART.md)
for the full guide — interactive wizard first, then scripted /
non-interactive use for CI, plus the four-combination smoke-test
matrix.

**Adding a new robot?** See
[`docs/operations/ADDING_A_ROBOT.md`](docs/operations/ADDING_A_ROBOT.md) —
URDF/SRDF requirements, deployment-YAML schema, capability adapter
selection, and smoke-test curls. All operator docs are catalogued at
[`docs/INDEX.md`](docs/INDEX.md).

**Qualifying the physical Lite6?** Use the staged, attended procedure in
[`docs/operations/LITE6_LIVE_QUALIFICATION.md`](docs/operations/LITE6_LIVE_QUALIFICATION.md).
Launcher startup is observation-only; it never enables drives or dispatches a
trajectory.

## Running Tests

All commands run inside the container (`docker compose run --rm dev bash`).

```bash
# Full suite (all supported test/build suites)
./scripts/test-all.sh all

# Individual suites
./scripts/test-all.sh cpp          # all C++ package tests
./scripts/test-all.sh roboplan     # retained-model RoboPlan qualification only
./scripts/test-all.sh bindings     # explicitly unavailable during I2 cutover
./scripts/test-all.sh python       # manyforge_behavior
./scripts/test-all.sh integration  # unavailable until native restoration
./scripts/test-all.sh composer     # Composer backend tests + frontend build
```

The current destructive `dev` checkpoint is not a full-suite qualification:
the aggregate deployment registry intentionally retains stale identity/golden
assertions until the I2/I3 surface freezes, and runtime-dependent Python,
Composer, ROS, and lane suites remain unavailable. Use the focused package
commands recorded by the active plan rather than interpreting an unavailable
suite as a product regression.

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
manyforge_content_identity/  Shared ROS-free canonical JSON and content-digest primitives
manyforge_persistence/   Shared ROS-free mechanical durable-storage safety primitives
manyforge_scene/         ROS-free logical-scene semantic owner and durable state
manyforge_deployment/    ROS-free composable-cell codec, qualification, final C++ trust boundary, and retention substrate
manyforge_component_manager/  ROS-free Manager observation, typed bootstrap journal, owner ports, and active-publication store
manyforge_collision/     C++ planner-neutral collision query implementation
manyforge_roboplan/      Retained planning-model staging and offline RoboPlan import qualification
manyforge_ros2_control_execution/  C++ ROS controller execution and exact-admission adapter
manyforge_robot_lifecycle/  ROS-independent real-robot lifecycle supervisor
manyforge_robot_lifecycle_msgs/  ROS IDL for same-host lifecycle admission transport
manyforge_xarm_adapter/  Experimental xArm model/binding/observation boundary
manyforge_xarm_control/  Experimental xArm lifecycle and sole transport-owner boundary
manyforge_behavior/      Python behavior tree runner (py_trees), node catalog, program loader
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
Program YAML --> Behavior Runner --> ExecutionKernel (C++) --> Planner / execution adapters
                      |                    |                         |
                 Node Catalog       validation + stop       RoboPlan / ros2_control
                 recovery offer     collision authority              |
                                            |                  robot / Isaac
                                   immutable events/facts
                                            |
                                     Composer / viewer
```

The kernel owns terminal and stop authority; ManyForge's collision layer owns
collision admission; the behavior layer owns orchestration, run identity,
and the one versioned recovery offer. Composer owns authoring, presentation,
the non-safety-rated condition-backed alarm manager, and a bounded observer
data plane—not motion authority. For the full component map, deployment lanes, scene
initialization, recovery, and observability model, see
[`docs/reference/ARCHITECTURE.md`](docs/reference/ARCHITECTURE.md).

- **manyforge_core** is ROS-free, pure C++20. All robot-framework-specific code lives in adapter packages.
- **manyforge_roboplan** currently provides retained planning-model staging
  and offline RoboPlan import qualification. Runtime planning/execution is
  intentionally unavailable until the target adapter is restored.
- Python code calls C++ through pybind11 bindings -- no duplicate state machines.
- The Docker container includes all dependencies (Pinocchio, RoboPlan, pybind11).

## Pick-and-Place Demo

Run the validated end-to-end demo with RoboPlan motion planning and Composer's native 3D viewer:

```bash
./scripts/launch.sh --lane manyforge-only --assistant off --scenario ur5-pick-place --non-interactive --yes
```

Then open `http://localhost:9000` for the Composer UI and native 3D viewer.

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
(`docker-entrypoint.sh`) only establishes the runtime environment:

- Sets `PYTHONPATH` for `manyforge_behavior`
- Uses `/tmp/manyforge-build/` for out-of-tree builds (avoids conflicts with host build dirs)
- Refuses a non-writable build root; it never builds or deletes cached products

Canonical `docker compose run --rm dev ...` test containers use an ephemeral
build root. The unified launcher performs its required builds explicitly and
retains them only in a cache bound to the immutable image ID it actually runs.

To rebuild the image after Dockerfile changes:

```bash
docker compose build
```

To rebuild from scratch (e.g. after RoboPlan update):

```bash
rm -rf .docker/roboplan
./scripts/setup-docker.sh
```

### Retained launcher caches

Raw Composer frontend/API debug profiles do not mount compiled-product caches.
Canonical launchers retain native/ROS products only in cache volumes bound to
the immutable runtime image ID. List and remove an exact unused cache using the
guarded procedure in
[`LAUNCHER_QUICKSTART.md`](docs/operations/LAUNCHER_QUICKSTART.md#likely-first-run-gotchas).

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
