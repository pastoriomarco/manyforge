# ManyForge

Repository for a modular robot-execution framework built around headless C++
semantic owners and thin authoring/operator clients. The maintained
ManyForge-only native path is current on `dev` through explicit owner
entrypoints, including generic component lifecycle/controller routes and
addressed multi-participant execution, Stop, surveillance, and recovery.
Packaged installation, the historical aggregate launcher, and the Isaac, ROS,
external-agent, and real-robot product lanes are not yet available.

> **`dev` migration status:** the composable-cell destructive implementation
> window is active. I2's pure ROS-free compiler and `manyforge_core_py`
> compiler/DTO projection are qualified at executable commit `0ee506a...`.
> I3's retained deployment compiler is qualified at implementation freeze
> `62df57c...`: deterministic planning/collision, viewer, control-domain,
> reference runtime-unit, and projection-input products can be generated,
> durably reloaded, and verified offline. I4 now owns the bounded internal M0
> path that Applies canonical empty to the retained reference runtime, retains
> it across Manager replacement, reconciles crash/restart, and shuts it down
> explicitly. I5/H0 now exposes the independent Manager/headless API, CLI,
> alarms/history, verified NodeEnvironment projection, and thin Composer read
> clients. At I6/N3, one bounded native absolute joint-goal path first crossed
> an independently restartable C++ owner, headless API, Behavior caller, and
> Composer proxy; that checkpoint alone was not the complete native lane. N6-c1
> and N6-c2 then added a bounded addressed
> four-mode Robot Control workflow: joint goals, pose goals, Cartesian paths,
> and gripper commands with source-local no-motion preview, explicit approval,
> owner-issued progress, and operation Stop. That checkpoint completed the
> maintained manual-control breadth but not yet the full product join. N6-c3
> additionally
> restores the strict Program
> Draft/Saved workspace: environment-aware palette/tree/property/declaration
> authoring plus bounded Open-as-Draft, Save, and Save-As. It does not Apply or
> execute Programs. N6-c4 adds source-local scene/viewer authoring and one
> exact owner-routed current-pose Commit without restoring dynamic viewer
> truth. N6-c5 adds the strict bounded direct/`nemoclaw` assistant bridge,
> Draft-only callbacks, and operator conversation/reconciliation surface; its
> live model/launcher lane and external OpenClaw/Hermes adapters remain
> unqualified. N7-a5.1-a5.4 now add exact Program admission and Apply, the
> captured native motion/gripper/scene/model join, a fixed-pool Program tick
> host, an independently restartable Program-runner process, and strict
> Headless/Composer scenario, Applied, Run/status, and one-click Stop clients.
> Composer remains observational, and closing any client never implies Stop.
> N7 closes the maintained seven-input/eight-Program native path and
> unchanged-source repeated-instance isolation through the Component Manager,
> Headless API, Program runner, and thin Composer clients. I7/L0 adds the sole
> generic lifecycle/controller-route owner and verified readiness publication.
> I8 adds prepared multi-participant dispatch and Resume, asynchronous
> Pause/Reset/Acknowledge, public scoped Stop and Full Stop, collision
> containment, immutable terminal/fault truth, durable surveillance/recovery,
> and scoped orphan reconciliation. Isaac and real operation remain I9/I10
> work, and the historical aggregate launcher and packaged installation remain
> unavailable. The historical launch recipes below are not valid bring-up
> instructions for the current `dev` branch.

> **For LLMs** (Claude, ChatGPT, agentic IDEs): read
> [`AGENTS.md`](AGENTS.md) before making any change. It tells you what
> this repo is, which runtime surfaces are unavailable, how to interpret the
> historical operator material, and the branch + commit workflow that applies
> to LLM contributors. The full contributor
> reference is [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Documentation map

| You want to…                                        | Start here                                                                                                                                 |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| Start the current development workbench             | [`docs/operations/DEVELOPMENT_WORKBENCH.md`](docs/operations/DEVELOPMENT_WORKBENCH.md)                                                      |
| Study historical operator workflows                 | [`docs/INDEX.md`](docs/INDEX.md) → runbooks, each marked as restoration input                                                              |
| Understand architecture and authority boundaries    | [`docs/reference/ARCHITECTURE.md`](docs/reference/ARCHITECTURE.md)                                                                         |
| Inspect current REST or historical client contracts | [`docs/INDEX.md`](docs/INDEX.md) → structural REST and historical interaction references                                                   |
| Study the historical robot-import workflow          | [`docs/operations/ADDING_A_ROBOT.md`](docs/operations/ADDING_A_ROBOT.md); v0 migration/reference input until the replacement gates qualify |
| Contribute code or documentation                    | [`CONTRIBUTING.md`](CONTRIBUTING.md); LLM agents also read [`AGENTS.md`](AGENTS.md)                                                        |

The public structural-shell and historical operator documentation is
self-contained in this repository. Private maintainer specifications are not
needed to inspect those surfaces. The maintained native path is available
through explicit owner entrypoints; packaged installation, historical aggregate
bring-up, and the I9-I11 product lanes are still unavailable.

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

# 3. Run the explicit retained aggregate qualification in the container
./scripts/test-all.sh retained
```

**Working on the current Composer UI?** Start the complete simulated native
development workbench:

```bash
MANYFORGE_DEV_QUALIFIED_BUILD_ROOT=/path/to/qualified/build \
  ./scripts/compose-dev.sh start --scenario lite6_pick_and_place
```

The build-root setting is needed only when importing a compatible
`maintained-package/` into a fresh image-keyed cache. Obtain that artifact with
its matching source revision from the maintainer/distributor; building the image
alone does not provide it. Later starts use `./scripts/compose-dev.sh start
--scenario lite6_pick_and_place`; `status` and `stop` are separate commands. The launcher
incrementally builds stale Manager/Headless, native execution/candidate-worker
and Deployment authoring compiler targets with their dependencies, starts owners in order,
establishes the exact active Deployment and Applied Program/Binding, and serves
Vite HMR at `http://localhost:3000`. It does not run tests, sanitizers,
qualification, package generation, or proof generation. The
selected workbench lane is simulated and cannot authorize physical execution.
See the
[`development workbench runbook`](docs/operations/DEVELOPMENT_WORKBENCH.md).
Installed product launch and distribution remain later gates.

**Restoring or designing the new robot-import workflow?** See
[`docs/operations/ADDING_A_ROBOT.md`](docs/operations/ADDING_A_ROBOT.md) —
historical URDF/SRDF requirements, v0 artifact behavior, adapter-selection
intent, and smoke acceptance inputs. It is not the current artifact schema or
a runnable recipe. All operator docs are catalogued at
[`docs/INDEX.md`](docs/INDEX.md).

**Studying the former physical Lite6 qualification?** The staged procedure in
[`docs/operations/LITE6_LIVE_QUALIFICATION.md`](docs/operations/LITE6_LIVE_QUALIFICATION.md)
is pinned as I10/R reimplementation and acceptance input. Do not run it from
the current branch; the real-robot product lane and its launcher are
unavailable.

## Running Tests

All commands run inside the container (`docker compose run --rm dev bash`).

```bash
# Current retained structural/offline aggregate
./scripts/test-all.sh retained

# Individual suites
./scripts/test-all.sh cpp          # all C++ package tests
./scripts/test-all.sh roboplan     # retained-model RoboPlan qualification only
./scripts/test-all.sh bindings     # ROS-free I2 compiler bindings and API contract
./scripts/test-all.sh python       # manyforge_behavior
./scripts/test-all.sh integration  # historical aggregate selector; still unavailable
./scripts/test-all.sh composer     # structural Composer tests + frontend build
```

The historical no-argument and `all` forms intentionally return
`composable_cell_runtime_unavailable`: they formerly meant the complete
product matrix and must not silently succeed after that matrix was narrowed.
Use the named retained or focused selectors above.

The generic `all`/`integration` script selectors are not current product-suite
qualification entrypoints. Current I7/I8 qualification uses the bounded CMake
targets and maintained-native process workflows owned by those milestones.
The `bindings` selector builds and tests the qualified ROS-free
`manyforge_core_py` compiler surface and generated API contract. I2 closed on
the exact executable commit `0ee506a0d1c05006d2d0f5356cc93db17dca0b98`,
tree `22de99ef24350e533616d899534914478cb53cee`, after its complete cumulative
offline matrix and independent closure review. I3 subsequently closed on
implementation freeze `62df57ca76b55991c8ac01d3a316dfc8a3b450aa`, tree
`968422a59dd1216a0db07ec398c9f49b1a9ff7cc`, after cumulative Release,
sanitizer, deterministic-regeneration, static-boundary, and adversarial
qualification. I3's retained products are offline inputs to later owners; they
do not by themselves start processes or restore a runtime. The explicit
maintained native owners and their focused process suites are current. The
historical aggregate, ROS/Isaac, and physical product-lane suites remain
unavailable.

The separate ROS Jazzy image remains a development toolchain for focused
internal contracts. Such checks do not restore or qualify a ROS product:

```bash
docker compose --profile ros run --rm ros-jazzy ./scripts/test-ros.sh
```

The ROS image starts from `ros:jazzy-ros-base-noble` and keeps ROS Python
dependencies apt-managed. The default development image remains ROS-free; ROS
wrappers/adapters live in the `manyforge_ros` lane.

The heavier ROS Jazzy full image also remains a build/test toolchain, but its
product entry point is explicitly unavailable:

```bash
docker compose --profile ros-full run --rm ros-jazzy-full ./scripts/test-ros-full.sh
```

`test-ros-full.sh` returns the stable cutover reason and exit 78; it must not
build, probe a cached product, or be cited as lane evidence.

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
  python/                pybind11 DTO and graph/compiler projections; no runtime-owner bindings
manyforge_content_identity/  Shared ROS-free canonical JSON and content-digest primitives
manyforge_persistence/   Shared ROS-free mechanical durable-storage safety primitives
manyforge_scene/         Canonical-empty and bounded runtime-only Apply logical-scene owner
manyforge_deployment/    ROS-free composable-cell codec, qualification, final C++ trust boundary, and retention substrate
manyforge_component_manager/  ROS-free I4 owner for the bounded M0 Apply/runtime fixture
manyforge_headless/      C++ Manager/scene/alarm/history and retained-product projection owners
manyforge_headless_api/  Bounded FastAPI/CLI projection and attributed intent transport
manyforge_program_runner/ Independent Program capture/tree/tick/status/Stop orchestration process
manyforge_collision/     C++ planner-neutral collision query implementation
manyforge_roboplan/      Retained planning-model staging and bounded N3 native execution host
manyforge_ros2_control_execution/  Retained C++ ROS execution/admission library; no current product lane
manyforge_cclc_msgs/     ROS IDL for the bounded same-host CCLC request transport
manyforge_xarm_adapter/  Experimental xArm model/binding/observation inputs; no live lane
manyforge_xarm_control/  Build-owned Lite6 catalog and restricted xarm_ros2 CCLC adapter
manyforge_behavior/      Build-owned v2 node environment and Program orchestration/client mechanisms
manyforge_composer/      Structural FastAPI/React shell and thin H0/N6/N7 authoring/operator clients
manyforge_assistant_bridge/ Strict bounded direct/nemoclaw provider mediation; no runtime ownership
manyforge_ros/           Retained pure contracts and explicit unavailable runtime boundaries
tests/                   Cutover and package contract tests
scripts/                 Build/test/setup scripts
docs/                    Architecture reference
```

## Target architecture

```
Program YAML --> Program Runner --> ExecutionKernel (C++) --> Planner / execution adapters
                      |                    |                         |
            Behavior tree/catalog   validation + stop       RoboPlan / ros2_control
                      |             collision authority              |
                      |                     |                  robot / Isaac
                      +---------- Headless facts/intents ------------+
                                            |
                                     Composer / viewer
```

This diagram and the ownership statements below describe both the current
maintained native boundary and the later transport lanes. The kernel owns
terminal and Stop
authority; ManyForge's collision layer owns collision admission; the
independent Program runner hosts Behavior orchestration and run identity;
Headless owners publish runtime, scene, alarm, and history facts. Composer owns
authoring and presentation only—not motion, alarm, scene, or runtime authority.
For the full component map, deployment lanes, scene
initialization, recovery, and observability model, see
[`docs/reference/ARCHITECTURE.md`](docs/reference/ARCHITECTURE.md).

- **manyforge_core** is ROS-free, pure C++20. All robot-framework-specific code lives in adapter packages.
- **manyforge_roboplan** provides retained planning-model staging and the
  current native motion/gripper/pick-place execution, CCLC composition, and
  addressed Stop/recovery host. ROS/Isaac and physical product lanes remain
  unavailable.
- Python code calls C++ through pybind11 bindings -- no duplicate state machines.
- The Docker container includes all dependencies (Pinocchio, RoboPlan, pybind11).

## Historical pick-and-place workflow — unavailable

The former workflow was:

```bash
./scripts/launch.sh --lane manyforge-only --assistant off --scenario ur5-pick-place --non-interactive --yes
```

The command now fails before effects. Use its workflow only as historical
reimplementation input; the demo shim and native viewer are removed.

## Current inspection APIs and historical LLM interactions

The structural Composer REST surface and the independent I5/H0 headless
projection are current. Provider-backed assistant conversations and a public
MCP server remain unavailable; their former interactions are retained as
historical design inputs:

- **REST API** — FastAPI backend at `http://localhost:9000/api/`. Operator-
  facing reference: [`docs/reference/REST_API.md`](docs/reference/REST_API.md).
  The live OpenAPI schema is also available at `http://localhost:9000/docs`
  and `http://localhost:9000/openapi.json` once Composer is running.
- **Headless REST/CLI** — the independent Manager, runtime, scene, alarm,
  history, health, and thin addressed execution/preview projection defaults to
  `http://127.0.0.1:9010` when
  started with `python -m manyforge_headless_api serve`. Contract:
  [`docs/reference/HEADLESS_REST_API.md`](docs/reference/HEADLESS_REST_API.md).
  It exposes the selected bounded native execution owner, confirmed CCLC
  lifecycle/route requests, scoped Stop, Full Stop, and Stop-fault recovery.
  Robot Control preview never moves a resource, and approval uses the ordinary
  operation endpoint. An absent owner is reported as unavailable rather than
  inferred from client state.
- **MCP Server (historical)** — former Model Context Protocol interface.
  Reference-only tool contract:
  [`docs/reference/MCP_RUNTIME_CONTRACT.md`](docs/reference/MCP_RUNTIME_CONTRACT.md).
- **Assistant Provider Bridge (historical)** — former HTTP mediation service
  between Composer and an LLM endpoint. Reference-only wire contract:
  [`docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md).

See [`docs/reference/HISTORICAL_COMPOSER_INTERACTION_REFERENCE.md`](docs/reference/HISTORICAL_COMPOSER_INTERACTION_REFERENCE.md)
for the exact source commit and the rule for translating interaction intent
onto new headless owners.

## Docker Details

The container runs as a non-root user (`manyforge`, uid 1000). The entrypoint
(`docker-entrypoint.sh`) only establishes the runtime environment:

- Sets `PYTHONPATH` for `manyforge_behavior`
- Uses `/tmp/manyforge-build/` for out-of-tree builds (avoids conflicts with host build dirs)
- Refuses a non-writable build root; it never builds or deletes cached products

Canonical `docker compose run --rm dev ...` test containers use an ephemeral
build root. No product launcher may consume a retained runtime cache during the
cutover.

To rebuild the image after Dockerfile changes:

```bash
docker compose build
```

To rebuild from scratch (e.g. after RoboPlan update):

```bash
rm -rf .docker/roboplan
./scripts/setup-docker.sh
```

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

ManyForge development is spec-driven by private maintainer specs. Those specs
are not needed to inspect the current public structural and Program Draft/Saved
authoring shell or historical operator references, but non-trivial behavior
changes need maintainer audit against them.
Without that context, contributions should stay to bounded fixes,
tests, docs, or proposals that explicitly call out the audit need.

The historical assistant baseline was tested with **NemoClaw-Thor 0.1.x** on
Jetson Thor. That evidence does not qualify the current assistant lane; see the
companion repo at
<https://github.com/pastoriomarco/NemoClaw-Thor>.
