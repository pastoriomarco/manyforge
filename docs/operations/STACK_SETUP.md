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

On a Jetson Orin with a small eMMC root, place the workbench caches on the
NVMe as described in [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md).

1. Prepare the host: Docker Engine with Compose v2 and BuildKit, `git`,
   Python 3, and enough disk for the image and build caches.
1. Clone `manyforge` and change into the checkout.
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

Robot execution and the AI assistant are independent. The Scenario's
Deployment selects one execution lane; the assistant is enabled by the
Deployment's `ai_model` component and never changes motion authority,
controller binding, or robot-state precedence. All lanes run in the same
workbench image and container.

When the Program Runner has no Manager liveness stream, it reads the Active
TCP at registration and before work resumes. If the execution owner is replaced
while reads overlap, a delayed answer from the old owner can arrive after the
new owner's answer and restore the old Active TCP value in the Runner. This
accepted limitation can make a Program's Current TCP selection stale.

### Robot execution lanes

#### Mock

No simulator or robot. ManyForge drives ROS 2 mock controllers:

```bash
./scripts/compose-dev.sh start --project examples/ur5e_lite6_wall_workbench.scenario
```

#### Isaac Sim (attach-only)

Start Isaac Sim yourself and open the scene; the workbench attaches to it and
does not start Isaac Sim or load worlds. Then start the workbench with an
Isaac Scenario project:

```bash
./scripts/compose-dev.sh start --project examples/isaac-ur10e-light-scene.scenario
```

The workbench uses host networking so DDS can discover the Isaac endpoints;
export any DDS settings (`ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION`, …) in the shell
that runs `start` (see [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)).
The Isaac `commanded_sim` profile and the physical controller-observed profile
have different state precedence; see
[`../reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md#deployment-lanes).

For the multi-machine Lite6 trocar workflow with Isaac ROS
YOLOv8/FoundationPose perception, use
[`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md).

#### Real (Lite 6)

Lite 6 is the supported Real model; physical execution is not yet qualified
and requires attended robot sessions. Put the robot address in the site
settings (`device_addresses`, see
[`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md#project-folders)) and
start a Real Scenario such as `examples/lite6_real_arm_observation.scenario`.
A Real robot component names its vendor adapter explicitly in
`runtime_configuration.adapter_family` (`xarm` for UFACTORY arms; the Real
examples set it). ManyForge does not infer the family from the model. Apply
refuses a Real robot without a supported family
(`real_adapter_family_unknown`).
**Run**, Resume, retry/replan, and Robot Control approval stay disabled until
the robot is reported enabled and motion admission is open. Browser state is
only an operator aid: the backend and execution adapter enforce the same
refusal.

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
as above. Controller-side TCP features then refer to the flange. The adapter's
`enable_only` mode never sends offsets. If a sent offset is not confirmed (no
answer, a failed re-engage, or the mismatch above), the arm reports a
component fault; Recover clears this persistent fault, and the subsequent
Enable re-syncs the Active TCP, sending its offset if the report differs. An
offset changed in UFACTORY Studio is corrected at the next Enable. The accepted
late-request residual is described in [TCPs](../authoring/TCPS.md#the-controllers-tcp-offset).

**Disabled does not power down the motors.** On a UFACTORY arm, **Disable
robot** (and the state **Disabled**) switches off the trajectory controller
and the ros2_control hardware interface, so ManyForge sends no more commands.
The vendor driver's deactivation only changes the arm's mode: the servo
motors stay enabled and hold the arm in place. To power the motors down,
press the emergency stop, or switch them off with the vendor tools (UFACTORY
Studio, or `motion_enable(false)` in the vendor SDK). The next **Prepare**
enables the motors again through the vendor activation. An error the
controller still reports after an emergency stop has been released (for
example C2) is shown on the component and cleared by the next **Prepare**. If
the emergency stop (or its input) is still engaged, that Prepare fails with
`xarm_error_persists_after_clear` and the error code. You can try again after
releasing it. An emergency stop while the arm is
working is an incident that stops the cell; **Recover cell** then tries to
enable the arm again (Recover, Prepare, Enable) in the same press, and can be
pressed again if it fails. Its Recover clears the error first. Before sending
`clean_error`, the adapter reads `get_err_warn_code` and `get_state` on demand,
each under the remaining step budget and the 10 s call cap. An answered service
read error refuses the clear without sending it; an unanswered read leaves the
clear unsent and reports `robot_controller_unresponsive`. After `clean_error`
answers, it schedules three on-demand `get_err_warn_code` reads at 0, 150 and
300 ms; when those services answer promptly, verification usually takes about
0.3 s. A read with error code 0 proves the clear immediately, without another
state read. When an error remains after polling, the adapter reads `get_state`
once and compares the final on-demand error, warning and state values before
and after.
If the step expires before that state read can complete, the step fails with
`component_lifecycle_deadline_expired`. A completed persistence
result uses `xarm_error_persists_after_clear` and includes the latest error
code. The clear is judged only on those three values (error, warning, state)
before and after; the mode and motion enable are set and checked by Prepare
and Enable. As for every lifecycle step, the result is succeeded, failed or
connection lost, and what comes next is decided from the arm's current status.
No answer within a call's allowed time reports the controller unresponsive; the clear request is never repeated. The
full Recover can take longer because it also performs baseline, status and
proof calls within the component budget; measure the complete path on the live
controller. If the robot controller or its controller
manager does not answer in time, the step ends at once with
`robot_controller_unresponsive` or `controller_manager_unresponsive` and the
arm reports a component fault (an incident) until a Recover finds both
answering again. A Recover press acknowledges only the incidents it
showed: if a newer incident replaced one, the press is refused
(`cclc_incident_changed`, nothing acknowledged) and shows the new incident;
review it and press again. If the incident was closed meanwhile and nothing
else is open, the press proceeds as an ordinary Recover. API clients may send the same check
(`expected_incident_ids` on `POST /api/execution/cell-recover`,
`expected_incident_id` on a lifecycle Recover) or omit it. Prepare also reactivates the joint
state broadcaster if the hardware plugin left it inactive, and waits for a
broadcaster sample after the activation; **Enable** activates the trajectory
controller. Each of these steps is bounded at 10 s on a UFACTORY arm.

**Moving the arm from UFACTORY Studio.** While no Program runs and no
ManyForge motion or operation holds the cell, a move or mode change from
UFACTORY Studio is a warning on the component
(`moved_externally:rearm_at_next_motion`), not an error. Press Run, Execute,
jog or Move to as usual. During the Studio move the
3D view and joint values follow the arm from the driver's own joint states
(display only); those samples also keep the arm's joint stream fresh, so a
lost joint-state broadcaster is not reported until they stop. Once the motion
is admitted, and before it is planned, the arm's vendor adapter puts the robot
back into joint-servo mode and the ready state (`set_mode(1)`, `set_state(0)`);
the driver then switches its controllers back on, the trajectory controller
holding the current pose, and the motion starts from there (about 2 s more).
While the arm counts as moved, a jog or Move to preview starts from its live
pose (the driver's joint states). Executing a preview made before the manual
move plans the motion again from the current pose after the re-engage; only if
the arm moves between that planning and the motion's start is it refused
(`start_state_mismatch`, "preview again"), and nothing moves. While the arm is
still moving under Studio, or the robot reports an error (an emergency stop is
never re-enabled automatically), the motion fails typed
(`motion_reengage_failed` with the cause) and nothing moves. **Recover cell**
still re-arms a moved arm explicitly, and is the way back when the re-engage
fails. While a Program or ManyForge motion is in progress the same move is an
`external_control` incident: the cell stops, and only the operator's
**Recover** re-arms the arm.

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
and Enable never refuse because of the model server (with the default warning/auto `endpoint` stream); its reachability is the
component's `endpoint` stream warning.

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
Lyrical, controller overlay, xArm driver, `ur_msgs`, Python packages).
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

### Tree does not progress after `Run`

Check, on the Composer API (`http://127.0.0.1:9020`):

- cycle state and recovery offer via `GET /api/cycle/status`
- current alarms via `GET /api/execution/alarms`
- recent events via `GET /api/execution/history`

### Model selection appears ignored

The model name sent to the server is the `ai_model` component's `model`. The
server must serve exactly that name (llama.cpp `--alias`, vLLM
`--served-model-name`).
