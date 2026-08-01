# ManyForge xArm control

> **Current `dev` boundary:** this retained adapter code is not a runnable
> xArm control product. The provider/launcher wiring and all physical
> qualification remain unavailable until I10/R. Present-tense protocol detail
> below records the intended/captured boundary and must not be read as proof
> that a process or command endpoint is live.

`manyforge_xarm_control` maps the generic robot-lifecycle contract onto xArm
semantics. It does not own, include, link, or instantiate the xArm SDK.

The reusable I7/I10 seed is vendor observation and result normalization,
declarative xArm action/postcondition mapping, bounded primitive audit, the
no-SDK wrapper boundary, and fake-backend/plugin evidence. The old
caller-session coordinator, singleton identities, execution-admission
projection, controller-manager sequencing, and renewable command-gate service
are migration inputs, not the target architecture. Generic lifecycle policy
moves to I7; the renewable gate can be removed only when I10 replacement
evidence exists.

The three installed native names (`lite6_supervised_preflight`,
`lite6_lifecycle_command_evidence`, and
`lite6_execution_admission_provider`) are currently stable unavailable gates.
They print `composable_cell_runtime_unavailable` and exit 78 before argument
parsing or ROS initialization, and they do not link the retained runtime
libraries.

## Captured production boundary

At the last commissioning baseline, one `xarm_ros2`
`UFRobotSystemHardware` instance owned the only SDK 1.18.1
session and the real Joint Trajectory Controller data path:

```text
JTC update loop
  -> xarm_ros2 hardware plugin
  -> private SDK session
  -> Lite6

ManyForge lifecycle coordinator (NRT state/status callbacks)
  -> capacity-one serialized lifecycle worker
  -> restricted xarm_msgs ROS services
  -> same xarm_ros2 hardware-plugin process

ManyForge command-gate projection (NRT, fail closed)
  -> independent coalescing gate-lease worker
  -> restricted xarm_msgs command-gate service

xarm_ros2 supervised state topic
  -> ManyForge ROS client (NRT)
  -> generic lifecycle/admission projector
  -> reliable transient-local admission topic
  -> strict generic execution ingress
```

The former installed `lite6_execution_admission_provider` owned one
`XArmLifecycleRosPort`, consumed the exact supervised state and
controller-manager activity endpoints from the resolved runtime manifest, and
published `manyforge_robot_lifecycle_msgs/ExecutionAdmissionSnapshot` every
50 ms with a maximum 250 ms lease. Its process identity, robot identity, and
controller-binding generation are exact-match fences. The generic
`manyforge_ros` ingress preserves the publisher's source deadline, rejects
duplicates, replacement, regression, and hidden semantic changes, and hands
only the validated robot-neutral snapshot to the native execution backend.

Provider startup remains observation-only: constructing or starting the
provider sends no lifecycle primitive and cannot admit motion. The provider
does expose the restricted generic session and lifecycle services used by the
selected headless local-operator adapter (and later by the mutually exclusive
PLC/ROXSIE adapter). A valid authority session, an explicit accepted target
transaction, fresh observed postconditions, active controller binding, and a
positively applied command-gate lease are all required before admission can
open.

The ordinary ROS surface exposes seven named operations: clear error, clear
warning, enable drives, disable drives, select joint-servo mode, set ready,
and set stopped. It exposes no raw SDK mode, state, servo ID, or
system-control argument.

A separately addressed, high-consequence `shutdown_controller` transaction is
also implemented for qualification. It is not part of the ordinary lifecycle
primitive catalog and never accepts a caller-provided system-control value:
the xarm_ros2 owner maps the dedicated service to exactly
`system_control(1)`. ManyForge admits that request only after fresh owner and
controller-manager observations prove the command gate closed, hardware and
JTC inactive, drives disabled, state stopped, mode joint-servo, initialized
stationary pose, permitted controller error class, and no warning. It requires
two exact confirmations, one attempt, and a terminal
`process_restart_required` result; the old owner session can never recover or
re-arm.

Every request carries a bounded, globally unique request ID and expected
observation generation. Each restricted primitive derives its transport ID
from that stable outer request ID plus its plan-step index, so a fresh
ManyForge client cannot alias an earlier transaction merely because its local
generation counter restarted. The xarm owner rejects stale generations and
replays exact duplicates without executing twice. Its command gate accepts a
bounded relative lease and is closed by default. A source process/session
replacement is terminal to the current ManyForge ROS client; a fresh
supervisory client and authority generation are required.

The NRT coordinator retains a bounded xArm-specific primitive audit alongside
the robot-neutral lifecycle transaction. Each record binds the transaction and
action to its step index, transport request ID, expected observation
generation, exact restricted primitive, permission/attempt flags, vendor
return code/reason, and returned generation. Rejected supervisor requests and
idempotent replays do not create a new primitive record.

All ROS waits, transaction reduction, controller activity handling, and
operator/PLC authority work are NRT. Lifecycle primitives execute exactly once
on one capacity-one worker; their immutable command/request/generation identity
is fenced back into the originating transaction. Late, duplicate, replaced, or
post-deadline completions are retained for audit but cannot mutate another
transaction. Command-gate lease updates use a separate coalescing worker so a
slow lifecycle primitive cannot delay a close request. Status publication,
authority-session services, and lifecycle mutations use explicit callback
groups, and Composer status reads use a short evidence lock that is independent
of session ROS waits. Provider health therefore remains observable throughout
bounded vendor/controller calls.

The xarm hardware `read()`/`write()` loop uses only the owner's local SDK
session and atomic/cached gate state. It never waits on a ROS service or enters
the ManyForge reducer. A pending or rejected asynchronous gate update keeps
coordinator admission closed; an open result is usable only through its
confirmed bounded effective deadline.

If NRT authority revocation closes the transport gate concurrently with one
update-loop write, the refused write is treated as an expected fail-closed
outcome rather than a hardware fault. A refusal while admission is still
current remains an error. This prevents ordinary revocation from causing
`controller_manager` to deactivate the hardware component while preserving
transport-failure detection.

The xArm state request and feedback values are not symmetric. The restricted
**Set ready** primitive sends `set_state(0)`, which requests standby; it does
not treat observed state `0` as ready. UFACTORY documents feedback state `2`
as `READY` and state `1` as `MOVING`. The command gate may remain open only in
those two feedback states. Initial hardware activation is stricter and
requires observed state `2`, so it cannot activate while the robot is already
moving or still transitioning.

Selecting joint-servo mode with `set_mode(1)` deliberately produces feedback
state `5` (`MODE_CHANGED`). ManyForge verifies that interlocked postcondition,
then **Start control** sends `set_state(0)` and waits for feedback state `2`.
States `0`, `3`, `4`, `5`, and `6`, a non-servo mode, partial drives, a fault,
or stale/invalid observation close the command gate. These semantics follow
the official [robot state and mode
explanation](https://docs.supportarticle.ufactory.cc/support_articles/developer/robot-state-and-mode-explanation.html)
and the upstream [ROS mode-change
sequence](https://github.com/xArm-Developer/xarm_ros#6-mode-change).

At that baseline, the guarded shutdown path was offline-qualified only. Its
matching xarm_ros2 service was an experimental local candidate and the
physical action had not been invoked or accepted. Do not expose it in Composer
or a PLC lane until
the attended preconditions, electrical postcondition, ambiguous-result
handling, and fresh-process restart procedure pass their dedicated live gate.
ROS/component teardown and physical controller shutdown remain different
operations.

## Fake/HIL boundary

`manyforge_xarm_control/ManyForgeXArmSystemHardware` is test infrastructure.
It accepts only:

- `backend=fake`;
- `fake_backend_confirmation=ALLOW_TEST_ONLY_FAKE_BACKEND`; and
- the complete fake identity, authority, controller, timing, and joint
  binding.

It rejects `backend=real`. Production generated URDF instead names
`uf_robot_hardware/UFRobotSystemHardware`; therefore the ManyForge fake plugin
cannot become a physical transport through configuration drift.

The fake lane exercises the generic supervisor, xArm transition coordinator,
admission lease, controller re-arm fence, real Jazzy `controller_manager`,
and real `joint_trajectory_controller`. It does not validate vendor transport.

## Historical verification and acceptance coverage

The following recipe records the last qualification shape for later I7/I10
porting. It is not a current runnable product recipe; the installed native
entrypoints and their shell wrappers now stop at the typed relocation gate.

```bash
export MANYFORGE_XC7A_IMAGE=manyforge-ros-jazzy-real-lite6:xc7a-local
MANYFORGE_REAL_LITE6_IMAGE="$MANYFORGE_XC7A_IMAGE" \
MANYFORGE_XARM_SOURCE_CONTEXT=/path/to/xarm_ros2 \
  ./scripts/xarm/build-real-lite6-image.sh

docker run --rm --network none \
  -v "$PWD:/workspace:ro" -w /workspace \
  "$MANYFORGE_XC7A_IMAGE" \
  ./scripts/ros/test-real-lite6-runtime.sh
```

The offline gate covers:

- xarm owner command mapping, replay, generation, and request validation;
- ManyForge state mapping, stale-state and owner-session replacement fencing;
- former installed provider publication, source-expiry closure, and proof that
  the observation-only provider issued no lifecycle or controller-shutdown call;
- strict ROS admission transport, generation/identity fencing, and native
  execution handoff;
- the seven ordinary operations, separate guarded shutdown transaction, and
  one-operation-per-plan rule;
- the complete deferred five-stage Enable target, exact completion fencing,
  one-dispatch timeout semantics, stale/duplicate completion rejection, and
  bounded worker shutdown;
- lifecycle status, refresh, and authority-session progress during deliberately
  slow lifecycle and command-gate ROS responses;
- fake/HIL controller-manager and JTC behavior;
- exact production/fake URDF ownership and endpoint generation;
- a static prohibition on direct `xarm_api` or SDK-session dependencies in
  `manyforge_xarm_control`; and
- package lint plus shell validation.

The historical stress and sanitizer invocations were:

```bash
./scripts/ros/test-xarm-controller-manager-stress.sh
./scripts/ros/test-xarm-lifecycle-sanitizers.sh
./scripts/ros/test-xarm-lifecycle-thread-sanitizer.sh
```

At the captured baseline, the installed
`lite6_lifecycle_command_evidence` executable was an attended, headless
qualification client. It opened a short local-operator session through
`LocalOperatorGateway`, verified the selected capability was available,
submitted exactly one explicitly confirmed action through the restricted ROS
owner, waited for the catalog postcondition, closed authority, and emitted one
JSON document containing the gateway and primitive audits. Timeout, identity
drift, an inexact audit, a failed postcondition, or failed authority closure
made the capture fail. It was covered as a separate ROS process against a fake
restricted owner.

In that baseline, one physical lifecycle request had a four-second total
discovery-plus-response budget inside the catalog's five-second transaction
budget. The transport waited for the original in-flight request and never
retried it. A timeout before dispatch was a known failure; a timeout or
transport exception after dispatch was retained as
`outcome_unknown_after_dispatch`, closed admission, and required explicit
state reconciliation. This left approximately one second for fresh
postcondition reports under the initial Lite6 timing profile.

The former ROS-only preflight observed an already-running supervised xarm
owner and did not contact the robot independently. Its historical invocation
was:

```bash
MANYFORGE_SKIP_REAL_LITE6_BUILD=1 \
  ./scripts/ros/run-lite6-supervised-preflight.sh
```

It accepted owner and namespace binding through
`MANYFORGE_LITE6_TRANSPORT_OWNER_ID`,
`MANYFORGE_LITE6_ROS_NAMESPACE`, and
`MANYFORGE_LITE6_HARDWARE_NAMESPACE`.

The former attended executable created an in-process authenticated-principal
assertion only after the operator had supplied the exact action confirmation
to the local shell wrapper. It was qualification tooling, not credential
authentication or a public ingress. Authentication and a Composer mutation
route remained later hardening work; no browser, HTTP, or public ROS
lifecycle-command ingress was added by that slice.
