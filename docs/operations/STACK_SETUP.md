> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Stack Setup

> **Current `dev` boundary:** use
> [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md) for the ROS development
> entrypoint. Workbench empty-ready and mock Phase A `qualified-live02` have run;
> the full Compose wrapper, observed GUI journey, Isaac and physical operation
> remain unqualified. The native simulator is removed. Commands and services
> below describe historical aggregate-launch restoration inputs.

Historical operational runbook for bringing up ManyForge locally, with or
without an assistant provider. This file intentionally stays compact and
points to the captured scripts and configuration sources instead of embedding
their contents.

User-facing product overview belongs in [`../../README.md`](../../README.md).

## Scope

This runbook covers:

- ManyForge-only bring-up
- ManyForge plus ROS 2 controller execution
- the optional AI assistant (an `ai_model` Deployment component)
- the current recommended commands

It does not cover:

- detailed integration design rationale
- upstream platform gaps
- historical sprint logs

For runtime/operator-facing reference docs see:

- [`../reference/REST_API.md`](../reference/REST_API.md) — Composer REST surface
- [`../reference/MCP_RUNTIME_CONTRACT.md`](../reference/MCP_RUNTIME_CONTRACT.md) — MCP tool contract
- [`../reference/ASSISTANT_PROVIDER_CONTRACT.md`](../reference/ASSISTANT_PROVIDER_CONTRACT.md) — AI model endpoint and bridge contract
- [`../operations/ASSISTANT_E2E_COOKBOOK.md`](./ASSISTANT_E2E_COOKBOOK.md) — AI assistant setup and checks

(Internal architectural design rationale, platform-gap investigations, and
sprint history are tracked in the private `manyforge_specs` repository, which
is not required for deployment or operation of this repo.)

## Clean Machine Install Order

Use this section when a machine has been freshly formatted. For Jetson Orin
AGX with a small eMMC root and a secondary NVMe, start at
[`ORIN_STACK_SETUP.md`](ORIN_STACK_SETUP.md) instead; it adds the required
storage staging before the same runtime flow.

1. Prepare the host: Docker Engine + Compose v2, `git`, `curl`, and enough
   disk for Docker images and build caches.
1. Clone `manyforge` under `${HOME}/workspaces/dev_ws/src/`.
1. Build the ManyForge image:
   ```bash
   cd ${HOME}/workspaces/dev_ws/src/manyforge
   ./scripts/setup-docker.sh
   ```
1. Start the stack with the development workbench
   ([`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)).
1. Optional, for the AI tab: run your own OpenAI-compatible model server and
   add an `ai_model` component to the Deployment
   ([`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md)).

## Runtime Selection

Robot execution and the AI assistant are independent. Select one execution
lane; the assistant is enabled by the Deployment's `ai_model` component and
never changes motion authority, controller binding, or robot-state
precedence.

### Robot execution lanes

#### Historical ManyForge-only native

No model server dependency.

Use when:

- validating tree execution
- debugging planning/execution
- avoiding any model/runtime integration variables

#### ROS 2 controller execution

ManyForge plans and validates locally, then dispatches robot motion to a
ROS 2 controller selected by deployment config. Use this for Isaac Sim
or robot-driver previews where the program stays portable and the
deployment provides controller topics/actions.

Canonical activation goes through the unified launcher so the selected image,
build products, and cooperating services share one captured identity:

```bash
scripts/launch.sh --lane isaac-sim --assistant off \
  --scenario isaac-ur10e-light-scene --non-interactive --yes
```

By default, the selected scenario's supervisor loads:

- program: `examples/isaac_manipulator_scene_light.program.yaml`
- deployment: `examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml`

Set `LOAD_ARTIFACTS=false` only when you intentionally want to load them
manually.

The `composer-ros` service uses the full ROS-capable image with host
networking so DDS can discover Isaac/robot endpoints. The former native
simulated execution path is removed. Current development
uses the ROS host through the Workbench; this historical Compose recipe has
not been qualified as the complete current wrapper.

The shipped `manyforge_ros2_control_execution` C++ adapter owns
FollowJointTrajectory dispatch, feedback/settling, bounded stop/reset, and the
lane-selected execution-state projection. Deployment/Composer factory dispatch
fails closed if that adapter or a required controller/provider is unavailable.
The Isaac `commanded_sim` profile and physical controller-observed profile have
different state precedence; see
[`../reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md#deployment-lanes).

Use when:

- validating ROS 2 controller endpoint discovery
- testing Isaac Sim / ros2_control execution wiring
- historical trajectory execution qualification through ROS controllers

#### Supervised physical robot

The experimental `real-robot` lane uses the same generic ROS 2 execution
adapter, but adds a robot-lifecycle provider and a strict retained runtime
record. The physical xArm/ros2_control owner and Composer run as separate
services; only the owner process talks to the robot. Startup leaves drives,
hardware, the trajectory controller, the program, and motion admission off.

For the first Lite6 target, use only the attended workflow in
[`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md#experimental-real-lite6-commissioning)
and [`LITE6_LIVE_QUALIFICATION.md`](LITE6_LIVE_QUALIFICATION.md). In
particular, **Run**, Resume, retry/replan, and Robot Control approval remain
disabled until a fresh, identity-matched lifecycle snapshot proves the robot
enabled and the complete motion-admission gate open. Browser state is only an
operator aid: the backend and execution adapter enforce the same refusal.

### Optional model and assistant surfaces

#### Runtime model nodes

Runtime nodes such as `query_model` name a model alias of the Deployment's
assistant profile; each alias resolves to one of the Deployment's `ai_model`
components (the chat uses the same mechanism for its alias). A token, if any,
comes from the component's `api_key_env` variable (`MANYFORGE_AI_*`).

Use when:

- testing `query_model`
- validating bounded model interactions
- running semantic-selection programs on the current text-only baseline

#### Composer AI assistant

The AI tab chats with a model that edits the Program Draft through a fixed
tool set (mode `composer-assistant`). The model server is yours: any
OpenAI-compatible server with tool calling (llama.cpp `llama-server --jinja`,
vLLM `--enable-auto-tool-choice --tool-call-parser ...`). The Deployment names
it with an `ai_model` component (`base_url`, `model`, optional token
environment variable name `api_key_env`, which must start with
`MANYFORGE_AI_`) next to an assistant profile. Run
`export MANYFORGE_AI_<NAME>=<token>` before starting the workbench. Prepare
and Enable never refuse because of the model server (with the default warning/auto `endpoint` stream); its reachability is the
component's `endpoint` stream warning.

The launcher always starts the bridge on `127.0.0.1:8100`. The AI tab is
enabled when the active Deployment has an AI model component; otherwise it
shows "No AI model component in this Deployment". Assistant edits land in the
Draft; Apply, or Discard/Revert, as for manual edits.

Setup and checks: [`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md).

## Prerequisites

### Host

ManyForge runs inside Docker containers, so the host requirement is
primarily "a machine that runs Docker." Specifics:

- **OS**: Linux (Ubuntu 24.04 LTS is the reference). Docker Desktop on
  macOS / Windows works for native development but is unsupported for
  the assistant and Isaac lanes — Isaac Sim and NVIDIA-runtime workloads
  require a Linux host with NVIDIA drivers.
- **Architecture**: amd64 (x86_64) is the reference. arm64 is supported
  for the dev image and the official full ROS Lyrical image. Isaac Sim itself
  is x86_64-only.
The official Lyrical tooling image must be built before its compatibility aliases
are run. Actual Isaac topic-control and experimental real-Lite6 image defaults
remain on their current bases temporarily until the Lyrical topic-control roundtrip and xArm
driver/mock bring-up qualification. Preserve those images during this hold. The laptop has the exact held amd64
base `manyforge-ros-jazzy-full:q44-20261010` (image ID
`sha256:0ecba713fbddcd4f6a853f8380c8dd59c204b98f6c5463005052fc43e1a156d9`).
`MANYFORGE_ISAAC_ROS_IMAGE` selects only the held Isaac image;
`MANYFORGE_REAL_LITE6_ROS_BASE_IMAGE` selects only the held real-Lite6 build
base. Neither uses the common `MANYFORGE_ROS_FULL_IMAGE` override. Current
Orin/Thor are arm64 and their inventories have no qualified held Jazzy full
image; the preserved laptop amd64 image cannot supply those lanes. These recipes are
unavailable there until an exact qualified image is explicitly supplied.

- **Docker**: Docker Engine with BuildKit, plus the Compose v2 plugin
  (`docker compose ...`, not the legacy `docker-compose`).
- **GPU / driver**: not required for native execution without a local model.
  A local model server for the AI assistant usually needs one; it runs
  outside ManyForge.
  Required for the Isaac lane (NVIDIA driver + RTX-class GPU).
- **RAM**: 16 GB minimum for native execution with a comfortable C++ build.
  32 GB recommended for the Isaac lane. A local model server needs its own
  memory on top.
- **Disk**: ~10 GB for the Docker image plus build cache. Model files live
  outside the manyforge tree.
- **Model server (AI assistant only)**: an OpenAI-compatible HTTP server with
  tool calling, such as llama.cpp or vLLM, named by the Deployment's
  `ai_model` component.

### Repositories

- `manyforge` (this repo) — required.
- `manyforge_specs` — **dev-only**, not required for runtime/operation.
  Cloned alongside `manyforge` only by maintainers working on specs/ADRs.

### External dependencies (pinned versions and source)

These are baked into the Docker images by `scripts/setup-docker.sh`
and the Dockerfiles. Operators do not need to install or manage them
manually; they are documented here for traceability.

| Dependency                                                                           | Version / pin                                                      | Source                                                           | Notes                                                                                                                                                                                                                                                                                                                      |
| ------------------------------------------------------------------------------------ | ------------------------------------------------------------------ | ---------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Base dev image                                                                       | `python:3.12-slim-bookworm`                                        | Docker Hub                                                       | Dockerfile L31; ROS-free dev environment                                                                                                                                                                                                                                                                                   |
| Frontend builder                                                                     | `node:22-slim`                                                     | Docker Hub                                                       | Dockerfile L23; React/Vite build stage                                                                                                                                                                                                                                                                                     |
| ROS image                                                                            | pinned Ubuntu Noble + ROS Lyrical                                         | Docker Hub                                                       | Dockerfile.ros-lyrical-full; single official full ROS toolchain                                                                                                                                                                                                                                                                         |
| RoboPlan                                                                             | reviewed `0.7.0` commit `099dfa5ccd80b191eb3d6da9f4795f808a63913f` | `https://github.com/open-planning/roboplan.git`                  | Sole selected baseline, pinned in `scripts/setup-docker.sh` as `ROBOPLAN_PINNED_REF`. Exact source identity and every checked patch/removal criterion are recorded in `third_party/roboplan/README.md`; earlier series remain historical/restoration evidence, not selectable builds.                                                               |
| ProxSuite                                                                            | reviewed `v0.7.3` commit `b93d7778ffc3299d84b5cb0851022a29bf24a596` | `https://github.com/Simple-Robotics/proxsuite.git`               | Source-built from the exact commit with only its frozen `cmake-module` gitlink populated; Docker configuration is disconnected from network fallback.                                                                                                                                                                                               |
| TOPPRA                                                                               | reviewed `0.6.10` commit `8121e9af72f5aa589b515aa15e0dc408344c3220` | `https://github.com/hungpham2511/toppra.git`                     | Source-built from the exact commit for RoboPlan 0.7; Docker configuration is disconnected from network fallback.                                                                                                                                                                                                                                     |
| Pinocchio (kinematics / dynamics)                                                    | `pin==4.1.0`, `libpinocchio==4.1.0`                                | PyPI                                                             | Both wrapper and C++ library wheels are pinned; the compatible aarch64 graph uses `coal==3.0.3` and `libcoal==3.0.3`.                                                                                                                                                                                                      |
| `pybind11`                                                                           | `3.0.3`                                                            | PyPI                                                             | Dockerfile L58                                                                                                                                                                                                                                                                                                             |
| `nanobind`                                                                           | `2.12.0`                                                           | PyPI                                                             | Dockerfile L63                                                                                                                                                                                                                                                                                                             |
| `topic_based_ros2_control`                                                           | commit `aa9f9ef9525a60f8becea322537dd1d6d1bf9123`                  | `https://github.com/PickNikRobotics/topic_based_ros2_control.git` | Dockerfile.ros-lyrical-full; selected stock PickNik source, built into the ROS image                                                                                                                                                                                                                                      |
| Eigen 3                                                                              | apt `libeigen3-dev`                                                | Debian Bookworm                                                  | Dockerfile L45                                                                                                                                                                                                                                                                                                             |
| `urdfdom`                                                                            | apt `liburdfdom-dev`                                               | Debian Bookworm                                                  | Dockerfile L45                                                                                                                                                                                                                                                                                                             |
| `octomap`                                                                            | apt `liboctomap-dev`                                               | Debian Bookworm                                                  | Dockerfile L46                                                                                                                                                                                                                                                                                                             |
| `boost` (filesystem / serialization / system / python / chrono / thread / date_time) | apt                                                                | Debian Bookworm                                                  | Dockerfile L43-44                                                                                                                                                                                                                                                                                                          |
| `trimesh`                                                                            | `4.11.5`                                                           | PyPI                                                             | Dockerfile L61; OBJ/mesh parsing                                                                                                                                                                                                                                                                                           |
| `yourdfpy`                                                                           | `0.0.60`                                                           | PyPI                                                             | Dockerfile L62; URDF Python loader                                                                                                                                                                                                                                                                                         |
| FastAPI                                                                              | `0.135.3`                                                          | PyPI                                                             | Dockerfile L67; Composer backend                                                                                                                                                                                                                                                                                           |
| MCP (Model Context Protocol)                                                         | `1.27.0`                                                           | PyPI                                                             | Dockerfile L71; assistant-lane MCP server                                                                                                                                                                                                                                                                                  |

The AI model endpoint is the Deployment's `ai_model` component; the model
server itself is not part of ManyForge.

**Confirming the RoboPlan ref baked into your image.** The
`package.xml` `<version>` field can lag the actual code (PR branches
carry the same string as the tag they forked from), so
`pip show roboplan` inside the container is not authoritative. After
`setup-docker.sh` runs, the actual ref and resolved SHA are recorded
on the host:

```bash
cat .docker/roboplan.manyforge-ref   # the ref passed to git fetch
cat .docker/roboplan.manyforge-sha   # the resolved commit SHA
```

The disposable checkout retains shallow Git metadata. Every setup run verifies
the recorded commit, tracked and untracked files, and recursive submodule
commits before reusing it; any drift causes a fresh clone. Docker excludes the
Git metadata and applies exactly the hash-manifested patches in manifest order.

**Force a refresh** (e.g. to test a newer release tag or local branch):

```bash
rm -rf .docker/roboplan/
ROBOPLAN_REF=<branch|tag|sha> ./scripts/setup-docker.sh
```

With no `ROBOPLAN_REF`, the script uses the reviewed RoboPlan 0.7.0 commit
pinned in `scripts/setup-docker.sh` (`ROBOPLAN_PINNED_REF`) together with the
exact ProxSuite and TOPPRA source closure. Maintainers move any pin only after
the affected dependency, adapter, sanitizer, platform, and performance gates
in `third_party/roboplan/README.md` pass. An explicit `ROBOPLAN_REF` changes
only RoboPlan for a development experiment and cannot produce release evidence
for the selected build. The setup script marks that image build
`development_override`; exact pinned-source verification is disabled visibly,
the source-stack builder reports non-qualification mode, the exact dependency
closure remains enforced, and the checked patch manifest must still apply.
Canonical builds default to fail-closed `qualified` mode.

## Primary Scripts

> **Bringing up the stack from a clean checkout?** Use the unified
> launcher `./scripts/launch.sh`. Its operator smoke-test recipe lives
> at [`docs/operations/LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md)
> — covers all four lane × assistant combinations end-to-end. The
> `demo-*.sh` scripts below are now thin shims that delegate to
> `launch.sh`; their legacy CLI flags and `restart-bridge` /
> `reload-composer` verbs are deprecated (see
> [`scripts/MIGRATION.md`](../../scripts/MIGRATION.md) for replacements).

### `scripts/demo-shape-pick.sh`

Purpose:

- start the semantic shape-pick demo
- rewrite the example deployment/program into temp artifacts for the chosen
  runtime options

Recommended baseline:

```bash
./scripts/demo-shape-pick.sh \
  --model-id Qwen3.5-9B-Claude-Distilled-NVFP4 \
  --selection-image-mode text \
  --skip-bootstrap
```

Notes:

- `text` is the current supported baseline
- `inline` and `http` image modes are experimental placeholders only

### `scripts/manyforge-stack.sh`

**Removed legacy transport, not a restoration target.** This script belonged
to the older NemoClaw mux/plugin integration path and is intentionally absent
from the current tree together with its direct-route and mux proxies. Its exact
source remains available at
`manyforge@08222188293df8e12f90dc72e204c21f22f92ea1` for archaeology; the
commands below describe that historical baseline only.

Purpose:

- start/stop the ManyForge stack with legacy NemoClaw mux integration
- manage the NemoClaw plugin lifecycle (`plugin deploy`,
  `plugin rollback`, `plugin status`, `plugin list`)
- drive sandbox smoke tests (`smoke direct|mux|all`)

Typical usage:

```bash
./scripts/manyforge-stack.sh up                  # legacy ManyForge container only
MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true ./scripts/manyforge-stack.sh up --with-nemoclaw
./scripts/manyforge-stack.sh status
MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true ./scripts/manyforge-stack.sh smoke all
./scripts/manyforge-stack.sh plugin status
./scripts/manyforge-stack.sh down
```

### `scripts/configure-manyforge-provider.sh`

Removed NemoClaw/OpenClaw mux/plugin configuration helper. It was exclusive to
the deleted direct-route/mux transport and is not a future assistant-client
port target. Consult the historical commit named above if its interaction is
needed for archaeology; do not recreate it as a compatibility path.

### `scripts/dev-agent-session-start.sh` (dev-only)

Purpose:

- restore sandbox reference materials and contracts for a new
  *maintainer development* session
- upload the engineering reference and specs into the sandbox

Operators do not need this script. It requires the private
`manyforge_specs` repo on disk and is intended for maintainer
dev-assist agents only.

## Recommended Bring-Up Sequences

### ManyForge only

```bash
scripts/launch.sh --lane manyforge-only --assistant off \
  --scenario lite6-pick-place --non-interactive --yes
```

Then use:

- Composer API on port `9000`
- native Composer viewer in the Composer UI

This lane does not require ROS, Isaac Sim, or a robot controller. The
native execution adapter is in-process: reset and retry only affect ManyForge's
software state and RoboPlan-backed scene projection. No external robot or
simulator is commanded.

### Isaac / ROS 2 controller execution

For the multi-machine Lite6 trocar workflow that adds Isaac ROS
YOLOv8/FoundationPose perception, use
[`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md).
That workflow keeps Isaac ROS perception, Isaac Sim, and ManyForge in separate
process/container contexts and adds DDS/QoS requirements for raw camera streams.

1. Start the ros2_control bridge and ROS-capable Composer:

   ```bash
   ./scripts/demo-isaac-ros2-control.sh start
   ```

   Isaac Sim may be started before or after this command.  The bringup lane
   includes an Isaac demo supervisor that loads the Composer program/deployment
   as soon as Composer is healthy, so the backend-authored scene is visible in
   the native Composer viewer even if Isaac is still off.  It keeps the arm command gate closed
   until `/isaac_joint_states`, controller state, the arm FollowJointTrajectory
   action, and the gripper action are all reachable. Once they are ready, it
   requests a Composer reset so Isaac is commanded to the configured home pose
   through the normal execution backend path.

   The loaded artifacts are:

   - `examples/isaac_manipulator_scene_light.program.yaml`
   - `examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml`

   The lower-right Runtime button in Composer reports whether the current
   profile can render, plan, and execute.  For this Isaac commanded-sim profile,
   `Waiting` means rendering/planning may be available while execution is still
   blocked on simulator/controller readiness.

1. If you want to inspect the raw Isaac side manually, confirm these topics:

   ```bash
   docker run --rm --network host ${MANYFORGE_ISAAC_ROS_IMAGE:-manyforge-ros-jazzy-full:q44-20261010} \
     bash -lc 'source /opt/ros/jazzy/setup.bash; ros2 topic list | grep isaac_joint'
   ```

   Expected topics once Isaac playback and ROS publishing are active:

   - `/isaac_joint_states`
   - `/isaac_joint_commands`

1. The `isaac-topic-control` service launches:

   - `topic_based_ros2_control/TopicBasedSystem`
   - `joint_state_broadcaster`
   - `isaac_arm_controller`
   - `/isaac_arm_controller/follow_joint_trajectory`
   - `manyforge_isaac_topic_command_gate`
   - `manyforge_isaac_demo_supervisor`

   Keep this container running while Composer executes against Isaac. Host IPC
   is required for reliable ROS 2 action/service behavior across containers;
   the compose service already sets `ipc: host`. Canonical preflight requires
   the selected image to vendor `topic_based_ros2_control`. A temporary build
   from `${ISAAC_ROS_WS}/src/topic_based_ros2_control` is a debug-only escape
   hatch and requires
   `MANYFORGE_SOURCE_DEVELOPMENT_ROS_OVERLAYS=ON`.

1. Raw Compose startup is retained only as a debugging surface:

   ```bash
   docker compose --profile ros-full up isaac-topic-control composer-ros
   ```

   It does not pin a mutable image tag to an immutable ID or derive the cache
   identity from that ID. Do not use it for qualification or product
   operation. The supervisor still handles startup-order recovery and
   program/deployment loading; set `SUPERVISOR_AUTO_LOAD=false` to disable
   that behavior.

1. If automatic loading is disabled, load the portable program and ROS
   deployment variant in Composer:

   - `examples/isaac_manipulator_scene_light.program.yaml`
   - `examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml`

The deployment targets `/isaac_arm_controller/follow_joint_trajectory` for the
arm. Raw `/isaac_joint_states` feeds `topic_based_ros2_control`; ManyForge
observes the controller-owned `/joint_states` and
`/isaac_arm_controller/controller_state` surfaces through its C++ execution
backend. Because this profile is `commanded_sim`, ManyForge treats its
backend/authored state as the simulator truth and disables JTC path/goal
tolerance aborts on arm goals; controller feedback is still captured as
tracking telemetry. Do not use that tolerance override for real robot
deployments. The preview deployment uses `command_mode: ros_gripper_command`
for the Robotiq gripper. The Isaac-only bridge exposes the standard
`/robotiq_gripper_controller/gripper_cmd` `control_msgs/GripperCommand` action
and translates it to the scene's raw `/isaac_gripper_state` JointState command
topic.

The launcher verifies the ROS image and the local
`manyforge_ros_bringup` install before startup. If the Isaac gripper bridge
bridge, command gate, or supervisor executable is missing from the installed
ROS package overlay, it rebuilds the ROS image/overlay instead of starting a
stale bridge stack. Use `./scripts/demo-isaac-ros2-control.sh status` to see
the supervisor readiness JSON, `./scripts/demo-isaac-ros2-control.sh smoke` for
a tiny reversible arm-action probe, and
`./scripts/demo-isaac-ros2-control.sh stop` to stop the preview lane.

### Real ROS 2 controller / robot execution

Real robot deployments must use a deployment profile separate from the Isaac
preview YAML. The source of truth is the controller/driver observed state, not
ManyForge-authored state. In this profile:

- reset clears ManyForge execution state and resyncs from the controller; it
  must not command hardware to a home pose unless an explicit, qualified
  motion node does so,
- `disable_controller_path_tolerances` must remain false,
- controller path/goal tolerances, stop completion, start-state tolerance, and
  observed-state freshness must be qualified for the cell,
- any external safe-state request is an integration hook to the cell's
  independent safety/supervisor chain, not ManyForge acting as the certified
  safety controller.

### AI assistant

1. Start your model server and check `curl {base_url}/models` lists the model.
1. Add the `ai_model` component and an assistant profile to the Deployment.
1. Start the stack, Prepare/Enable, and open the AI tab.

See [`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md).

## Operational Notes

### Temp demo artifacts

The semantic demo launcher creates temp files such as:

- `.manyforge-demo-shape-pick.<id>.program.yaml`
- `.manyforge-demo-shape-pick.<id>.deployment.yaml`

These are per-run rewritten artifacts. They are not source docs and should not
be treated as durable repository content.

### Model aliasing

Assistant profiles name model aliases; the Deployment's single `ai_model`
component serves every alias, so the served model is always the component's
`model`.

### Blackboard inspection baseline

For current selection/model/planning flows:

- declared blackboard artifacts are authored through `namespace_ref`,
  `key_ref`, and `field_ref`
- runtime namespace outputs appear as one root bundle per namespace
- when inspecting `/api/execution/blackboard`, expect bundle-shaped values with
  `meta` and `value`, not the old scattered `response/status/fields/...` form

### RoboPlan planner trace logging

Native RoboPlan planner traces are disabled by default to keep normal demo and
qualification logs readable. Enable them only while diagnosing planner behavior:

```bash
MANYFORGE_ROBOPLAN_PLANNER_DEBUG=1 ./scripts/demo-ur10e-robotiq-pick-place.sh
```

Accepted disabled values are unset, `0`, `false`, and `FALSE`. Any other
non-empty value enables `[PLANNER]` stderr traces from the native RoboPlan
backend.

### Runtime image modes

Current status:

- `text`: supported baseline
- `inline`: experimental
- `http`: experimental and topology-sensitive

Do not treat browser-backed image capture as a production path.

## Troubleshooting

For the cross-cutting symptom → cause → diagnostic-step matrix that
spans collision, interrupt, assistant, and Isaac/ros2_control failures,
see [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md). The rest of this section
covers stack-bring-up-specific cases.

### Tree does not progress after `Run`

Check:

- cycle state via `GET /api/cycle/status`
- execution digest via `GET /api/execution/digest`
- event history via `GET /api/execution/history`

If testing image-enabled demos:

- switch back to `--selection-image-mode text`
- browser-backed image capture is still experimental

### Model selection appears ignored

The model name sent to the server is the `ai_model` component's `model`. The
server must serve exactly that name (llama.cpp `--alias`, vLLM
`--served-model-name`).

## Keep This File Small

When scripts change:

- update the command examples and scope notes here
- do not paste entire script bodies into this document
- treat the script files as the source of truth
