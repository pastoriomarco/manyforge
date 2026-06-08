> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Stack Setup

Operational runbook for bringing up ManyForge locally, with or without
NemoClaw/OpenClaw. This file intentionally stays compact. It points to scripts
and live configuration sources instead of embedding their contents.

User-facing product overview belongs in [`../../README.md`](../../README.md).

## Scope

This runbook covers:

- ManyForge-only bring-up
- ManyForge plus ROS 2 controller execution
- ManyForge plus direct local model endpoint
- ManyForge plus NemoClaw/OpenClaw integration
- the current recommended commands

It does not cover:

- detailed integration design rationale
- upstream platform gaps
- historical sprint logs

For runtime/operator-facing reference docs see:

- [`../reference/REST_API.md`](../reference/REST_API.md) — Composer REST surface
- [`../reference/MCP_RUNTIME_CONTRACT.md`](../reference/MCP_RUNTIME_CONTRACT.md) — MCP tool contract
- [`../reference/ASSISTANT_PROVIDER_CONTRACT.md`](../reference/ASSISTANT_PROVIDER_CONTRACT.md) — assistant provider HTTP wire format
- [`../operations/ASSISTANT_E2E_COOKBOOK.md`](./ASSISTANT_E2E_COOKBOOK.md) — end-to-end recipes

(Internal architectural design rationale, platform-gap investigations, and
sprint history are tracked in the private `manyforge_specs` repository, which
is not required for deployment or operation of this repo.)

## Clean Machine Install Order

Use this section when a Thor/workstation has been freshly formatted and the
whole ManyForge + NemoClaw/OpenClaw assistant stack needs to be installed from
zero. For Jetson Orin AGX with a small eMMC root and a secondary NVMe, start at
[`ORIN_STACK_SETUP.md`](ORIN_STACK_SETUP.md) instead; it adds the required
storage staging before the same runtime flow.

`scripts/launch.sh` is the repeatable runtime launcher. It starts and wires
components after the host prerequisites and first-time NemoClaw sandbox
onboarding already exist. It does **not** install Docker/NVIDIA runtime,
Node.js, NemoClaw, or OpenShell, and it does **not** create the first
`my-assistant` sandbox with `nemoclaw onboard`.

1. Prepare the host: Docker Engine + Compose v2, NVIDIA Container Toolkit for
   assistant/model serving, `git`, `curl`, enough disk for Docker images and
   model caches, Hugging Face token/licence acceptance for gated models.
2. Clone the sibling repos under `${HOME}/workspaces/dev_ws/src/`. Use the
   same tested branch/tag across `manyforge` and `NemoClaw-Thor`; do not mix a
   migration branch in one repo with `main` in the other.
3. Build the ManyForge image:
   ```bash
   cd ${HOME}/workspaces/dev_ws/src/manyforge
   ./scripts/setup-docker.sh
   ```
4. Install the NemoClaw/OpenShell toolchain from `NemoClaw-Thor` and verify
   `nemoclaw`, `openshell`, and Docker work from the operator account.
5. Start the local model once from `NemoClaw-Thor/serving/` and confirm
   `curl http://127.0.0.1:8000/v1/models` returns the served model.
6. Run first-time `nemoclaw onboard` to create the `my-assistant` sandbox.
   The onboard wizard probes the model endpoint, so the model must already be
   running.
7. After `my-assistant` exists, use the launcher for repeatable bring-up:
   ```bash
   cd ${HOME}/workspaces/dev_ws/src/manyforge
   ./scripts/launch.sh restart
   ```
8. Verify the live stack:
   ```bash
   curl -fsS http://127.0.0.1:9000/api/health
   curl -fsS http://127.0.0.1:8000/v1/models
   curl -fsS http://127.0.0.1:8200/healthz
   ```

The launcher may re-apply provider config, egress policy, skills, MCP server
registration, gateway startup, and bridge startup on every run. That is
intentional and idempotent. The non-idempotent first-time step is creating the
sandbox itself with `nemoclaw onboard`.

## Current Supported Modes

### 1. Deterministic ManyForge only

No LLM, no NemoClaw/OpenClaw dependency.

Use when:

- validating tree execution
- debugging planning/execution
- avoiding any model/runtime integration variables

### 2. ManyForge plus ROS 2 controller execution

ManyForge plans and validates locally, then dispatches robot motion to a
ROS 2 controller selected by deployment config. Use this for Isaac Sim
or robot-driver previews where the program stays portable and the
deployment provides controller topics/actions.

Current activation point:

```bash
docker compose --profile ros-full up composer-ros
```

Then load:

- program: `examples/isaac_manipulator_scene_light.program.yaml`
- deployment: `examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml`

The `composer-ros` service uses the full ROS-capable image with host
networking so DDS can discover Isaac/robot endpoints. The default
Composer services remain ROS-free and continue to use native simulated
execution unless a deployment selects another adapter.

Important current status: the deployment and Composer factory dispatch
are wired to fail closed when the ROS execution runtime factory is not
installed. The ROS-side FollowJointTrajectory transport helpers and fake
controller tests exist, but the in-process `IExecutionBackend` runtime
adapter is still the next implementation slice before this mode can move
Isaac from the Composer Run button.

Use when:

- validating ROS 2 controller endpoint discovery
- testing Isaac Sim / ros2_control execution wiring
- qualifying trajectory execution outside the native simulated backend

### 3. ManyForge plus direct local model endpoint

ManyForge talks directly to a local OpenAI-compatible server such as vLLM.

Use when:

- testing `query_model`
- validating bounded model interactions
- running the semantic selection demo on the current text-only baseline

### 4. ManyForge plus NemoClaw/OpenClaw

Two assistant-provider lanes coexist; **OpenClaw + Cosmos-Reason2-8B
(`openclaw` provider) is the demo default** as of 2026-05-07. The
launcher [`scripts/demo-assistant-known-good.sh`](../../scripts/demo-assistant-known-good.sh)
sets `ASSISTANT_PROVIDER=openclaw` and `MODEL_PROFILE=cosmos-reason2-8b`
by default.

- **OpenClaw lane (default — `ASSISTANT_PROVIDER=openclaw`)**: persistent
  OpenClaw gateway in the NemoClaw `my-assistant` sandbox. Composer's
  `openclaw` provider talks to an adapter at `:8200` (lives in
  NemoClaw-Thor at `manyforge/openclaw_assistant_bridge/`) which
  forwards to `/v1/chat/completions` on the gateway. The gateway runs
  the agent loop internally; ManyForge enforces every tool call via the
  mode-scoped MCP wrapper + `/api/assistant/bridge/tools/{toolId}`.
  Production-validated 9/9 on the 3×3 lane-comparison smoke (P1 wrap-
  root, P2 scene add, P3 tree insert) once envelope reductions and
  load-bearing fields landed (2026-05-08); Cosmos-Reason2-8B at 256K
  context with the cascading 120s chain timeout. Recommended whenever
  sandbox isolation + the bounded-autonomy audit envelope
  (assistantMode + catalogHash + requestId + principal) matter, which
  for the production deployment shape is always.

- **Direct vLLM lane (`ASSISTANT_PROVIDER=nemoclaw`)**: in-tree
  `manyforge_assistant_bridge` on `:8100` makes chat-completions calls
  against vLLM directly, runs the agent loop in-process. Sub-2 s warm
  latency, ~22 s/test p50 — fastest end-to-end, but no sandbox
  isolation and no per-call audit envelope. Use this for local
  development, debugging the bridge in isolation, or single-tenant
  setups where the model is fully trusted.

Both lanes preserve the same bounded-autonomy contract on the ManyForge
side. Switching between them is a single env var on
`scripts/demo-assistant-known-good.sh`. The lane-comparison data that
justified the default flip (`nemoclaw` → `openclaw` on 2026-05-07)
and the §10 ablation that validated 9/9 reliability live in
[`LANE-COMPARISON-direct-vs-openclaw.md`](https://github.com/pastoriomarco/NemoClaw-Thor/blob/main/manyforge/docs/LANE-COMPARISON-direct-vs-openclaw.md)
in NemoClaw-Thor.

Setup recipe:
[`./ASSISTANT_E2E_COOKBOOK.md`](./ASSISTANT_E2E_COOKBOOK.md) →
"Recipe: Known-Good Assistant Demo Launcher".
Integration design:
[`MANYFORGE-MCP-INTEGRATION.md`](https://github.com/pastoriomarco/NemoClaw-Thor/blob/main/manyforge/docs/MANYFORGE-MCP-INTEGRATION.md)
in NemoClaw-Thor.

Use when:

- validating sandbox-to-ManyForge agent flows
- working on assistant, monitoring, or delegated-recovery integration

## Prerequisites

### Host

ManyForge runs inside Docker containers, so the host requirement is
primarily "a machine that runs Docker." Specifics:

- **OS**: Linux (Ubuntu 24.04 LTS is the reference). Docker Desktop on
  macOS / Windows works for Mode 1 development but is unsupported for
  the assistant and Isaac lanes — Isaac Sim and NVIDIA-runtime workloads
  require a Linux host with NVIDIA drivers.
- **Architecture**: amd64 (x86_64) is the reference. arm64 is supported
  for the dev image and the ROS Jazzy image (`ros:jazzy-ros-base-noble`
  has both architectures); the Jetson Thor deployment target is
  arm64+CUDA via NemoClaw-Thor. Isaac Sim itself is x86_64-only.
- **Docker**: Docker Engine with BuildKit, plus the Compose v2 plugin
  (`docker compose ...`, not the legacy `docker-compose`).
- **GPU / driver**: not required for Mode 1 (deterministic). Required
  for Mode 2 (direct LLM) and Mode 3 (sandboxed assistant) — both
  expect an OpenAI-compatible model server reachable from the host;
  on Thor that is the NemoClaw-Thor-managed vLLM container, which
  needs the NVIDIA Container Toolkit (`nvidia-ctk`) configured.
  Required for the Isaac lane (NVIDIA driver + RTX-class GPU).
- **RAM**: 16 GB minimum for Mode 1 with a comfortable C++ build.
  32 GB recommended for Mode 2/3 (vLLM warm-up) and for the Isaac lane.
  Jetson Thor 64 GB unified memory is the supported assistant-lane
  deployment target.
- **Disk**: ~10 GB for the Docker image plus build cache. The vLLM
  model cache (Mode 2/3) can grow to 30-60 GB depending on which models
  are pulled — that cache lives outside the manyforge tree, under
  NemoClaw-Thor's volume.
- **Local model server (Mode 2/3 only)**: an OpenAI-compatible HTTP
  endpoint such as vLLM, llama.cpp, or any other server that implements
  `/v1/chat/completions`. Default port 8000.

### Repositories

- `manyforge` (this repo) — required.
- `NemoClaw-Thor` — required only when using the integrated OpenClaw stack
  (Jetson Thor inference + sandboxed assistant lane).
- `manyforge_specs` — **dev-only**, not required for runtime/operation.
  Cloned alongside `manyforge` only by maintainers working on specs/ADRs.

### External dependencies (pinned versions and source)

These are baked into the Docker images by `scripts/setup-docker.sh`
and the Dockerfiles. Operators do not need to install or manage them
manually; they are documented here for traceability.

| Dependency | Version / pin | Source | Notes |
| --- | --- | --- | --- |
| Base dev image | `python:3.12-slim-bookworm` | Docker Hub | Dockerfile L31; ROS-free dev environment |
| Frontend builder | `node:22-slim` | Docker Hub | Dockerfile L23; React/Vite build stage |
| ROS image | `ros:jazzy-ros-base-noble` | Docker Hub | Dockerfile.ros-jazzy L11; Ubuntu Noble + ROS Jazzy |
| RoboPlan | `0.4.0` tag SHA `7bddd403cf679dd976135aff6d9412fae9c4c04d` | `https://github.com/open-planning/roboplan.git` | Pinned in `scripts/setup-docker.sh` as `ROBOPLAN_PINNED_REF`. This release includes the mesh/cylinder geometry APIs required by `manyforge_roboplan/`; the Dockerfiles install RoboPlan's modular Python packages from their package roots. |
| Pinocchio (kinematics / dynamics) | `pin==3.9.0` | PyPI | Dockerfile L65; brings `hpp-fcl` / `coal` and Eigen helpers transitively |
| `pybind11` | `3.0.3` | PyPI | Dockerfile L58 |
| `nanobind` | `2.12.0` | PyPI | Dockerfile L63 |
| `topic_based_ros2_control` | commit `7ee291ab13adba52ab5889deb9e520009fe2283d` | `https://github.com/karanchahal-nv/topic_based_ros2_control.git` | Dockerfile.ros-jazzy L17; built into the ROS image; the bridge used by the Isaac lane |
| Eigen 3 | apt `libeigen3-dev` | Debian Bookworm | Dockerfile L45 |
| `urdfdom` | apt `liburdfdom-dev` | Debian Bookworm | Dockerfile L45 |
| `octomap` | apt `liboctomap-dev` | Debian Bookworm | Dockerfile L46 |
| `boost` (filesystem / serialization / system / python / chrono / thread / date_time) | apt | Debian Bookworm | Dockerfile L43-44 |
| `trimesh` | `4.11.5` | PyPI | Dockerfile L61; OBJ/mesh parsing |
| `yourdfpy` | `0.0.60` | PyPI | Dockerfile L62; URDF Python loader |
| FastAPI | `0.135.3` | PyPI | Dockerfile L67; Composer backend |
| Viser | `1.0.26` | PyPI | Dockerfile L66; legacy HMI fallback, started only with `MANYFORGE_ENABLE_LEGACY_VISER_HMI=true` |
| MCP (Model Context Protocol) | `1.27.0` | PyPI | Dockerfile L71; assistant-lane MCP server |

The model endpoints used by Mode 2/3 are configured in
[`examples/model_endpoint_profiles.yaml`](../../examples/model_endpoint_profiles.yaml).
The runtime model server (vLLM) is provisioned by NemoClaw-Thor — see
its repo for model pinning.

**Confirming the RoboPlan ref baked into your image.** The
`package.xml` `<version>` field can lag the actual code (PR branches
carry the same string as the tag they forked from), so
`pip show roboplan` inside the container is not authoritative. After
`setup-docker.sh` runs, the actual ref and resolved SHA are recorded
on the host:

```bash
cat .docker/roboplan/.manyforge-roboplan-ref   # the ref passed to git fetch
cat .docker/roboplan/.manyforge-roboplan-sha   # the resolved commit SHA
```

**Force a refresh** (e.g. to test a newer release tag or local branch):

```bash
rm -rf .docker/roboplan/
ROBOPLAN_REF=<branch|tag|sha> ./scripts/setup-docker.sh
```

With no `ROBOPLAN_REF`, the script uses the RoboPlan 0.4.0 SHA pinned at
`scripts/setup-docker.sh` (`ROBOPLAN_PINNED_REF`). Maintainers move that pin
forward only after Docker build and RoboPlan adapter tests pass.

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

**Not a replacement for `scripts/launch.sh`.** This script is the
NemoClaw/OpenClaw integration lifecycle helper. Use `launch.sh` for
ManyForge bring-up; use `manyforge-stack.sh` only when you need to
wire the running stack into NemoClaw mux/plugin workflows.

Purpose:

- start/stop the ManyForge stack with NemoClaw mux integration
- manage the NemoClaw plugin lifecycle (`plugin deploy`,
  `plugin rollback`, `plugin status`, `plugin list`)
- drive sandbox smoke tests (`smoke direct|mux|all`)

Typical usage:

```bash
./scripts/manyforge-stack.sh up                  # ManyForge + mux proxy
./scripts/manyforge-stack.sh up --with-nemoclaw  # + NemoClaw integration
./scripts/manyforge-stack.sh status
./scripts/manyforge-stack.sh smoke all
./scripts/manyforge-stack.sh plugin status
./scripts/manyforge-stack.sh down
```

### `scripts/configure-manyforge-provider.sh`

NemoClaw/OpenClaw configuration helper, not a ManyForge entrypoint.
Configures the current mux/plugin-based ManyForge provider path on the
NemoClaw side. Use only when wiring a sandbox to ManyForge through the
current mux baseline; the transport design is not final.

### `scripts/dev-agent-session-start.sh` (dev-only)

Purpose:

- restore sandbox reference materials and contracts for a new
  *maintainer development* session
- upload the engineering reference and specs into the sandbox

Operators do not need this script. It requires the private
`manyforge_specs` repo on disk and is intended for maintainer
dev-assist agents only. For deployment / operator bring-up, use
`scripts/demo-assistant-known-good.sh`.

## Recommended Bring-Up Sequences

### ManyForge only

```bash
docker compose up --build
```

Then use:

- Composer API on port `9000`
- native Composer viewer in the Composer UI

The legacy Viser HMI can still be started for compatibility with:

```bash
MANYFORGE_ENABLE_LEGACY_VISER_HMI=true ./scripts/launch.sh start
```

This profile does not require ROS, Isaac Sim, or a robot controller. The
native execution adapter is in-process: reset and retry only affect ManyForge's
software state and RoboPlan-backed scene projection. No external robot or
simulator is commanded.

### Isaac / ROS 2 controller execution

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

2. If you want to inspect the raw Isaac side manually, confirm these topics:

   ```bash
   docker compose --profile ros-full run --rm --no-deps ros-jazzy-full \
     bash -lc 'source /opt/ros/jazzy/setup.bash; ros2 topic list | grep isaac_joint'
   ```

   Expected topics once Isaac playback and ROS publishing are active:

   - `/isaac_joint_states`
   - `/isaac_joint_commands`

3. The `isaac-topic-control` service launches:

   - `topic_based_ros2_control/TopicBasedSystem`
   - `joint_state_broadcaster`
   - `isaac_arm_controller`
   - `/isaac_arm_controller/follow_joint_trajectory`
   - `manyforge_isaac_topic_command_gate`
   - `manyforge_isaac_demo_supervisor`

   Keep this container running while Composer executes against Isaac. Host IPC
   is required for reliable ROS 2 action/service behavior across containers;
   the compose service already sets `ipc: host`. If the image has not been
   rebuilt with `topic_based_ros2_control` yet, the script can build a temporary
   overlay from `${ISAAC_ROS_WS}/src/topic_based_ros2_control`.

4. If you want to run the services manually instead of using the one-command
   launcher:

   ```bash
   docker compose --profile ros-full up isaac-topic-control composer-ros
   ```

   The supervisor still handles startup-order recovery and program/deployment
   loading. Set `SUPERVISOR_AUTO_LOAD=false` to disable that behavior.

5. If automatic loading is disabled, load the portable program and ROS
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

### Direct local model testing

1. Start the model server separately.
2. Point ManyForge deployment/model alias at that endpoint.
3. Prefer the semantic demo in text-only mode first.

### NemoClaw/OpenClaw integration

1. Bring up the local provider/model path in `NemoClaw-Thor`.
2. Run `scripts/configure-manyforge-provider.sh`.
3. Validate tool discovery and health before testing agent behavior.

(Maintainers working on specs/ADRs additionally run
`scripts/dev-agent-session-start.sh` to seed sandbox dev-context;
operators don't need it.)

## Operational Notes

### Temp demo artifacts

The semantic demo launcher creates temp files such as:

- `.manyforge-demo-shape-pick.<id>.program.yaml`
- `.manyforge-demo-shape-pick.<id>.deployment.yaml`

These are per-run rewritten artifacts. They are not source docs and should not
be treated as durable repository content.

### Model aliasing

Composer nodes select deployment aliases, not raw model IDs. In single-alias
cases the UI should show resolved read-only info, not fake choice.

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

Check deployment aliases rather than raw model names. The node chooses the
alias; the deployment maps that alias to the actual provider/model endpoint.

### Sandbox agent cannot reach ManyForge cleanly

That is expected on the current platform baseline. The OpenClaw lane
provisioner ([`manyforge/setup-manyforge-assistant.sh`](https://github.com/pastoriomarco/NemoClaw-Thor/blob/main/manyforge/setup-manyforge-assistant.sh))
configures the SSRF egress preset, MCP server registration, and proxy envs
required for the in-sandbox MCP bridge to call back into Composer. Run that
provisioner whenever the deployment sandbox is recreated. For diagnosis,
work through [`COMPOSER-ASSISTANT-RUNBOOK.md`](https://github.com/pastoriomarco/NemoClaw-Thor/blob/main/manyforge/docs/COMPOSER-ASSISTANT-RUNBOOK.md)
gates 4-9 (gateway in sandbox netns, vLLM reachability, bundle-mcp policy,
proxy envs in MCP server config, SSRF L7 paths).

## Keep This File Small

When scripts change:

- update the command examples and scope notes here
- do not paste entire script bodies into this document
- treat the script files as the source of truth
