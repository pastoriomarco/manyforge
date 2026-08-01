# Lite6 Live Qualification

> **Do not run this procedure from the current `dev` branch.** The
> composable-cell destructive migration has intentionally taken the real-robot
> product lane offline until the I10/R restoration gate. This document records
> the last qualified baseline only. Its exact source is
> `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`, blob
> `b6a6796ad8b14f1ed1d5c6585c3a5677ad848926`; it is a reimplementation and
> acceptance reference, not a runnable fallback.

This runbook qualifies one physical Lite6 for the experimental ManyForge
real-robot lane. It separates safe observation, supervised lifecycle
qualification, and physical motion. Passing an earlier stage never authorizes a
later one.

That last qualified baseline supported:

- deterministic generation and validation of the exact production controller
  binding;
- the XC-6 observation-only launch;
- automatic, bounded evidence capture for the read-only graph;
- an offline fake-hardware qualification using the exact generated
  controller-manager/JTC binding;
- an offline-qualified xarm_ros2 sole-owner session façade, restricted ROS
  state/request/result boundary, and separately addressed lifecycle catalog;
- layered transport health: a 250 ms generic-admission publication lease, a
  1000 ms producer-observation/write-gate lease, and a 2000 ms terminal
  report/control-loss timeout that requires a fresh owner process;
- deterministic runtime-token resolution with a digest-bound runtime manifest;
  and
- a command-free, E-stop-pressed supervised-session and ros2_control
  configuration preflight;
- an offline-qualified, attended one-command evidence path through the
  local-operator gateway and restricted ROS owner;
- bounded transient sampling for Enable/Start that excludes unqualified
  zero-placeholder poses from motion metrics; and
- an offline-qualified, no-trajectory controller/JTC command-gate evidence
  transaction;
- a robot-neutral execution binding monitor and lifecycle-admission gate that
  rejects before FJT enqueue and cancels on authority loss; and
- an offline-qualified manifest-bound admission provider plus strict generic ROS
  ingress, with a per-process identity and bounded source lease; and
- an offline-qualified, separately addressed guarded-controller-shutdown
  boundary whose physical row remains unexecuted;
- an experimental product `real-robot` lane with a portable cell/template,
  retained run-specific provenance, separate sole-owner control and non-owner
  Composer services, coordinated Enable/Disable controls, and a
  browser-independent headless authority heartbeat;
- a commit-bound physical minimum gate for the initial unprefixed
  `local_operator` profile: observation-only reload, durable enabled-idle
  authority, exact live controller binding, controller-loss fencing and explicit
  re-arm, UFactory Studio Stop fencing, and non-drive-toggling recovery from an
  attended mode-0/state-2 baseline established by one restricted, non-public
  test-setup call; a natural external source for that exact baseline remains
  unqualified;
- startup epochs and receiver-local critical-progress reduction for the cell
  supervisor, transport owner, lifecycle provider, generic execution backend,
  and selected control authority;
- a composed upper cycle/lower ros2_control execution pin that fails closed
  across planning, final dispatch, goal response, active monitoring, resume, and
  successful terminal commit;
- a host-side command-free supervisor with a host-only control channel,
  receiver-local monitoring of Composer's advancing reduced-health snapshot,
  immediate identity/process-loss handling, 2 s persistence for a merely
  reduced-unhealthy graph, and terminal dependency-ordered teardown; and
- a no-dispatch utility that prepares an attended, at-most-1-degree Robot
  Control preview from fresh observed state.

The earlier direct-SDK candidate had been removed by that baseline. Its
production path loaded the fork's `UFRobotSystemHardware`, which owned one
unmodified SDK 1.18.1 session; ManyForge and PLC/ROXSIE communicated with that
owner only through ROS. The vendor lifecycle primitives and transition table
were live-gated. A resolver produced and verified the retained
address-bearing runtime artifact before configuration. A future I10/R port
must preserve sole SDK ownership rather than starting a parallel driver or
calling the SDK from ManyForge.

SDK teardown races were a documented deferred hardening concern. The captured
prototype mitigated them in `xarm_ros2`: configuration owned startup, teardown
was serialized with the owner's session, no shutdown was requested while
initialization was in flight, transport loss was terminal, and recovery
required a fresh process. The owner did not rely only on SDK socket flags:
prolonged absence of rich reports or successful control reads independently
promoted the session to terminal loss. No local SDK source modification was
retained.

## Frozen initial binding

The binding manifest is compiled from the generated supervised URDF and
generated xArm controller YAML. Generation fails if either artifact drifts. The
initial, unprefixed single-robot profile is:

| Fact                                 | Required value                                          |
| ------------------------------------ | ------------------------------------------------------- |
| Controller manager                   | `/controller_manager`                                   |
| Controller manager update rate       | 150 Hz                                                  |
| Hardware component                   | `lite6_system`                                          |
| Hardware plugin                      | `uf_robot_hardware/UFRobotSystemHardware`               |
| Trajectory controller                | `/lite6_traj_controller`                                |
| Controller type                      | `joint_trajectory_controller/JointTrajectoryController` |
| Action                               | `/lite6_traj_controller/follow_joint_trajectory`        |
| Supervised state                     | `/ufactory/supervised_lifecycle/state`                  |
| Supervised lifecycle request         | `/ufactory/supervised_lifecycle/execute`                |
| Supervised command gate              | `/ufactory/supervised_lifecycle/set_command_gate`       |
| Generic execution admission          | `/manyforge_execution_admission`                        |
| Joints, in order                     | `joint1` through `joint6`                               |
| Command interfaces, per joint        | `position`, `velocity`                                  |
| State interfaces, per joint          | `position`, `velocity`                                  |
| Controller state publication         | 25 Hz                                                   |
| Action monitoring                    | 10 Hz                                                   |
| Controller activity QoS              | reliable, transient-local, depth 1                      |
| Runtime binding snapshot maximum age | 1000 ms                                                 |
| Generic admission lease / refresh    | 250 ms / 50 ms                                          |
| Producer observation / xArm gate     | 1000 ms / 1000 ms                                       |
| Terminal report/control-loss timeout | 2000 ms                                                 |
| Reduced-health teardown persistence  | 2000 ms                                                 |

The exact robot/controller values and artifact SHA-256 digests live in
`binding-manifest.json`; the external-supervisor value is retained in each run's
`cell-supervisor-policy.json`. This table is an operator summary. A prefixed or
namespaced deployment must produce and validate its own manifest.

Generate the model, manifest, and validation result without robot or network
access:

```bash
docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/generate-lite6-fixed-pneumatic-model.sh
```

The expected outputs under `.manyforge-var/real-lite6/` are:

- `model/lite6_fixed_pneumatic.supervised.runtime.urdf`: unresolved,
  production-owned template;
- `model/lite6_fixed_pneumatic.supervised_fake.runtime.urdf`: test-only
  equivalent using the fake backend;
- `model/ros2_controllers.yaml`: exact xArm controller configuration;
- `binding-manifest.json`: immutable names, order, interfaces, QoS, rates,
  digests, and generation IDs; and
- `controller-binding-validation.json`: independent recompilation result.

Resolve one deployment instance with network disabled:

```bash
docker run --rm --network none \
  -e MANYFORGE_LITE6_ROBOT_IP=192.168.1.31 \
  -e MANYFORGE_LITE6_RUNTIME_BINDING_CONFIRM=RESOLVE_SUPERVISED_RUNTIME \
  -e MANYFORGE_LITE6_LIFECYCLE_PROVIDER_INSTANCE_ID=lite6-a:provider:offline-example \
  -e MANYFORGE_CELL_SUPERVISOR_COMPONENT_EPOCH=offline-supervisor-example \
  -e MANYFORGE_CELL_SUPERVISOR_HEARTBEAT_PATH=/tmp/manyforge-real-lite6/cell-supervisor-heartbeat.json \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/resolve-lite6-supervised-runtime.sh
```

The resolver permits only `local_operator` or `plc_roxsie`, substitutes exactly
the address and sole-owner tokens into the hardware URDF, records the
robot-instance, authority, and per-process admission-provider identities in the
runtime manifest, rejects any remaining token, recompiles the controller
binding, preserves model semantics, and records the resolved URDF digest. A live
supervised-owner launch generates a fresh provider UUID automatically; the
stable example above is only for network-isolated artifact inspection. The
resulting address-bearing artifact is runtime state and must not be committed.

`test-xarm-lifecycle-fake.sh` loads the test-only URDF through the real Jazzy
`ResourceManager`, starts the real `controller_manager` and JTC using the
generated YAML, and verifies the exact component, type, update rate, joint
order, and all 12 claimed command interfaces.

The runtime manifest also carries `ros2_control_execution_binding`, a
robot-neutral projection of the exact generated facts. The generic backend
requires the initial reliable, transient-local controller-manager activity
generation before service snapshots can become valid. It invalidates first on
every activity event, rechecks the controller, hardware component, claims,
interfaces, and conflicts in NRT work, and serializes the final fresh
binding/admission check with FJT enqueue.

It additionally carries `execution_admission` and `manyforge_execution_config`.
The supervised-owner launcher uses the first to start exactly one xArm
lifecycle/admission provider. That provider observes the restricted xarm_ros2
state plus controller activity and publishes a reliable transient-local
robot-neutral snapshot at 50 ms with a maximum 250 ms lease. The generic
subscriber checks exact provider/robot/binding identity, generation
monotonicity, semantic-generation changes, sorted reason codes, and the original
source deadline before publishing into the native execution bundle. Exact
duplicate delivery never renews the lease.

The provider and subscriber must run on the same Linux host. The source deadline
uses the host's `CLOCK_MONOTONIC` boot epoch, which is shared across ordinary
containers but not across different machines. A cross-host deployment needs a
separately versioned time/lease transport contract.

Provider startup is deliberately observation-only and cannot arm the robot. The
experimental product launcher now starts Composer separately, consumes the
resolved `manyforge_execution_config`, and offers the `local_operator` profile
through the same lifecycle owner. Composer never calls vendor or
controller-manager services directly. A host-side cell supervisor observes both
services; its control directory is deliberately outside their writable mount. It
also consumes the advancing reduced `cellSupervision` status under
receiver-local freshness, so a frozen-but-running process is not accepted as
healthy. It performs teardown only and never issues a robot command or automatic
restart. The `plc_roxsie` ingress remains later work and is mutually exclusive
with local-operator authority.

## Evidence rules

Evidence is stored below the ignored runtime root
`.manyforge-var/real-lite6/live-evidence/` for isolated owner/read-only tools,
or below the active product run's `live-evidence/` directory. It is not a source
artifact and must not be committed. Each command-bearing capture contains:

- `capture-provenance.json`: image identity and ManyForge commit/tree state,
  without the robot address or a physical unit identifier;
- `evidence.json`: overall result and every bounded event;
- `events.jsonl`: one machine-readable event per observation; and
- `raw/`: bounded raw output with a SHA-256 digest recorded in the event.

Each event records the exact argv, timeout, duration, return code, raw-output
digest, required/optional status, postcondition, and postcondition result. A
timeout is always an unavailable or failed observation; it is never interpreted
as a pass. Output is capped at 1 MiB per probe, and truncation fails the
corresponding gate.

The validated joint-state probe is optional in read-only mode because a
drive-disabled Lite6 can legitimately withhold a trustworthy physical pose. Its
timeout is recorded as unavailable, while all graph, endpoint, diagnostic,
raw-state, and raw-joint probes remain mandatory.

For the product lane, capture one bounded read-only snapshot before and after
each lifecycle/supervision row:

```bash
MANYFORGE_LITE6_CAPTURE_LABEL=before-enable \
  ./scripts/xarm/capture-lite6-product-lane-state.sh
```

The helper resolves the active run through the host-only launcher record and
captures runtime, lifecycle, cycle, Kernel, external-supervisor, and limited
container-state evidence. Every raw object is capped at 1 MiB and hashed. It
does not call a lifecycle, controller, execution, or robot-command endpoint. A
missing/stale endpoint or malformed/oversized payload makes the capture fail; it
is never converted into a pass.

The capture also retains a `robot-lifecycle-history` artifact: a bounded recent
tail of at most 256 source-ordered lifecycle baselines, material observed
transitions, and transaction terminals selected from the newest 300 records in
Composer's independently retained robot-lifecycle channel. It is focused
qualification context, not an exhaustive replacement for durable JSONL history.
It is transition evidence, not a causal reconstruction: an observed state change
is labelled external/unknown unless it is temporally correlated with an exact
provider transaction or Composer command. Repeated unchanged publications and
counter/timestamp churn do not create additional records.

When supervised lifecycle commands are physically qualified, the same
qualification session must append one event for each command with:

1. authority profile, session/generation, request ID and intended primitive;
1. precondition snapshot and its age;
1. exact vendor primitive and arguments;
1. whether it was permitted and attempted;
1. vendor return code and reason;
1. bounded postcondition observations and deadlines; and
1. reconciliation result after timeout, cancellation, or authority loss.

No command may be inferred successful from a ROS return code alone. The observed
robot postcondition is authoritative.

The initial timing profile reserves four seconds of each five-second lifecycle
transaction for discovery plus the response to the one original owner request,
leaving approximately one second for fresh postcondition reports. The client
does not resend a physical request. If the bounded wait expires after dispatch,
the audit records `outcome_unknown_after_dispatch`, admission remains closed,
and explicit state reconciliation is required even when the physical
postcondition later appears correct.

The maintained command capture implements this envelope for one action at a
time. Its installed-process test uses a fake restricted ROS owner and proves the
exact path
`LocalOperatorGateway -> XArmLifecycleRosPort -> restricted ROS service`,
including authority closure. That offline pass qualifies the tooling, not a
physical primitive.

For `robot.enable` and `robot.start_control`, the command runner arms a
read-only state/joint sampler before dispatch. Failure to arm the sampler
prevents the command. The default three-second post-response window records
initialization delay, source cadence, first valid pose, and maximum deviation
from that first valid pose. Samples count toward the deviation metric only after
the owner reports a valid pose and all six brake/servo bits. An explicit
`MANYFORGE_LITE6_TRANSIENT_POSE_LIMIT_RAD` converts the metric into an automatic
bound; leaving it unset records `captured_requires_bound_adjudication` and does
not invent an acceptance threshold.

## Stage A — offline exact-binding qualification

Use the image built from the intended xArm worktree and run:

```bash
docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/test-real-lite6-runtime.sh

docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/test-xarm-lifecycle-fake.sh
```

The second command must report the exact generated binding test as executed, not
skipped. Run the fresh-process stress and sanitizer lanes described in
`manyforge_xarm_control/README.md` before accepting a source revision.

### 2026-07-25 generic admission checkpoint

The XC-7B offline implementation now consumes the exact generated binding
without xArm types or naming rules in the generic backend. Its immutable
admission publication freezes robot/binding identity, rejects generation or
timestamp regression, and requires a new lifecycle generation for a semantic
readiness/admission change. Dispatch pins the controller-manager activity and
lifecycle generations immediately before FJT enqueue. Binding or admission loss
cancels a known active goal and fails execution closed; a delayed goal response
is cancelled if authority changed while the response was in flight.

Container qualification against the final source reported:

- generic C++ release build and CTest: `1136/1136` passed;
- focused ASan/UBSan controller-binding/backend suite: `70/70` passed;
- ROS/Python native-binding contracts: `62/62` passed;
- xArm binding/runtime-manifest projection tests: `64/64` passed;
- exact generated Lite6 fake binding: real Jazzy `ResourceManager`, real
  `controller_manager`, and real JTC loaded `lite6_system`,
  `lite6_traj_controller`, `joint1..joint6`, and all 12 command claims;
- focused update-loop timing, 500 samples per state: blocked p50/p95/p99
  `19.509/21.898/29.871 us`, admitted p50/p95/p99 `19.370/22.093/28.223 us`,
  admitted maximum `78.194 us`, and p95 delta `0.195 us`; and
- the existing 10,000-iteration hot-path probe: zero allocations for both reused
  action-feedback storage and repeated idle-sample skipping, with respective
  means `0.311 us` and `0.138 us`.

These are scoped offline/fake results. A follow-up worktree qualification adds
the actual installed provider process, IDL transport, strict Python ingress, and
native handoff: it observes a fake exact owner as controller-ready but
unadmitted, makes no lifecycle or controller-shutdown call, and removes
readiness after source expiry. This qualifies the wiring structure, not a
physical authority session or trajectory.

## Stage B — XC-6 stationary read-only checkpoint

Before connection:

- verify that the configured IPv4 address is not assigned to another device;
- stop every other xArm driver, ManyMove process, simulator bridge, and
  controller manager that might own the robot;
- keep the cell clear and independent E-stop path available; and
- record the pendant state, mode, drive/brake state, warnings, and errors.

Start the read-only container exactly as documented in
`manyforge_xarm_adapter/README.md`. In a second terminal, capture the bounded
evidence:

```bash
./scripts/xarm/capture-lite6-live-evidence.sh
```

The capture passes only when:

- the expected namespaced driver, state publisher, and observer nodes exist;
- no controller manager is present;
- no ROS action exists;
- no xArm command, enable, fault-clear, mode, gripper, or controller service
  exists;
- diagnostics say `access_mode=read_only`, `motion_admission=closed`, and
  `command_endpoints=suppressed`;
- the connected endpoint reports six axes and the Lite6 SDK device type before
  startup inspection; and
- raw robot state and joint state are observed.

Compare the pendant before and after connection and shutdown. There must be no
movement, drive enable, mode/state change, warning/error clear, or tool command.
A retained or unavailable validated pose is evidence to analyze, not an
instruction to enable the drives.

### 2026-07-24 observation-only transition finding

An attended session kept the ManyForge graph read-only while the operator used
the vendor interface. It established these implementation constraints without
qualifying a ManyForge command path:

- releasing E-stop did not clear the latched controller fault;
- the vendor UI's clear/reset control was composite: it initialized the physical
  pose, cleared the fault, enabled all drives, briefly moved the arm, and
  returned it to the original pose;
- the vendor Stop control changed the robot to stopped but left all drives
  enabled;
- the attended safety-input event, reported by the controller as C2, restored
  the disabled, faulted state and produced only the known Lite6 settling; and
- the vendor joint-state velocity field remained zero during operator-observed
  movement, so it cannot independently certify zero motion.

The same recording measured the ROS joint stream at about `24.2 Hz` overall
(`24.6 Hz` median cadence) and the rich robot-state stream at about `5.0 Hz`.
Both published exact six-axis zeros while drives were disabled. The rich stream
first published the physical nonzero pose before the higher-rate joint stream
had fully stopped publishing placeholder zeros, so neither frequency nor a
nonzero-value test is sufficient by itself.

The production candidate therefore treats exact all-zero joints as a potentially
valid physical pose, not as an error sentinel. It qualifies any pose, including
all zeros, only after:

- identity and control/report transports are valid and fresh;
- all six brake and servo-enable bits are set, with no error or warning;
- the rich report and polled joint source agree within `0.002 rad` on all six
  axes for three distinct rich reports; and
- the joint sample remains inside its freshness lease.

Disabled, faulted, warning, partial-mask, disconnected, report-lost, divergent,
or stale observations invalidate the qualification and close command delivery.
Mode/state readiness remains a separate admission condition; a valid pose alone
never permits motion.

Do not map the composite vendor UI control to **Clear fault**. Supervised fault
clear, drive Enable, Start Control, Stop Control, and drive Disable are separate
actions with separate postconditions. In particular, robot state `4` does not
prove Disable; the observed drive/brake masks must also be zero. E-stop remains
the independent safety path, not a normal Disable implementation.

### 2026-07-24 historical direct-session preflight

With physical E-stop pressed, the now-removed direct-session prototype completed
three fresh command-free sessions against `192.168.1.31`:

| Observation                             | Result                          |
| --------------------------------------- | ------------------------------- |
| SDK / robot firmware                    | `1.17.3` / `v2.7.0`             |
| Identity                                | axis `6`, device type `9`       |
| Final vendor state                      | mode `0`, state `4`, error `2`  |
| Brake / servo-enable masks              | `0` / `0`                       |
| Pose / motion / vendor readiness        | false / false / false           |
| Rich-report rate                        | `4.982–4.984 Hz`                |
| Successful measured SDK joint-read rate | `179.361–179.805 Hz`            |
| Measured joint reads                    | `900/900`, `902/902`, `902/902` |
| Lifecycle calls / joint-write attempts  | `0` / `0` in every run          |
| Disconnect teardown                     | `0.481–5.561 ms`                |

The final corrected evidence is retained under
`.manyforge-var/real-lite6/live-evidence/20260724T230128Z-supervised-preflight-estop-v3/`.
The three raw-log SHA-256 values are:

- `b4f4e103977952161e1cf39f549626d530e8f00ad46a56181eaaa3dd6633da11`;
- `ad43c4f200ab3b4313fd9dfa58a883e0fe8349d9c09e9ba38390794ffb7af203`; and
- `0dba9333da9e8fa724e8e89345e4eff7586b250d74eae370a8a18821e792b596`.

The checked-in conclusion is the observed values and pass boundary, not the
ignored raw files.

The resolved runtime artifact then passed a plugin-level lifecycle check:
`on_init` loaded without a transport, `on_configure` connected, cleanup
disconnected, a second configuration reconnected, and `on_activate` returned
error because E-stop left the pose unqualified. No controller manager update
loop, JTC, authority session, lifecycle primitive, command gate, or trajectory
was started. Its runtime generation ID is
`1e527ac59b8d47479400f207f5c4a0f3be51fa12adc9514aa4ff7e4d59f42bc1`; the resolved
URDF SHA-256 is
`0fc5ab3dad0ce88f0ca94a0124e851c7d66aa691bcdf8d5be1a27cc8f0c1c384`. The retained
plugin log SHA-256 is
`fa85c7bb247a3ad40edb2871c5d91229426cf5671a00e1fbf25ffa1e8eafc793`.

This evidence remains useful for the observed robot behavior but does not
qualify the replacement ROS ownership topology. It does not qualify fault clear,
enable, mode/state selection, stop, disable, controller activation, recovery, or
motion.

## Stage C — supervised non-motion lifecycle qualification

The minimum standalone `local_operator` subset passed physically on 2026-07-27
from committed ManyForge `7f7e35a7` and xarm_ros2 `c3172e57`, using image
`sha256:1ce135e699185584d0a6184bfc05499238620fce37c994cd1a6ad50afd2cef19`. This
releases bounded XC-8 continuation for that exact initial unprefixed profile. It
does not qualify PLC/ROXSIE, every vendor fault, physical controller shutdown,
prefixed/multi-instance deployments, or release-wide real-time behavior.
Continuation is limited to the qualified `1/2`, `1/4`, and exact `0/2` operating
rows under attended operational exclusivity. Do not invoke Prepare from an
unexplained state 5/unsupported mode, use ambiguous shutdown reconciliation, or
treat the prototype as release-ready until the enforcement gaps below are fixed.

Runtime resolution, the command-free transport/configuration subset, and the
reviewed ordinary primitives are complete. The implementation enforces one
transport owner, one vendor primitive per internal stage dispatch,
observation-qualified pose initialization, and a lease-bound command gate.
Startup is observation-only, and the intended robot-lifecycle reacquisition
policy requires an explicit target intent. The current re-arm-provenance gap
below can violate that last requirement and blocks release.

Completed prerequisites:

- resolve all runtime tokens from a validated binding manifest;
- reject an inexact configured endpoint before exposing admission and verify the
  live endpoint as six-axis/device-9;
- configure and clean up the sole-owner plugin without an automatic vendor
  transition;
- prove that E-stop prevents pose qualification and hardware activation; and
- capture command-attempt counters, report/read rates, and bounded teardown.

The restricted internal primitives are separately attributable. Composer exposes
the composite Enable/Disable targets and the guarded recovery actions; it does
not ask clients to sequence these stages:

Three similarly named operations are intentionally different: Composer **Enable
robot** submits the full `robot.prepare_for_motion` target; internal
`robot.enable` performs only `motion_enable(true, id=8)`; UFactory Studio
**Enable** is an external UI operation whose exact constituent vendor sequence
is not yet qualified. Observing the third can never be recorded as if ManyForge
issued either of the first two.

| Internal action                          | Eligible observed start                                                         | Sole primitive      | Required observed result                                                  |
| ---------------------------------------- | ------------------------------------------------------------------------------- | ------------------- | ------------------------------------------------------------------------- |
| `robot.clear_fault`                      | fault present; drives off; state 4                                              | clear error         | error/vendor `recovery_required` clear; drives remain off; state 4        |
| `robot.clear_warning`                    | warning present; drives off; state 4                                            | clear warning       | warning clear; drives remain off; state 4                                 |
| `robot.enable`                           | fault/warning/vendor `recovery_required` clear; drives off; state 4             | enable all drives   | full brake/servo masks; valid pose; state 4 preserved                     |
| `robot.select_external_control`          | fault/warning/vendor `recovery_required` clear; drives on; state 4              | `set_mode(1)`       | mode 1; state 5 (`MODE_CHANGED`); vendor not ready                        |
| `robot.start_control`                    | fault/warning/vendor `recovery_required` clear; drives on; mode 1; state 4 or 5 | `set_state(0)`      | feedback state 2 (`READY`); vendor ready; H/J/A still independently gated |
| `robot.activate_hardware`                | clean, valid, enabled, vendor-ready; H/J inactive                               | activate hardware   | H active; J inactive; vendor result unchanged                             |
| `robot.activate_trajectory_controller`   | clean, valid, enabled, vendor-ready; H active; J inactive                       | activate JTC/JSB    | H/J active; vendor result unchanged; admission still separate             |
| `robot.deactivate_trajectory_controller` | stationary; H/J active                                                          | deactivate JTC      | J inactive; H remains active                                              |
| `robot.deactivate_hardware`              | stationary; H active; J inactive                                                | deactivate hardware | H/J inactive                                                              |
| `robot.stop_control`                     | drives on                                                                       | `set_state(4)`      | state 4; vendor not ready; drives remain on                               |
| `robot.disable`                          | state 4                                                                         | disable all drives  | zero brake/servo masks; state 4; vendor not ready                         |

Every row also requires the fresh expected robot identity and the selected live
owner/provider session. Accepting any standalone primitive closes admission and
the owner command gate and sets the separate `controller_rearm_required`
interlock. Current code clears that interlock after observing J inactive then
active; it does not yet prove that the transition belonged to the explicit
target transaction. `recovery_required` below is the vendor fault-recovery fact
and is not that controller re-arm interlock.

The public target adds stricter stage eligibility: inside
`robot.prepare_for_motion`, `robot.select_external_control` runs only after H/J
are inactive; inside `robot.make_noncommandable`, Stop/Disable run only after J
then H are inactive. Those composite ordering guarantees are not intrinsic side
effects of the raw primitive.

The state-0 value in the **Start control** row is a request, not its successful
feedback value. UFACTORY documents state `5` as the automatic interlock after a
mode change, state `2` as ready to receive and execute, and state `1` as moving.
UFACTORY also documents a raw `set_state(0)` request as clearing the error code.
ManyForge prevents that side effect from becoming an implicit recovery path:
`robot.start_control` and `robot.prepare_for_motion` are unavailable while a
fault, vendor `recovery_required`, or warning is present. The separate
`robot.clear_fault` or `robot.clear_warning` transaction must first pass its own
disabled/state-4 preconditions and observed postcondition.

The adapter also accepts that Ready request only from observed state 4 or 5. It
never sends raw state 0 from state 3, where UFactory documents it as Resume. An
observed moving, paused, or decelerating state must first become stationary and
pass through the explicit Stop/state-4 termination-and-command-rejection stage,
or the target fails closed. Do not infer Studio-style cache clearing from raw
state 4 alone.

The owner therefore keeps command delivery closed in states `0`, `3`, `4`, `5`,
and `6`; it permits initial hardware activation only after fresh state-`2`
feedback. See the official
[UFactory Studio mode/state manual](https://docs.ufactory.cc/user_manual/ufactoryStudio/10.motion_mode_and_state.html),
and upstream
[ROS mode-change sequence](https://github.com/xArm-Developer/xarm_ros#6-mode-change).

### Current mode/state and command map

The notation `M/S` below means reported vendor mode/state. Drive masks `63/63`
mean that all six brake and servo-enable bits are set. `H`, `J`, and `A` mean
the generated ros2_control hardware component, the named JTC, and ManyForge
motion admission. A lifecycle command may briefly invalidate the derived
stationary observation while fresh samples are reacquired; that is not, by
itself, evidence of physical movement.

The source class in the last column is load-bearing:

- **documented** means UFactory defines the primitive semantics;
- **observed** means the transition was retained from Lite6 firmware 2.7;
- **policy** means ManyForge requires the stated ordering and postconditions;
  and
- **open** means the physical row is not yet qualified.

| Input or condition                                                | Mode/state and drive result                                                                                                                               | ManyForge controller/admission result                                                                                                                                                                                                                                                                                                                    | Source and status                                                                                                                                                                |
| ----------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Browser disconnect/reload or Composer UI reconnect                | Preserve every robot fact                                                                                                                                 | The headless authority continues; do not alter H/J, A/gate, lifecycle, or execution merely because the browser changes                                                                                                                                                                                                                                   | Policy; browser-independent authority passed                                                                                                                                     |
| Fresh xArm owner/controller-manager process                       | Preserve the robot's existing mode, state, masks, fault, and warning                                                                                      | Start H/J inactive and A/gate closed; issue no lifecycle primitive or joint write. Replace the lifecycle-provider/transport binding because the old provider rejects a changed owner session, establish fresh selected authority, then explicitly Prepare before motion                                                                                  | Policy; physically passed from enabled `1/2`, `63/63`                                                                                                                            |
| Composer/selected-authority process replacement                   | Preserve robot and H/J facts where that process domain survives                                                                                           | Expire the old session and close A/gate. A fresh authority session and explicit Prepare are required; no drive cycle or automatic resume. Product containment may deliberately tear down a broader process domain                                                                                                                                        | Deterministic policy; physical restart matrix remains open                                                                                                                       |
| Lifecycle-provider/transport replacement                          | Preserve physical vendor facts; H/J may remain in their last observed state                                                                               | Invalidate the provider epoch and admission, let the owner gate fail closed, bind the fresh provider and selected authority to the current owner epoch, and explicitly Prepare. Never inherit the prior transaction                                                                                                                                      | Deterministic policy; isolated physical component row remains open                                                                                                               |
| Full product-lane restart                                         | Preserve the robot's physical mode/state/masks unless an independent safety/vendor event or the separately defined active-motion containment changed them | After any required active-motion containment, combine fresh component epochs, H/J inactive, A/gate closed, observation-only startup, fresh provider/authority binding, and explicit Prepare before motion. Issue no Disable, mode selection, Ready request, trajectory, or automatic resume during startup                                               | Initial enabled-idle reload passed; active-motion/broader restart matrix remains open                                                                                            |
| `robot.clear_fault`                                               | Invoke only the restricted error-clear primitive; retain state 4 and disabled masks                                                                       | The current primitive guard requires drives off/state 4 and closes A/gate on acceptance; verify error and vendor `recovery_required` clear; never enable, select mode, or request Ready. It currently has no per-error allowlist                                                                                                                         | Current contract and prior physical pass. Intended recovery first makes J/H inactive; duplicate H/J/stationary guard and vendor error-class eligibility remain open              |
| `robot.clear_warning`                                             | Invoke only the restricted warning-clear primitive; retain state 4 and disabled masks                                                                     | The current primitive guard requires drives off/state 4; verify warning independently and do not infer fault recovery                                                                                                                                                                                                                                    | Current contract; deterministic tests pass, natural physical warning row open. J/H/stationary guard hardening remains open                                                       |
| `robot.enable` / `motion_enable(true, id=8)`                      | Enable all six drives; it does not select mode or Ready state                                                                                             | A remains closed until pose, vendor control, H, and J are separately ready                                                                                                                                                                                                                                                                               | Documented; physically passed earlier. One startup-settling transient remains open/unbounded qualification debt                                                                  |
| UFactory Studio **Enable**                                        | May be a vendor-UI composite rather than raw drive enable; the attended row included the Lite6 startup settling and external state changes                | Observe and classify only; do not attribute each primitive or let external Enable itself establish ManyForge admission                                                                                                                                                                                                                                   | Physically observed; exact composite sequence/transient bound remains open                                                                                                       |
| `robot.disable` / `motion_enable(false, id=8)`                    | Standalone primitive requires observed state 4 and disables all drives                                                                                    | Public `robot.make_noncommandable` first closes A/gate and unwinds J/H; standalone Disable does not itself deactivate them                                                                                                                                                                                                                               | Documented and physically passed earlier; never a startup/restart prerequisite                                                                                                   |
| `set_mode(0)` or UFactory Studio Position/Joint Control           | Mode 0 is vendor position control. The 2026-07-27 Studio jog produced `1/2 → 0/1` while moving, then `0/5`                                                | On observation, close A/gate and latch explicit re-arm within the qualified freshness/lease bound; H/J may still be active but cannot retain command admission                                                                                                                                                                                           | Documented plus observed twice. The jog physically moved joint 6; concurrent Studio/ManyForge control is unsupported                                                             |
| `robot.select_external_control` / `set_mode(1)`                   | Report `M=1, S=5` (`MODE_CHANGED`)                                                                                                                        | Public Prepare schedules it only with H/J inactive and A/gate closed. The non-public standalone primitive lacks the duplicate H/J guard                                                                                                                                                                                                                  | Documented and observed; whether to tighten the standalone guard remains hardening work                                                                                          |
| `robot.start_control` / guarded `set_state(0)`                    | State 0 is a request; successful feedback becomes `S=2` (`READY`)                                                                                         | Requires fault/vendor-`recovery_required`/warning clear and state 4 or 5 first; does not activate H/J or dispatch a trajectory                                                                                                                                                                                                                           | Documented and observed. The raw vendor primitive's error-clear/Resume side effects are not allowed shortcuts                                                                    |
| UFactory Studio **STOP**                                          | Report `S=4`, stop immediately, clear Studio/vendor cached commands, and keep drives powered                                                              | Close A/gate. The 2026-07-27 row preserved `M=1`, masks `63/63`, and initially left H/J active                                                                                                                                                                                                                                                           | Cache clearing is documented for the Studio control and the attended transition was observed; Stop is not Disable                                                                |
| `robot.stop_control` / raw `set_state(4)`                         | Report `S=4`, terminate current execution, reject new commands until state 0, and keep drives powered                                                     | Close A/gate. Do not infer the Studio STOP button's documented cache-clear guarantee from the raw state primitive without separate evidence                                                                                                                                                                                                              | Raw termination/rejection is documented; cache equivalence remains open                                                                                                          |
| ManyForge FJT motion and natural completion                       | Expected mode 1 with state 1 feedback while moving and state 2 when vendor Ready                                                                          | Keep A/gate only while the execution pin, FJT goal, controller binding, observations, and all other gates remain valid. State 1 is accepted only as ManyForge ongoing-motion policy after admission; JTC success is not measured endpoint/rest proof                                                                                                     | First hash-retained bounded move/return milestone passes with measured endpoint and stationarity; planned-program, first-motion timing, Stop/fault/loss, and RT rows remain |
| Composer execution **Stop** / FJT cancel                          | No automatic `set_state(4)` or drive-off transition may be assumed; the controller may remain in mode 1 and state 1/2 while cancel/hold converges         | Fence new dispatch, cancel/stop the active execution once, and retain measured convergence; do not claim physical stop from action/process completion alone                                                                                                                                                                                              | ManyForge policy; reduced physical stop/dynamics row remains XC-8/RL-7 work                                                                                                      |
| External Studio/SDK program, jog, or motion                       | State 1 indicates motion, but mode and completion state depend on the external command                                                                    | A transition outside mode 1 with state 1/2 fences within the observation/lease bound. Mode-1/state-1 is also valid ManyForge motion, so mode/state alone cannot attribute a parallel ServoJ actor                                                                                                                                                        | Two mode-0 Studio jogs observed. Same-mode external concurrency is unsupported and remains an ownership/qualification gap                                                        |
| External critical configuration change                            | UFactory documents payload, TCP offset, collision sensitivity, and other critical changes as producing state 5                                            | Required policy is to fence A/gate and invalidate the execution/configuration assumption. Current Prepare can still normalize clean stationary state 5 without identifying its cause, so operators must not invoke it from an unexplained state 5                                                                                                        | Vendor-documented event; missing configuration fingerprint/cause allowlist is an implementation gap and release blocker                                                          |
| External modes 2, 4, 5, 6, 7, or any unknown/vendor-specific mode | Free-drive, joint/cartesian velocity, online planning, or another unsupported control path                                                                | Active use closes A/gate on observation. Current Prepare can generically normalize a stationary, enabled, fault-free non-ready mode through J/H off → state 4 → mode 1 → Ready → H/J on. Operational policy supports only qualified starts `1/4` and `0/2`; it is not yet enforced for every other start                                                 | Cause-aware enforcement and every other physical recovery row remain release blockers; do not use generic normalization as a supported shortcut                                  |
| Controller/JTC loss while idle                                    | Vendor `1/2` and masks `63/63` remain unchanged                                                                                                           | Close A/gate and latch re-arm. The attended row used explicit Prepare and left H active/J inactive before it. Current code clears the latch on any observed J off→on transition, not on transaction provenance                                                                                                                                           | Physical explicit-Prepare row passed; provenance-bound re-arm remains release hardening                                                                                          |
| Hardware/controller-manager binding loss or unknown activity      | No vendor mode/state/drive change may be assumed                                                                                                          | Fence A/gate, cancel a known goal through the still-qualified path, latch explicit re-arm, and preserve the exact loss source. Never equate software fencing with physical stop                                                                                                                                                                          | Deterministic/fake rows pass; broader physical idle/active loss matrix open                                                                                                      |
| Selected-authority expiry or replacement                          | No vendor mode/state/drive change may be assumed                                                                                                          | Close A/gate and latch re-arm. Same-session health renewal alone cannot reopen it; intended recovery is explicit Prepare. A separate external J off→on can currently satisfy the evidence latch and is not accepted policy                                                                                                                               | Durable-authority logic tested; target-provenance binding and active-motion physical row open                                                                                    |
| Routine command-gate/admission lease lapse                        | No vendor mode/state/drive change may be assumed                                                                                                          | Writes fail closed while stale. A fresh healthy provider refresh may renew the short lease without J cycling; latch re-arm only if a material supervisor/admission health loss is observed                                                                                                                                                               | Deterministic expiry behavior tested; deployed jitter/load evidence remains open                                                                                                 |
| Lifecycle-provider or cell-supervisor loss                        | No vendor mode/state/drive change may be assumed                                                                                                          | Fence/contain; terminal supervisor policy retires the process epoch and tears down Composer before owner. Never infer a physical stop from teardown                                                                                                                                                                                                      | Deterministic containment passes; remaining physical freeze/exit rows open                                                                                                       |
| Attended E-stop/safety-input event                                | The firmware-2.7 row specifically reported controller error C2, `M=0, S=4`, masks `0/0`, and the known small Lite6 settling                               | Observe and fence only; do not generalize that C2 row to every E-stop source, treat it as normal Disable, or auto-clear it                                                                                                                                                                                                                               | Specific observation plus vendor C2 classification; C1/C3 and the complete current-lane recovery matrix remain open                                                              |
| E-stop release or safety-input restoration                        | Removes only the physical safety cause; reported fault/state/masks must be observed afresh                                                                | Do not clear a fault, enable drives, select mode, reactivate H/J, open A/gate, or resume. Follow the error-class-specific recovery and explicit Prepare                                                                                                                                                                                                  | Industrial policy; complete current physical recovery row remains open                                                                                                           |
| Vendor error or physical collision                                | UFactory documents immediate stop/cache discard; mode/state and required recovery can depend on the error class                                           | Close A/gate, fail/cancel execution, surface the exact error, and require cause removal plus an explicit eligible recovery                                                                                                                                                                                                                               | Documented policy; natural collision/fault matrix remains open                                                                                                                   |
| Terminal report/control loss                                      | The last physical state becomes unavailable rather than assumed changed                                                                                   | An explicit SDK loss callback fences immediately; otherwise the provisional 1000 ms gate lease is the command fallback and the 2 s silent-loss timeout terminally fences the owner. Replace the `UFRobotSystemHardware` owner/controller-manager process and lifecycle-provider/transport binding; restarting Composer or reconnecting the socket is insufficient | Policy and physically passed; recovery requires a fresh observation-only process; XC-8R must separate execution-owner liveness from robot-observation freshness                                                                                 |
| Guarded `system_control(1)`                                       | Requests physical controller-system shutdown; it is not ROS teardown, Stop, or Disable                                                                    | Requires mode 1/state 4, H/J inactive, gate closed, drives off, stationary evidence, controller error code in `{0,1,2,3}`, separate confirmation, terminal owner fencing, and a fresh process afterward. Never retry an ambiguous attempt. Current generic Reconcile can falsely infer success merely because terminal fencing makes shutdown ineligible | Offline command path only; positive shutdown-result/reconciliation contract is an implementation blocker and no physical row is accepted                                         |
| State 3 pause or state 6 decelerated stop                         | State 3 pauses and raw state 0 can resume. UFactory sources differ on whether state 6 decelerated stop applies outside mode 0                             | Not exposed by the current ManyForge mode-1 profile. Fence A/gate and do not silently map either state to Composer Resume or Stop                                                                                                                                                                                                                        | Documented ambiguity; physical applicability and stop dynamics open                                                                                                              |

Closing ManyForge admission prevents only new ManyForge dispatch. It does not
cancel or physically stop a program started independently through Studio or
another SDK client. That work must first be terminated through its owning source
or an independent safety path; only then may fresh stationary/state-4 evidence
and source-specific command-termination/cache state enter a supported recovery
branch.

The one-owner architecture prevents a second ManyForge/xarm_ros2 command path;
it cannot prevent UFactory Studio or another direct network client from
controlling the robot. Until a vendor-level ownership token or equivalent
detection is qualified, operational exclusivity is mandatory. The displayed
command counter is trace context, not an admission predicate, and mode-1/state-1
or mode-1/state-2 alone cannot prove which external source is commanding.

The lifecycle history records material mode, state, masks, error, warning,
readiness, stationarity, and component-epoch changes. It labels an unclaimed
transition `external_or_unknown`; timing is correlation, not causal proof. Pose
and the displayed command counter are not lifecycle-history facets, so an
external same-mode/state ServoJ stream can create no lifecycle event at all.
That is a known traceability boundary, not evidence that no external command
occurred.

UFactory's
[Live Control manual](https://docs.ufactory.cc/user_manual/ufactoryStudio/4.live_control.html)
states that Studio STOP is a software stop which leaves power on. Its
[mode/state manual](https://docs.ufactory.cc/user_manual/ufactoryStudio/10.motion_mode_and_state.html)
defines modes 0, 1, 2, and 4–7 plus states 0–6. Its
[error-handling manual](https://docs.ufactory.cc/user_manual/ufactoryStudio/12.error_handling.html)
requires manual cause correction and error recovery; some errors require
re-powering rather than a generic clear. ManyForge must not collapse those
different vendor procedures into one blind Reset.

Initial supervised H activation accepts only fresh mode 1/state 2. It never
attaches to an arm already reporting state 1/moving. After H/J/A are active and
a trajectory is admitted, state 1 is expected during measured motion and state 2
is expected when ready/idle; this phase distinction is part of the gate.

### Qualified composite sequences

`robot.prepare_for_motion` is a target intent, not a fixed macro. It performs
only the missing adapter-owned stages:

| Qualified start                                           | Observed or intended sequence                                                                                                       | Final condition                                                                                                                                                                                |
| --------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Already fully admitted at the target                      | Preserve A/gate and return target-already-satisfied; perform zero primitive or H/J stage churn                                      | Existing healthy `1/2`, `63/63`, H/J/A remains unchanged                                                                                                                                       |
| Enabled `1/2`, H/J inactive                               | Activate H → activate J → open A                                                                                                    | `1/2`, `63/63`, H/J/A true; no vendor primitive                                                                                                                                                |
| J revoked while enabled `1/2`, H active                   | Keep A/gate closed → activate J → open A                                                                                            | No vendor primitive or drive change; the attended path used explicit Prepare, while provenance-bound re-arm remains open                                                                       |
| Studio STOP: enabled `1/4`, H/J active                    | Keep A/gate closed → J off → H off → `set_state(0)` → H on → J on → open A                                                          | `1/2`, `63/63`, H/J/A true                                                                                                                                                                     |
| Test-established observed mode `0`, state `2`, H/J active | Keep A/gate closed → J off → H off → `set_state(4)` (`0/4`) → `set_mode(1)` (`1/5`) → `set_state(0)` (`1/2`) → H on → J on → open A | Exact 2026-07-27 recovery pass from the restricted setup baseline: masks stayed `63/63`, no FJT/cycle execution, and the operator observed no movement; a natural external source remains open |
| Eligible disabled state 4 in mode 0 or 1                  | Enable drives → select mode 1 only if needed → request Ready → activate H → activate J → open A                                     | This cold path may perform only stages whose fresh observed preconditions are true; unsupported disabled combinations remain closed                                                            |

The exact mode-0/state-2 starting point was established by one attended,
restricted, non-public `SET_READY_STATE` test-setup call after Studio jogging
had ended in mode 0/state 5. That setup is not a Composer or PLC route and is
not part of the accepted recovery. During the subsequent normal Prepare re-arm,
the lifecycle-command counter advanced by three for Stop, mode selection, and
Ready. The joint-write counter remained unchanged while A/gate were closed and
through J activation. Once J and A were open, normal current-position hold-loop
writes resumed; those writes are not a FollowJointTrajectory dispatch. The
Kernel and cycle remained idle, the execution command count stayed zero, and the
operator observed no motion.

`robot.make_noncommandable` is the inverse target used by **Disable robot**:

1. the public Disable route first stops/cancels the ManyForge execution and
   retires its active execution pin;
1. accepting the lifecycle target closes A/gate before any stage can wait or
   mutate;
1. require fresh stationary evidence as the target precondition; reject rather
   than wait with an open gate;
1. deactivate J, then H;
1. on the normal enabled path, request vendor state 4 if it is not already
   stopped, then disable all drives; and
1. verify the declared terminal facts: H/J inactive, drives disabled, and vendor
   control not ready. The normal enabled path reports state 4 and masks `0/0`;
   state/stationarity can become unavailable after hardware teardown and are not
   fabricated.

For the current generated Lite6 profile, stationarity means no more than
`0.001 rad` maximum joint drift over a `1000 ms` dwell with the direct-poll and
rich-report sources agreeing. Those are qualification-profile values, not
universal xArm constants. An already disabled/non-ready arm can satisfy the
declared non-commandable target without forcing mode/state normalization.
Consequently, an externally disabled mode-0/state-4 arm is non-commandable but
not eligible for guarded controller shutdown, whose stricter guard requires mode
1/state 4 as well as H/J inactive, drives off, and stationary evidence. The
current shutdown guard also accepts only controller error codes `0`, `1`, `2`,
or `3`; every attempted shutdown terminally fences the owner and is never
retried after an ambiguous result.

It must not be substituted for startup, ordinary component restart, Studio
takeover recovery, or a browser disconnect.

### Intended reset and recovery sequence

Four controls that look similar in an HMI have different authorities:

| Operator term            | What it changes                                                                                                                                   | What it must not do                                                   |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| **Acknowledge**          | Alarm attention state only                                                                                                                        | Clear a fault, change the robot, reopen admission, or resume          |
| **Reset cycle**          | Clear the already-terminal reset-required cycle and establish a clean new execution context; the old execution pin was retired during containment | Clear a vendor fault, home, enable, change mode, or run               |
| **Enable robot**         | Submit one explicit `robot.prepare_for_motion` target                                                                                             | Dispatch a trajectory, clear a fault, or resume an old execution      |
| **Run / Retry / Replan** | Create or continue the explicitly selected new execution after all gates pass                                                                     | Implicitly acknowledge, clear, enable, or inherit a retired execution |

The intended industrial recovery order is:

1. **Fence first.** Close dispatch and the owner command gate, stop/cancel the
   active ManyForge execution once through a still-qualified path, terminalize
   it, and retire its environment pin. Later Reset clears that terminal cycle;
   it does not perform the safety retirement. Never infer a physical stop from
   process exit alone.
1. **Observe and classify.** Record fresh mode, state, masks, error/warning,
   pose/stationarity, H/J activity, component epochs, and the initiating source
   when known. Do not infer the cause of an external transition merely from
   timing.
1. **Remove the cause.** Clear the workspace obstruction, release the safety
   input, correct payload/configuration, restore transport, or follow the
   vendor's error-specific power-cycle procedure. The operating procedure must
   not send generic `clear_fault` for an error class whose vendor procedure
   requires E-stop release, relocation, power cycle, or support. The current
   catalog does not yet encode that per-error allowlist; this is
   release-hardening work.
1. **Make the robot non-commandable when required and safe.** The intended
   normal xArm clear-fault branch first reaches H/J inactive, state 4, drives
   disabled, and stationary evidence. The current standalone clear primitive
   itself enforces drives disabled and state 4 but does not yet duplicate the
   H/J/stationary guard; tightening that primitive remains an explicit hardening
   decision. If the surviving command path cannot establish the intended state
   safely, require operator/vendor recovery instead.
1. **Clear only the eligible condition.** Fault clear and warning clear are
   separate explicit transactions. Confirm the observed code becomes zero; a
   successful ROS return alone is insufficient.
1. **Re-establish a fresh process epoch after terminal ownership loss.** Startup
   remains observation-only. A reconnected old owner cannot reclaim its previous
   session or commands.
1. **Explicitly prepare when robot lifecycle/controller readiness was lost.**
   Submit one new `robot.prepare_for_motion` only when the recovery branch
   fenced lifecycle admission, latched re-arm, or left the robot/controller
   non-ready. It may reconcile mode/H/J without cycling drives where the
   adapter-qualified state permits, but it never clears a fault or sends a
   trajectory. Omit it for cycle-only/orchestration failures when the immutable
   lifecycle snapshot stayed admitted and healthy.
1. **Reset orchestration separately.** Acknowledge alarms as needed, Reset the
   retired cycle, then explicitly Run, Retry, or Replan. No trajectory or tree
   resumes automatically.

An accepted primitive whose vendor return is nonzero, whose response disappears
after dispatch, or whose authority/transaction deadline is lost after execution
becomes `outcome_unknown`. It is never blindly retried. Composer **Reconcile**
closes admission, refreshes observations, and compares only the original
action's required postcondition; Reconcile sends no vendor command. It resolves
the original transaction as success or failure. If that original transaction was
Prepare and its authorized target postcondition is proven, it is already the
explicit intent; no second Prepare is required. Otherwise submit Prepare only
when lifecycle readiness/re-arm remains unsatisfied. Current code can also
reopen after an unrelated observed J inactive→active transition clears the
evidence-only re-arm latch; this is the same provenance gap documented above and
blocks release.

Apply that order according to the observed class; do not force every event
through the same physical sequence:

| Observed class                                                                                        | Intended recovery branch                                                                                                                                                                                                                                                                                      |
| ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| JTC/controller loss only; robot still enabled `1/2` and fault-free                                    | Keep drives and vendor state unchanged; explicitly Prepare to reactivate only the missing ROS components; Reset/Run separately if an execution was retired                                                                                                                                                    |
| Qualified Studio Stop `1/4` or exact test-established observed `0/2`; enabled, stationary, fault-free | Fence; explicitly Prepare. The adapter may unwind J/H, normalize state 4 and mode 1/Ready, then restore H/J without cycling drives. A natural external source for exact `0/2` remains unqualified                                                                                                             |
| ManyForge runtime collision with no vendor fault                                                      | Retain the incident, remove/update the scene cause, Reset/Replan, then pass the full admission gate; omit vendor fault clear and drive cycling                                                                                                                                                                |
| Program, planning, validation, or orchestration error with lifecycle still healthy                    | Preserve the admitted robot lifecycle; acknowledge/Reset/Retry/Replan as applicable. Do not submit Prepare merely because the cycle failed                                                                                                                                                                    |
| Procedurally eligible vendor fault or warning                                                         | Remove the cause; use `robot.make_noncommandable` to reach J/H inactive, state 4, masks `0/0`; clear fault and warning as separate explicit actions and verify zero codes; then explicitly Prepare. Per-error eligibility is still an operator/vendor procedure until the catalog gains a qualified allowlist |
| E-stop or error class requiring restart/power cycle                                                   | Follow the vendor-specific physical procedure; do not call generic clear/Prepare until fresh observations prove eligibility. If ownership/transport was lost, start a fresh observation-only process                                                                                                          |
| Terminal owner/report/control loss                                                                    | Let the gate/session fence, retire the affected execution, replace the owner process, observe from a fresh epoch, and require explicit Prepare; never reconnect or replay the old session                                                                                                                     |
| Ambiguous lifecycle-command outcome                                                                   | Keep A/gate closed; never retry the primitive. Reconcile fresh observations against the original postcondition without sending a command. If the original authorized Prepare target is proven, no second Prepare is required; otherwise Prepare only when readiness/re-arm remains unsatisfied                |

Every other external mode, state-5 configuration change, or independently
running Studio/SDK program is unsupported and must be treated operationally
closed. Its owning source or safety path must stop it first. Current Prepare can
still find a generic normalization path from some of these states, so the
cause/fingerprint allowlist must be enforced in code before this boundary can be
called fail-closed or advertised as general external-state recovery.

The natural physical-collision matrix and collision-rebound settings remain open
qualification work. Do not advertise one universal collision-reset macro.

### 2026-07-27 minimum physical acceptance

The retained product run is
`20260727T161931Z-e3cf412a-0036-4aa2-a8ea-1291afce1d0a`. Raw evidence remains
ignored runtime state; these committed SHA-256 values make the conclusions
auditable:

| Row                                    | Evidence label                             | `evidence.json` SHA-256                                            |
| -------------------------------------- | ------------------------------------------ | ------------------------------------------------------------------ |
| Observation-only committed reload      | `startup-closed-7f7e35a7`                  | `ba70ac9d646b484f229eff284c4dfaad74adda5cd035ebf4e6e0c9c583c731d6` |
| Initial explicit admission             | `initial-admitted-7f7e35a7`                | `3e4abfed04633c67f2d074c34dc1d70f39c2b496b3b6bb524328a05c58271baa` |
| Admission retained beyond 60 s         | `admitted-after-60s-7f7e35a7`              | `3bd3dc8de0db435bed735038a6646ed9ff0c6320805084fe1002698e8dc4f513` |
| External JTC loss fenced               | `controller-loss-fenced-7f7e35a7`          | `b428d49a983d13a8f4fa31d12e3317beeb5a6c4572c68a49c6271598511465ef` |
| Explicit JTC re-arm admitted           | `controller-rearmed-admitted-7f7e35a7`     | `29b33c4fbf835cb0af252196f3a19de50159ae5ee461c30e92b6bd9e1ae0aabd` |
| Studio STOP fenced                     | `studio-stop-fenced-before-rearm-7f7e35a7` | `4a1d14e93e7b452e38b9de0dc23f2c9fbea9bdc97ec2496ddd58b3dd916e9a18` |
| Test-established exact `0/2` pre-rearm | `mode0-state2-pre-rearm-7f7e35a7`          | `523005288f911f5ee9cd2707d0cf08ed073c1529ad3592624d9e227e848dc9e1` |
| Exact non-drive re-arm admitted        | `mode0-state2-rearmed-admitted-7f7e35a7`   | `3af4c75e4e06be76fc61f2de9d74a9dac2508cccd8050de9a61eacb6b5c6b246` |

The runtime-record and resolved runtime-binding SHA-256 values are
`831e805e41943cfcc44edc4620f09bcb3929a48ccedb9f27ecd4129a8c96c3db` and
`c6a55ff6900669d2bc10cf6d6ffec9031233251b342243f5db689abfe458ca61`. The live
binding verified `/controller_manager`, `lite6_system`,
`uf_robot_hardware/UFRobotSystemHardware`, the active
`joint_trajectory_controller/JointTrajectoryController`, `joint1..joint6`, all
position/velocity claims, the exact FJT endpoint, and reliable transient-local
activity QoS. The graph exposed only the restricted supervised xArm mutation
services, not a parallel legacy command surface.

The same run recorded six isolated owner-progress unhealthy/healthy pulses of
45–51 ms. They changed no admission, gate, epoch, authority session, fault, or
robot state and remained below the 250 ms gate lease and 2 s critical-progress
budget. Their exact reason was not retained. This repeated observability gap
does not block bounded XC-8, but it remains architecture-review and
release-promotion debt; do not hide it by widening a timeout.

An attended worktree-overlay run on 2026-07-25 exercised the six applicable
ordinary primitives, controller activation/deactivation, and a 100 ms
current-pose command-gate lease. Its command-bearing rows remain historical
provisional evidence because their lifecycle events were not captured by the
automatic evidence writer.

A later attended command-evidence run on 2026-07-25 passed separate
`robot.clear_fault` and `robot.enable` rows with zero joint writes. The
subsequent `robot.select_external_control` request was attempted exactly once
and returned vendor code `0`; fresh feedback became mode `1`, state `5`, with
drives enabled and admission closed. The evidence tool correctly rejected the
row because the then-current catalog incorrectly expected state `4`. This is a
catalog/postcondition defect, not a failed vendor transition. The corrected
table above adopts the documented state-`5` result and requires the separate
state-0 request/state-2 feedback transition before hardware activation.

A clean, committed-source command-evidence session later on 2026-07-25 passed
all six applicable ordinary rows through the installed local-operator gateway
and restricted ROS owner: `robot.clear_fault`, `robot.enable`,
`robot.select_external_control`, `robot.start_control`, `robot.stop_control`,
and `robot.disable`. Each retained exactly one permitted and attempted
primitive, vendor return code `0`, the corrected observed postcondition, and
successful authority closure. The source identities were ManyForge `ee1a4242…`,
xarm_ros2 `20245766…`, and image
`sha256:c970ffc8f2ce47d5aca31da81962e57c97246e1554e7245b04e13a6156faf567`. The
runtime-binding generation was
`5bf3d06aabe9924855dfd52eac702efd5c1d928cc74542be2cd96801400efe52`.

The six ignored evidence directories, in order, are:

- `20260725T202313Z-robot-clear_fault`;
- `20260725T202403Z-robot-enable`;
- `20260725T202547Z-robot-select_external_control`;
- `20260725T202620Z-robot-start_control`;
- `20260725T204203Z-robot-stop_control`; and
- `20260725T204243Z-robot-disable`.

The corresponding `evidence.json` SHA-256 values are:

- `b26cc5df4d883b5e7d4b0ad19d0851f51e3a37f941544902e92707b123b67460`;
- `345c147e6a950cd626993a41432e0f0b098bab00b6913c6ec4ae16cd93c71477`;
- `0f6a044f24741337c3cb1463856c440c9d2beafda8333e556e865782ac974327`;
- `3b741bb53f97309c8b030acc857b8ea737ce9f2d421656ff0841e7ce39b68823`;
- `e3bf1a7d2de31da9d25c7f72470cce8cf0d8df95e087cb19ac8e8135450e2184`; and
- `dba088867913eddd7d975108fd1b82a6d0606d564bf778a8d8c52b4b213ebf47`.

The same session manually exercised hardware activation, JTC
load/activate/deactivate/re-arm, and a 100 ms current-pose gate lease. The lease
produced exactly 15 successful writes at 150 Hz, returned the gate to closed,
changed measured pose by at most `0.000191749 rad`, and produced no
operator-observed motion. These controller/gate observations remain provisional
until one maintained automatic capture binds the controller activity, gate
response/expiry, write counters, and before/after pose. `robot.clear_warning`
remains unclaimed because no natural warning was present and no safe
warning-generation procedure was approved.

The maintained automatic controller/gate transaction is now implemented and
offline-qualified. It requires an operator-supplied pose-deviation limit,
deactivates and reactivates only the generated binding's named JTC, publishes no
trajectory, opens the restricted owner gate for exactly 100 ms, and fails unless
it observes the inactive/active activity transitions, closed-before and
automatic-expiry gate states, positive successful write-counter progress, an
unchanged lifecycle-command counter, at least two joint samples, and a pose
deviation inside the explicit limit. A physical run is still required before the
provisional observation above can be promoted.

A later command-free acceptance repeated the physical Ethernet-loss path from
clean, committed sources. It used ManyForge `c3d87fa0…`, xarm_ros2 `47847afd…`,
and image
`sha256:0951bd9de65ec9f64b4fcda9ff8ab75748bb81fa4ff74028a97f6a8a3c9383fe`. The
250 ms admission lease closed first; terminal loss followed 2.031 s after the
last report. Twelve seconds after the cable was reconnected, the original
session remained fenced with frozen counters and
`process_restart_required=true`. That owner closed in 0.119 s. A fresh process
then established a distinct session and restored verified observation in 2.418 s
while hardware remained inactive and admission remained closed. It closed in
0.165 s. Every phase recorded zero joint writes and zero lifecycle command
attempts.

The ignored raw evidence is under
`.manyforge-var/real-lite6/live-evidence/20260725T181109Z-clean-supervised-acceptance-v2/`.
The baseline, terminal, same-session-refusal, and fresh-process evidence SHA-256
values are, respectively:

- `15cfe8c563fd5d0822e93419a1d0f9f0ed5e72d9cb69572bb385488e77cf905a`;
- `62cac1cec20f43c0e4b326e1ebf357aecbdd00fa297f75aca4927c31970c8adb`;
- `6b8f08f757ce89116194413bc05c9e0e65dfc7cef08bdbd5e97e5b31d9d11e9a`; and
- `94b8a2a1a120cca84e30dc191e73884c3f647042ed32819a619bdbd1e6141f61`.

A separate-container observer initially discovered the owner but received no
samples when both ad-hoc containers used private IPC namespaces. Repeating the
same command-free probe with `network_mode: host` and `ipc: host` delivered 42
samples and passed; its evidence SHA-256 is
`202a2575574f14e306155308631a9a1aa5bb01e83c7cd9d176a8a7d8b818abda`. The
canonical ROS Compose services already set both values. Any future real-Lite6
multi-container launcher must preserve that topology or provide and qualify an
explicit DDS transport profile that does not depend on shared memory.

Still required before full Stage C/release acceptance:

- qualify `plc_roxsie` independently through its real PLC/ROXSIE ROS path; that
  profile does not block bounded `local_operator` XC-8 work and cannot inherit
  its evidence;
- qualify receiver-local component stale detection, same-epoch supervisor loss
  with terminal teardown and no re-arm, fresh-epoch observation-only restart,
  and Reset-required cycle containment;
- prove an execution-environment change cannot cross planning/dispatch, inherit
  Resume, or turn a stale successful result into a committed success;
- complete the remaining owner/SDK failed-connect, in-flight-close,
  actual-fault, and release-soak rows without reintroducing the deleted
  direct-SDK candidate or a modified SDK;
- investigate the repeated 45–51 ms owner-progress pulses and retain their exact
  reason before release promotion; do not widen the existing budgets;
- run the dedicated current-position gate-expiry capture if that specific
  primitive-level claim is promoted beyond the already passed product
  controller-loss/re-arm row;
- complete the warning-clear row only against a naturally present warning or an
  independently approved safe procedure;
- qualify natural vendor collision/fault recovery and error classes that require
  E-stop or power cycling rather than a normal clear;
- bound and review the one observed enable/start activation transient instead of
  treating “normal-looking” motion as a zero-motion pass; and
- qualify guarded physical controller shutdown, prefixed/multi-instance
  bindings, deployed load/RT behavior, and authentication in their separately
  scoped release rows.

Stage C ManyForge commands cover only explicit enable/disable, mode/state
selection, fault recovery, controller activation/loss/restart, and authority
loss. They send no trajectory and have a zero-commanded-motion expectation. The
two 2026-07-27 Studio jogs were external, attended setup actions that physically
moved joint 6; they are not evidence that a ManyForge non-motion command moved
the robot. Stop immediately on any other unexpected motion or on any command
whose result cannot be reconciled. Normal Stop must be followed by the
separately requested Disable test when drive-off is intended, with fresh
observations proving both command admission closed and drive/brake masks zero.
If a separately addressed enable primitive produces a physical activation
transient, retain the recording and leave that row incomplete until the behavior
has an explicit reviewed bound; do not inherit acceptance from the vendor UI's
composite startup.

Credential authentication is intentionally deferred as a separate hardening
step. The experimental Composer ingress records a server-assigned source with
honest `unverified` assurance; it does not manufacture an authenticated
principal. Until authentication is implemented and qualified, use this only as a
local, attended commissioning surface on the controlled cell network. The
provider still enforces one authority session, generation fencing,
preconditions, postconditions, and audit independently of the browser.

### Stage C command-evidence procedure

The source tree must be clean and the real-Lite6 image must already be rebuilt
from the intended clean xarm_ros2 branch. With the cell clear, independent
E-stop available, and the E-stop initially pressed, start the inactive owner in
one terminal:

```bash
export MANYFORGE_LITE6_ROBOT_IP=192.168.1.31
export MANYFORGE_LITE6_SUPERVISED_CONFIRM=START_SUPERVISED_OWNER_INACTIVE
./scripts/xarm/launch-lite6-supervised-owner.sh
```

This qualification launcher enforces the experimentally qualified host
network/host IPC topology, resolves the address-bearing binding only under
ignored runtime state, and starts `lite6_system` inactive. It performs no
automatic clear, enable, mode, state, controller, or trajectory action.

For each separately approved row, use a second terminal and make the
confirmation exactly equal to the action:

```bash
export MANYFORGE_LITE6_ACTION=robot.stop_control
export MANYFORGE_LITE6_ACTION_CONFIRMATION=robot.stop_control
./scripts/xarm/capture-lite6-supervised-command.sh
```

The allowed actions are `robot.clear_fault`, `robot.clear_warning`,
`robot.enable`, `robot.select_external_control`, `robot.start_control`,
`robot.stop_control`, and `robot.disable`. Current state still controls
availability; listing an action never bypasses its catalog preconditions. The
wrapper sends exactly one action, writes one ignored evidence directory, and
returns nonzero on timeout, rejected/mismatched identity, missing/inexact
primitive audit, failed postcondition, or failed authority closure. Review the
captured result before authorizing another action. Do not batch these commands
or use this procedure to manufacture a warning.

For Enable/Start, retain the default three-second capture window or select a
reviewed longer window up to ten seconds. Set a pose limit only after that bound
has been explicitly accepted:

```bash
export MANYFORGE_LITE6_TRANSIENT_CAPTURE_MS=3000
# Optional only after review:
# export MANYFORGE_LITE6_TRANSIENT_POSE_LIMIT_RAD=0.001
```

Once the robot is enabled, in joint-servo mode/state, the hardware component and
named JTC are active, and the operator has approved the no-trajectory test,
capture the controller/gate row:

```bash
export MANYFORGE_LITE6_CONTROLLER_GATE_CONFIRMATION=\
QUALIFY_NO_TRAJECTORY_CONTROLLER_GATE
export MANYFORGE_LITE6_CONTROLLER_GATE_POSE_LIMIT_RAD=0.001
./scripts/xarm/capture-lite6-controller-gate.sh
```

The value above is an example input, not a frozen product limit. The wrapper
requires a positive explicit value and records it in provenance. It never sends
a FollowJointTrajectory goal. On any failure it attempts a final explicit gate
close and returns nonzero; a timeout never passes.

Stop the owner with `Ctrl-C` after the attended row. The launcher terminates
both controller manager and robot-state publisher. The product-shaped Composer
lane is now implemented below; credential authentication and the PLC/ROXSIE
ingress remain separate work.

### Stage C product-lane procedure

The unified lane is the product-shaped local-operator test. The older
one-command wrappers remain useful for isolated evidence rows.

Start with the E-stop pressed, a clean ManyForge worktree, and the robot address
checked for uniqueness. Expected outcome before running this command: one fresh
external-supervisor epoch, one observation-only owner, one non-owner Composer,
inactive hardware/JTC, closed command/admission gates, and zero lifecycle or
trajectory commands:

```bash
export MANYFORGE_LITE6_ROBOT_IP=192.168.1.31
./scripts/launch.sh --lane real-robot --assistant off \
  --scenario real-lite6-commissioning smoke
```

`smoke` starts the two-container plus host-supervisor topology and proves only
that the lifecycle provider and component graph are configured/fresh while
motion admission is closed. Before any lifecycle command, verify in Composer
that:

- the robot-state card identifies `lite6-a` and shows the observed blocking
  reason;
- the main **Run** button and Robot Control approval are disabled;
- hardware and trajectory-controller states are inactive;
- no trajectory, joint write, fault clear, drive enable, or mode/state command
  occurred during startup; and
- the runtime record exists under the printed ignored run directory.

Interpret three health domains independently during this gate.
`transportOwnerProgressHealthy` proves advancing, identity-valid xarm_ros2 owner
execution; `providerHealthy` proves advancing, identity-valid lifecycle provider
delivery inside Composer's receiver-local budget; and `fresh` proves the shorter
robot-observation deadline required for motion. During one bounded lifecycle
primitive, owner/provider progress may remain healthy while `fresh=false`;
motion must remain fenced and the UI may briefly report `waiting`, without
classifying a responsive owner as dead. The owner becomes unhealthy if its
explicit command deadline expires. Duplicate, regressed, or stalled
owner/provider delivery must not renew either progress domain.

Expected outcome before the following read-only capture: it records the
closed-admission baseline and all five component epochs without changing any
robot, controller, lifecycle, or execution state:

```bash
MANYFORGE_LITE6_CAPTURE_LABEL=startup-closed \
  ./scripts/xarm/capture-lite6-product-lane-state.sh
```

Then execute only the separately attended rows appropriate to the observed
state. State the expected result before each action:

1. **Release E-stop.** Expect only the robot's documented physical safety
   transition. ManyForge must remain non-commanding and admission must stay
   closed.

1. **Clear a real current fault/warning, if one exists.** Expect exactly the
   displayed recovery primitive and its observed postcondition. Never
   manufacture a warning merely to exercise the row.

1. **Exercise one deliberate external-enable interoperability row, when selected
   for the session.** Enable the stationary robot through UFactory Studio,
   without jogging or starting a vendor program. Expect Composer to remain
   healthy, observe the material state changes, and record them as
   external/unknown rather than inventing a cause. Admission may open only if
   every ordinary lifecycle, binding, authority, and freshness gate becomes
   true. Then request **Disable robot** through ManyForge. Expect the normal
   coordinated Stop-first `robot.make_noncommandable` transaction to accept an
   already externally enabled mode-0/state-2 robot and verify inactive
   controller/hardware plus disabled brake/servo masks from fresh observation.
   This row proves observation and coordinated relinquishment only; it does not
   authorize concurrent Studio jog/program execution.

1. **Enable robot.** Expect one `robot.prepare_for_motion` transaction. Wait for
   transport, pose, drives, vendor control, hardware, named JTC, and final
   motion admission to become fresh/true. The previously observed Lite6 startup
   transient remains a measured, provisional row: stop on any motion outside
   that attended behavior and do not infer acceptance by appearance.
   Transport-owner progress, lifecycle-provider delivery, authority health, and
   the external supervisor heartbeat must keep advancing during every bounded
   vendor/controller stage. Robot-observation freshness may pause only inside
   the displayed command deadline and must recover before admission opens. An
   owner/provider-health timeout, expired primitive deadline, or supervisor
   teardown makes the Enable outcome unknown: press E-stop, retain the trace,
   and never submit Enable again in that process epoch.

1. **Capture `enabled`.** Expect unchanged component epochs, one live authority
   session/generation, and open admission:

   ```bash
   MANYFORGE_LITE6_CAPTURE_LABEL=enabled \
     ./scripts/xarm/capture-lite6-product-lane-state.sh
   ```

1. **Leave the robot enabled and stationary for more than 60 seconds without
   browser interaction.** Expect the same authority/component epochs, continuing
   receiver-local progress, no lifecycle mutation, and admission to remain open.

1. **Run the commissioning program.** Expect only the 0.1 s timer to complete:
   no FollowJointTrajectory goal, joint write, or pose change.

1. **Disable robot.** Expect the cycle to full-stop first, followed by one
   `robot.make_noncommandable` transaction that unwinds JTC, hardware, vendor
   control, and drives. Fresh observations, not the ROS response alone, must
   prove the final state.

1. **Inspect Messages/History.** Expect one epoch baseline, only material
   lifecycle deltas, and one correlated terminal result per lifecycle
   transaction. Counter-only samples must not flood history. Correlation labels
   provide context but never prove physical cause. Alarm acknowledgement may
   clear attention only; it must not change robot state.

Capture after every row with a distinct label. A capture timeout or unavailable
required endpoint is evidence of an incomplete/failed observation, never a pass.

The former component-loss row is historical and **must not be executed from
this branch**. It read the third line of the old `active-run` record and sent
`SIGSTOP`/`SIGCONT` to the Python
`manyforge_supervision.cell_supervisor`. That semantic owner has been removed;
the exact procedure remains recoverable from the pinned source above.

At I10/R, restore the acceptance intent against the then-current Manager and
headless owners, not the old Python process contract. The approved fault
injection must prove that receiver-local heartbeat loss makes the component
graph and selected authority unhealthy within the qualified stale budgets,
closes lifecycle and final-dispatch admission, makes any active cycle
terminal/reset-required, and never auto-recovers robot state. It must retain
the replacement owner evidence, prove dependency-ordered teardown, and require
fresh epochs with no inherited session, execution, controller arm, or replay
state. Only after that same-epoch loss row passes may a separately approved
container/process-exit row qualify the equivalent detection path.

Repeat controller loss, provider loss, Disable, and recovery rows only under
their separately approved procedures. The ordinary main **Stop** is a
tree/trajectory stop, not drive Disable. Expected outcome before the following
launcher command: Composer is stopped before the owner, the external supervisor
records a normal terminal shutdown, no physical-stop claim is inferred from
process exit, and no lane process remains:

```bash
./scripts/launch.sh stop
```

Before any active-motion release claim, measure the generated candidate's 1 s
heartbeat, 2 s critical-progress age, and 5 s authority lease under the
qualified host load. Exercise a no-motion headless-authority loss first:
admission must close, the session must not reopen merely because health returns,
and a fresh explicit Enable must be required. Only then run one reduced
reversible active-motion loss row while recording the actual physical stop/hold
transient. FJT cancel or process exit alone is not proof of the robot transient.

### Guarded physical controller shutdown — offline only

Physical controller shutdown is a separate terminal action, not ROS component
teardown, ordinary Stop, or Disable. The ManyForge side and its dedicated ROS
service contract are offline-qualified. The matching xarm_ros2 implementation is
still a local experimental candidate, and no physical shutdown request has been
sent or accepted.

The action can become available only when fresh owner and controller-manager
observations prove the hardware component and named JTC inactive, command gate
closed, drives disabled, stopped state, joint-servo mode, initialized stationary
pose, an allowed shutdown error class, and no warning. It maps to exactly vendor
`system_control(1)`, requires the additional literal `SHUTDOWN_CONTROLLER`
confirmation, and treats accepted return as a terminal owner-session fence. Any
timeout after dispatch is ambiguous; it must never retry. Electrical power state
is an independent attended observation, and any restart requires a fresh
xarm_ros2 process.

The current generic postcondition is insufficient for an ambiguous shutdown:
every attempted call terminally fences the owner, which makes the action
ineligible and can let generic Reconcile misclassify that fact as success
without proving vendor acceptance or electrical shutdown. Until shutdown has a
positive, shutdown-specific terminal result and is excluded from this false
success path, do not offer Reconcile for it and do not run or accept the
physical row.

Do not run this row until its attended live procedure and postconditions are
reviewed separately. It is not required to continue offline XC-7B work.

## Stage D — bounded physical motion

Physical trajectory execution is the XC-8 gate. The exact initial unprefixed
`local_operator` profile now has a first hash-retained bounded milestone:
axis-6 and joint-5/joint-6 `+0.5°/-0.5°` move/return rows completed from fresh
`observed_ros2` state without a drive cycle, fault, admission loss, or
commanded delta above `0.5°`. The fully monitored two-joint rows retained
continuous measured authority, endpoint, and stationary convergence. This
permits the next attended XC-8 rows; it does not close planned-program,
Stop/fault/loss, first-motion timing, maintained-evidence-tool, or real-time
qualification. Prepare a new preview without dispatch by:

```bash
python3 scripts/xarm/prepare_lite6_commissioning_candidate.py \
  --joint joint6 --delta-deg 0.5
```

The command refuses unless Composer reports the `real_robot` profile, the cycle
is idle, lifecycle and runtime admission are open, all five physical readiness
facts are true, the exact `joint1..joint6` state comes from fresh
`observed_ros2` feedback, and source age is at most 250 ms. It accepts no delta
larger than 1 degree and never calls the candidate execute endpoint. It retains
the lifecycle/runtime/cycle/robot snapshots, exact request, and planned
candidate under `.manyforge-var/real-lite6/candidate-previews/`. The resolved
deployment imposes absolute per-joint caps of 0.1 rad/s and 0.2 rad/s² plus a
0.05 m/s TCP translation cap; the runtime velocity slider may reduce but cannot
raise those limits.

The operator then inspects the ghost and preview trajectory and explicitly
approves in Composer. Approval is a separate command and rechecks alarms,
state/binding freshness, lifecycle admission, validation, and final dispatch
authority. Do not approve if the preview leaves the reviewed workspace or if any
displayed fact changes.

### 2026-07-27 bounded-motion checkpoint

The retained product run is
`20260727T161931Z-e3cf412a-0036-4aa2-a8ea-1291afce1d0a`, using motion
implementation `7f7e35a7`, xarm_ros2 `c3172e57`, and image
`sha256:1ce135e699185584d0a6184bfc05499238620fce37c994cd1a6ad50afd2cef19`.
Raw evidence is ignored runtime state; the following committed hashes bind the
checkpoint:

| Row | Evidence label | `evidence.json` SHA-256 |
| --- | --- | --- |
| Pre-motion baseline | `xc8-pre-motion-baseline` | `4812728a8b8f1db0b82a501d0a16fdf99daf5690360f45d7b8e16f87df49df3a` |
| Explicit admission | `xc8-admitted-before-axis6` | `61be817bd48ef070e962eabdc6a75686ed17d4501d33dca08298f3ed2cafa4b9` |
| Axis-6 outbound postcondition | `xc8-axis6-plus0p5-post` | `1756810ea4bce1c990d340c9c68e2fdef62d287d9052bd72ec0504b070040d32` |
| Axis-6 monitored return | `xc8-axis6-minus0p5-monitored` | `7ff35a9a1dca13e3252e27763127cd67a170aba87b4ed81f86d73ac123a1617c` |
| Joints 5/6 monitored outbound | `xc8-joint5-joint6-plus0p5-monitored` | `55fc2593e1a033f4797135312d2b8b36b942a2be29449e5c44a27dfb5ede57a6` |
| Joints 5/6 monitored return | `xc8-joint5-joint6-minus0p5-monitored` | `65f8aad34743c4e2dd850dc902553e4b08ea8d8dc85d0f721f073bc5945596ea` |
| Final settled state | `xc8-after-bounded-motion-settled` | `47e1cd68b8a61dc77eb0cd8e391fc20d527cb8f852fb9c8eb11372d2aaee1b20` |
| Plan-only `+0.25°` refusal | `xc8-two-joint-plus0p25-plan-only` | `5ae2ae1ed5bda6f1c85381cec54855e632422ba2b13de48c8afc7f9dc5be81c7` |

The monitored two-joint rows passed all capture checks. Native execution took
`1.007016 s` outbound and `1.008000 s` on return for a `0.975054 s` preview;
maximum measured source age was `140.469 ms`, endpoint error stayed below
`0.000286 rad`, and the largest sampled excursion was `0.567627°`. Final
settled drift from the initial baseline was at most `0.021973°`.

Three findings remain explicit:

- the first axis-6 dispatch took `2.480506 s` natively for the same
  `0.975054 s` preview, while its return took `1.004513 s`; explain the
  first-motion component before claiming repeatable wall-time parity;
- a `+0.25°` joint-5/joint-6 path failed closed before dispatch because
  conservative retiming could not prove the `0.2 rad/s²` acceleration cap,
  while `+0.5°` planned and executed; diagnose the small-path retimer without
  weakening limits; and
- the one-off axis-6 return monitor classified normal kernel `planning`
  provenance and delayed stationary convergence too strictly. Its immutable
  artifact remains `success=false`; the trace and later settled capture retain
  the physical result. Move the corrected semantics into a maintained tool and
  repeat before release.

The minimum standalone `local_operator` Stage C gate is now accepted for the
exact initial unprefixed profile. Each remaining Stage D claim still requires:

- validated live pose and exact joint order;
- hard limits, velocity/acceleration limits, and workspace envelope;
- a small, pre-reviewed trajectory with conservative scaling;
- controller/admission loss tests;
- independent stop timing; and
- complete command and postcondition evidence.

Do not treat fake-hardware acceptance, read-only connectivity, or a successful
non-motion lifecycle primitive as physical-motion approval.

## Acceptance record

For each live session record:

- source commits/trees and image digest;
- binding and controller-binding generation IDs;
- configured network endpoint and reported robot-family characteristics;
- operator, date/time, cell, and authority profile;
- each stage result and evidence directory;
- every deviation and disposition; and
- the exact highest stage accepted.

If a required probe, command result, postcondition, or reconciliation is
missing, the stage is incomplete rather than passed.
