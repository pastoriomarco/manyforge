> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Stack Setup

Compact setup overview: host requirements, the three execution lanes, the
optional AI assistant, and where the pinned dependencies come from. The
[workbench guide](DEVELOPMENT_WORKBENCH.md) is the complete reference for
building, starting, inspecting and stopping ManyForge.

User-facing product overview belongs in [`../../README.md`](../../README.md).

For runtime/operator-facing reference docs see:

- [`../reference/REST_API.md`](../reference/REST_API.md) — Composer REST surface
- [`../reference/MCP_RUNTIME_CONTRACT.md`](../reference/MCP_RUNTIME_CONTRACT.md) — MCP tool contract
- [`../reference/ASSISTANT_PROVIDER_CONTRACT.md`](../reference/ASSISTANT_PROVIDER_CONTRACT.md) — AI model endpoint and bridge contract
- [`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md) — AI assistant setup and checks

## Clean Machine Install Order

On a Jetson with a small eMMC root, place the workbench caches on the NVMe
(`MANYFORGE_DEV_CACHE_BASE`) as described in
[`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md). On a Jetson, `build`
selects NVIDIA's `noble-jetpack` package suite by itself.

1. Prepare the host: Docker Engine with Compose v2 and BuildKit, `git`,
   Python 3, and enough disk for the image and build caches.
1. Clone `manyforge` and change into the checkout.
1. Optional: write the [site settings](DEVELOPMENT_WORKBENCH.md#site-settings)
   file (robot addresses, project folders) and export
   `MANYFORGE_DEV_SITE_SETTINGS` with its absolute path.
1. Build the image once, then start the workbench:
   ```bash
   ./scripts/compose-dev.sh build
   ./scripts/compose-dev.sh start
   ```
   Open `http://localhost:3000`.
1. Optional, for the AI tab: run your own OpenAI-compatible model server and
   add an `ai_model` component to the Deployment
   ([`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md)).

## Runtime Selection

Robot execution and the AI assistant are independent. Each component of the
Scenario's Deployment runs on one execution lane (`components[].lane`; a
machine member runs on its owner's lane); the assistant is enabled by the
Deployment's `ai_model` component and never changes motion authority,
controller binding, or robot-state precedence. All lanes run ros2_control with
the stock joint trajectory controller, in the same workbench image and
container.

When the Program Runner has no Manager liveness stream, it reads the Active
TCP at registration and before work resumes. If the execution owner is replaced
while reads overlap, a delayed answer from the old owner can arrive after the
new owner's answer and restore the old Active TCP value in the Runner. This
accepted limitation can make a Program's Current TCP selection stale.

### Robot execution lanes

#### Mock

No simulator or robot. ManyForge drives ros2_control mock hardware:

```bash
./scripts/compose-dev.sh start --project examples/mock_dual_arm_workbench.scenario
```

#### Isaac Sim

Start Isaac Sim yourself and open the scene with its ROS 2 bridge and the ROS 2
Simulation Interfaces services; the workbench does not start Isaac Sim or load
worlds. Then start the workbench with an Isaac Scenario project, or Load one in
Composer:

```bash
./scripts/compose-dev.sh start --project examples/sim_ur10e_pick_and_place.scenario
```

The workbench uses host networking so DDS can discover the Isaac endpoints.
Use the same `ROS_DOMAIN_ID` (default 0) on both machines and leave
`ROS_AUTOMATIC_DISCOVERY_RANGE` unset or `SUBNET` when Isaac runs on another
host; export these in the shell that runs `start` (see
[ROS and DDS](DEVELOPMENT_WORKBENCH.md#ros-and-dds)).

A simulation that is not playing never blocks Enable, Activate or Execute.
Simulated robots and tools keep their lifecycle with the reason **Simulation
not playing** (never a fault), and a new motion or gripper command on them is
refused with `simulation_not_playing` (the node fails; the tree handles it).
Play the simulation in Isaac, or from the Program with a **Set simulation
state** node before its motion.

If Isaac is not reachable when the workbench starts, the simulator component
stays Offline with `simulator_unreachable` and a warning. Its state never
latches the cell or blocks Execute; robot and tool health still follows their
own reported states.

For the multi-machine Lite6 trocar workflow with Isaac ROS
YOLOv8/FoundationPose perception, use
[`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md).

<a id="real-lite-6"></a>

#### Real (UFACTORY xArm)

Real robots are UFACTORY xArm-family robots (xArm 5/6/7, Lite 6, UF850)
through the vendor driver; Lite 6 is the model measured on hardware, and
physical execution is not qualified and requires attended sessions. Put the
robot address in the site settings (`device_addresses`), set
`MANYFORGE_DEV_SITE_SETTINGS`, and Load a Real Scenario such as
`examples/real_lite6_arm_observation.scenario`. The robot is a machine: its
owner component carries the lane (`real`) and, on its machine card, the
address key (`configuration.device_address_ref`); the adapter comes from the
machine model's `lanes.real` row. Load and Prepare connect and clear the
controller's errors with the motors off; the motors switch on only at the
arm's **Enable**. See [Real robot setup](REAL_ROBOT_SETUP.md) for the full
procedure, what each action does and troubleshooting.

Execute, Run, Resume and Robot Control motion stay unavailable until the arm
is enabled and the cell is not latched. Browser state is only an operator aid:
the backend and execution adapter enforce the same refusal.

For a real robot, the controller/driver observed state is the source of truth,
not ManyForge-authored state:

- reset clears ManyForge execution state and resyncs from the controller; it
  does not command hardware to a home pose unless a motion node does so,
- controller path/goal tolerances stay enabled,
- any external safe-state request is an integration hook to the cell's
  independent safety chain; ManyForge is not a certified safety controller.

**TCP offset on the robot's controller.** A Real robot component has a
**Send TCP offset to controller** checkbox in the Deployment editor
(`controller_tcp_offset_sync`, selected by default; Save and Apply). When to
turn it off is in [TCPs](../authoring/TCPS.md#the-controllers-tcp-offset).
ManyForge tells the controller only on events, never by comparing before each
move:

- **A change of the robot's Active TCP** (a move to another TCP, Set TCP, a
  Robot Control selection) sends the TCP offset when offset sync is selected
  and the controller can take it. The xArm settles at rest, re-engages the
  controller and checks the reported offset against the one sent; a mismatch
  fails with `tcp_offset_not_applied` ("The robot controller did not apply the
  TCP offset"). The move that needs the TCP is sent only after this step. A
  Stop during the step lets it finish (a vendor call
  cannot be cancelled), and the pending move is then not sent. A move that
  changed the TCP and then failed still finishes the TCP change. Set TCP is
  allowed while the robot is not enabled; Enable then re-syncs the configured
  offset target.
- **A reset of the Active TCP** sends the End-effector TCP's offset at once,
  or offset 0 if there is no End-effector TCP, when offset sync is selected
  and the controller can take it
  ([TCPs](../authoring/TCPS.md#the-active-tcp)). If the controller is disabled,
  not prepared, in error or not connected, Reset still completes and the next
  Enable re-syncs it. With offset sync off, Reset changes ManyForge's
  selection only; Enable sends offset 0 if the controller reports another
  offset.
- **Enable** reads the controller's offset and sends the configured target if
  it differs: the Active TCP's offset with sync selected, or offset 0 (the
  flange) with sync off. It then sends the total payload before switching the
  controllers on. This is part of the Enable transition and its budget. A
  robot without an End-effector TCP still enables; its offset target is zero.
- **Scene projection changes** update the total payload, including Program,
  direct and automatic grasp-loss detach, also while the robot moves. A
  successful host projection transaction or rebuild prompts the update. When
  the total changes on an enabled robot, it uses `set_tcp_load` only; it does
  not send an offset or re-arm. An object without a declared payload or with
  payload 0 changes nothing. The controller provides no payload readback, so
  external payload edits are not detected. A composition failure or an
  unconfirmed update can fault the robot. Enable and each new motion goal also
  check the host's current projection; see
  [TCPs](../authoring/TCPS.md#attach-to-the-active-tcp).
  After a Manager restart, a previously published physical Scene may have no
  initialized projection in the new host. Enable and physical motion remain
  unavailable until the projection is established; restart does not itself
  reconstruct it.

An ordinary move, or one with the robot's current Active TCP, sends no TCP
offset and reads no offset report. A replacement that takes over a moving robot sends
nothing; one that resolves to a different TCP is refused. With the checkbox
off, changing TCP only changes ManyForge's selection; no offset is sent. The
configured controller offset target is 0 (the flange), and Enable re-syncs
zero if the controller reports another offset. Payload updates are still sent
as above. Controller-side TCP features then refer to the flange. If a sent offset is not confirmed (no
answer, a failed re-engage, or the mismatch above), the arm reports a
component fault; Recover clears this persistent fault, and the subsequent
Enable re-syncs the Active TCP, sending its offset if the report differs. An
offset changed in UFACTORY Studio is corrected at the next Enable. The accepted
late-request residual is described in [TCPs](../authoring/TCPS.md#the-controllers-tcp-offset).

**Motors.** Prepare and Recover never switch the motors on; the arm's
**Enable** does (`motion_enable`, `set_mode(1)`, `set_state(0)` through the
vendor hardware plugin). **Disable** switches off the trajectory controller
and the ros2_control hardware interface, so ManyForge sends no more commands;
it does not call `motion_enable(false)`. To power the arm down, press the
emergency stop, use **Power off** on the machine, or the vendor tools. An
error left after an emergency stop (for example C2) is normal: release the
e-stop, then Recover the machine and Enable the arm
([Real robot setup](REAL_ROBOT_SETUP.md#what-each-action-does-on-an-xarm)).

### Optional model and assistant surfaces

#### Runtime model nodes

Runtime nodes such as `query_model` name a model alias of the Deployment's
assistant profile; each alias resolves to one of the Deployment's `ai_model`
components (the chat uses the same mechanism for its alias). A token, if any,
comes from the component's `api_key_env` variable (`MANYFORGE_AI_*`).

#### Composer AI assistant

The AI tab chats with a model that edits the Program Draft through a fixed
tool set (mode `composer-assistant`). The model server is yours: any
OpenAI-compatible server with tool calling (llama.cpp `llama-server --jinja`,
vLLM `--enable-auto-tool-choice --tool-call-parser ...`). The Deployment names
it with an `ai_model` component (`base_url`, `model`, optional token
environment variable name `api_key_env`, which must start with
`MANYFORGE_AI_`) next to an assistant profile. Run
`export MANYFORGE_AI_<NAME>=<token>` before starting the workbench. Prepare
and Enable never refuse because of the model server; its reachability is the
freshness of the component's `endpoint` stream, which only shows stale and
never faults the component.

The workbench always starts the assistant bridge on `127.0.0.1:8100`. The AI
tab is enabled when the active Deployment has an AI model component; otherwise
it shows "No AI model component in this Deployment". Assistant edits land in
the Draft; Apply, or Discard/Revert, as for manual edits.

Setup and checks: [`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md).

## Prerequisites

### Host

- **OS**: Linux with Docker host networking (Ubuntu 24.04 LTS is the
  reference). Docker Desktop on macOS/Windows is not supported.
- **Architecture**: x86_64 or arm64 (including Jetson). Isaac Sim itself runs
  on a separate x86_64 host or the same x86_64 host, with an NVIDIA driver and
  RTX-class GPU.
- **Docker**: Docker Engine with BuildKit, plus the Compose v2 plugin
  (`docker compose ...`, not the legacy `docker-compose`).
- **GPU**: not required by ManyForge. A local model server for the AI
  assistant usually needs one; it runs outside ManyForge.
- **RAM**: 16 GB minimum for the C++ build; lower `MANYFORGE_DEV_BUILD_JOBS`
  if compilation exhausts memory.
- **Disk**: the image is about 5.4 GB; build caches and runtime state live
  under `.manyforge-dev/`. Model files live outside the manyforge tree.

### Repositories

- `manyforge` (this repo) is the only repository needed to build and run
  ManyForge.

### External dependencies

`./scripts/compose-dev.sh build` downloads and builds the pinned dependencies;
you do not install them yourself. The pins live in `scripts/setup-docker.sh`
(RoboPlan, ProxSuite, TOPPRA) and `Dockerfile.ros-lyrical-full` (ROS 2
Lyrical, controller overlay, `topic_based_ros2_control`, xArm driver,
Python packages).
[`../../NOTICE`](../../NOTICE) lists them with their licences, and
`third_party/roboplan/README.md` records the RoboPlan source identity and
patches.

**Confirming the RoboPlan ref.** After a build, the prepared ref and resolved
commit are recorded on the host:

```bash
cat .docker/roboplan.manyforge-ref   # the ref passed to git fetch
cat .docker/roboplan.manyforge-sha   # the resolved commit SHA
```

**Trying another RoboPlan revision** (development only):

```bash
rm -rf .docker/roboplan/
ROBOPLAN_REF=<branch|tag|sha> ./scripts/compose-dev.sh build
```

Without `ROBOPLAN_REF`, the reviewed commit pinned as `ROBOPLAN_PINNED_REF` in
`scripts/setup-docker.sh` is used. An override is reported as such and the
checked patch manifest must still apply.

## Operational Notes

### Model aliasing

Assistant profiles name model aliases; the Deployment's single `ai_model`
component serves every alias, so the served model is always the component's
`model`.

## Troubleshooting

For the cross-cutting symptom → cause → diagnostic-step matrix see
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md), and for startup problems the
[workbench troubleshooting table](DEVELOPMENT_WORKBENCH.md#operation-and-troubleshooting).

### Tree does not progress after Execute

Every Program starts held: press **Run** on each motion group in the Motion
panel. If it still does not progress, check, on the Composer API
(`http://127.0.0.1:9020`):

- Execute/Resume readiness and its blocked reasons via
  `GET /api/program/readiness`
- the cell latch via `GET /api/cell/latch`
- cycle state and recovery offer via `GET /api/cycle/status`
- current alarms via `GET /api/execution/alarms`
- recent events via `GET /api/execution/history`

### Model selection appears ignored

The model name sent to the server is the `ai_model` component's `model`. The
server must serve exactly that name (llama.cpp `--alias`, vLLM
`--served-model-name`).
