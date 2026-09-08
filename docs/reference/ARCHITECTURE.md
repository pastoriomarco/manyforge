# ManyForge Architecture

> **Current `dev` status:** the composable-cell cutover has one maintained
> ManyForge-only native product through explicit Manager, CCLC, Headless,
> Program-runner, and native execution owners. I7/L0 supplies declarative
> lifecycle catalogs, deterministic lifecycle/recovery, complete-domain
> controller-route transactions, verified readiness, and thin confirmed
> clients. I8 supplies addressed multi-participant Prepare/Dispatch/Resume,
> asynchronous coordinated controls, scoped Stop and Full Stop, collision
> consequence routing, immutable certificates/fault latches, durable
> surveillance/recovery, and command-free orphan reconciliation. The governance
> contracts are ROS-free; the current product realization is native. Packaged
> aggregate launch, external-agent provider loops, Isaac/ROS product launch,
> real hardware, and generalized tools remain later
> I9-I11 work. Sections describing those three-lane transports remain target or
> historical reference unless they explicitly name the current native owner.
> Composer's optional MCP adapter maps tools to the same owner routes as
> other clients; it has no independent command or lifecycle authority. The
> current viewer receives credited, current-only native observations.

A coherent, mid-depth map of how ManyForge is put together: the components, how
a program flows through them, and — most importantly — which layer owns which
decision. For install and operation see
[`../INDEX.md`](../INDEX.md); for exact wire contracts see the generated
references linked at the end.

This document records the baseline's **observable runtime behavior and
structure** together with explicitly identified target foundations. It is not
a tutorial and not a spec; it is the picture to recover before rebuilding any
single component in depth.

______________________________________________________________________

## Design principles

Six ideas explain most of the decisions below:

- **One owner per execution-critical decision.** Terminal outcome, stop behavior,
  and collision validation each have exactly one authoritative owner; other
  layers observe, they do not override.
- **Transport-independent governance core.** The execution kernel is ROS-free,
  so execution semantics are identical whether the robot is simulated or
  physical — the transport is swapped underneath it, not woven through it.
- **Explicit state, no implicit transitions.** Execution moves through a
  guarded state machine; nothing outside the kernel mutates execution state.
- **Contract-driven modularity.** Planners and robot transports sit behind
  stable interfaces, so a backend can be swapped without touching the kernel,
  the programs, or the public API.
- **Reconstructable execution.** Every event carries correlation identity, so a
  failure can be reconstructed after the fact rather than reproduced live.
- **Bounded observer planes.** Visualization and diagnostics consume immutable
  facts through demand-scoped, latest-only channels. An absent, slow, or failed
  observer cannot create work without demand, build an unbounded backlog, or
  acquire execution authority.

______________________________________________________________________

## Components

| Component                                 | Language         | Responsibility                                                                                                                                                                                                              |
| ----------------------------------------- | ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `manyforge_core`                          | C++20            | Execution kernel: lifecycle state machine, terminal authority, stop/pause/reset semantics, collision-validation policy, runtime collision monitoring, execution snapshots and events. **ROS-free.**                         |
| `manyforge_deployment`                    | C++20            | ROS-free, non-real-time composable-cell artifact decoding, qualified schema normalization, staged graph content identity, and compiler substrate. It is not a Kernel dependency.                                            |
| `manyforge_component_manager`             | C++20            | Durable Apply, active retained-graph reconciliation, runtime-unit convergence, and explicit shutdown. The I5 daemon is its sole process owner and publishes immutable snapshots.                                            |
| `manyforge_headless`                      | C++20 + pybind11 | I5/H0 runtime/scene source aggregation, scene-initialization lifetime, Alarm Lifecycle Owner, asynchronous historian, verified NodeEnvironment projection, and Manager/scene publication adapters.                          |
| `manyforge_headless_api`                  | Python/FastAPI   | Bounded REST/WebSocket projection and one transport-neutral, two-class execution-control ingress over the owners. It owns transport freshness and delivery, not runtime, Program, scene, alarm, or robot semantics.          |
| `manyforge_program_runner`                | Python/FastAPI   | Independently restartable durable Program-cycle owner: exact Applied capture, tree construction/ticking, backend-owned Resetting/allowed commands, current-state idempotence, and explicit Program Stop/Reset orchestration.   |
| `manyforge_core/python`                   | pybind11         | Bindings exposing the kernel to Python (poll-based; no C++→Python callbacks).                                                                                                                                               |
| `manyforge_collision`                     | C++20            | Planner-neutral collision-world/query implementation behind ManyForge collision ports.                                                                                                                                      |
| `manyforge_roboplan`                      | C++ + pybind11   | Primary planning/projection adapter (RRT + TOPP-RA), native execution, and native kernel composition.                                                                                                                       |
| `manyforge_ros2_control_execution`        | C++20/ROS 2      | Controller execution adapter: FollowJointTrajectory, feedback/settling, bounded stop/reset, isolated ROS scene projection, and optional exact controller/lifecycle dispatch admission.                                      |
| `manyforge_behavior`                      | Python           | Node catalog/environment plus Program validation, tree, admission, scheduling, and focused client mechanisms hosted by the independent runner; it does not own robot effects.                                               |
| `manyforge_composer`                      | FastAPI + React  | Structural/operator shell, revision-fenced Program authoring, transparent control/viewer proxies, and backend-state presentation. It owns no execution lifecycle and performs no Program/control polling.                    |
| `manyforge_ros` / `manyforge_ros_bringup` | C++/Python       | ROS utility and deployment wiring: simulator inspection, standard controller/gripper surfaces, readiness supervision, and real/sim bring-up.                                                                                |
| `manyforge_assistant_bridge`              | Python           | Bounded direct/`nemoclaw` model mediation, strict active-mode enforcement, Draft/read callback routing, cancellation/reconciliation, conversation history, and redacted audit. It owns no runtime effects or catalog truth. |

### Layers

The same components stack into four layers. Control requests flow downward and
immutable facts flow upward. At the code boundary, adapters depend on core
ports—not the reverse—so higher layers cannot reach in and change how the
kernel decides.

```
┌─────────────────────────────────────────────┐
│  Operator / authoring     Composer (REST/WS/MCP), UI, LLM tools
├─────────────────────────────────────────────┤
│  Orchestration            Behavior runner: program, node catalog, cycle
├─────────────────────────────────────────────┤
│  Governance core (ROS-free) Execution lifecycle, terminal + stop authority,
│                            collision policy, events/snapshots
├─────────────────────────────────────────────┤
│  Backend                  RoboPlan (native) / ros2_control / custom
└─────────────────────────────────────────────┘
              │ calls ▼        facts ▲
```

______________________________________________________________________

## Data flow

```
Program YAML ─► Behavior Runner ─► ExecutionKernel ─┬─► planner/projection adapter
                    │                   │           ├─► ManyForge collision query
             node catalog +        terminal/stop   └─► execution adapter ─► plant
             recovery offer        authority
                    │                   │
                    └──── immutable events, state, scene and offer facts ────┐
                                                                             ▼
                                                        Composer REST/WS/MCP + UI
```

A program (behavior tree authored as YAML) is loaded by the **behavior runner**,
which ticks the tree and, for motion nodes, issues execute requests to the
**execution kernel**. The kernel requests a candidate from the selected planner,
validates it through ManyForge-owned validation/collision authorities, and only
then dispatches through the selected execution adapter. The same collision
authority monitors execution. **Composer** observes immutable outputs and
presents them over REST, WebSocket, and MCP; it does not certify or advance the
execution.

### Scene authority and initialization

Program resources and accepted runtime mutations enter one logical scene
authority. Planner-native and collision-engine scenes are revision-bound
projections; neither is a second logical source of truth. Observed object poses
are separate measured facts and change the authoritative scene only through a
configured pose-source policy.

A program/deployment cutover is an explicit initialization phase:

```text
close generation barrier -> reset backend -> seed authored scene
        |                                      |
        +-- retain newest eligible pose per object/source --+
                                                           v
                                      replay current batch -> open barrier
```

Only the matching generation/token may complete the phase. Stale completion is
ignored; a failed candidate restores the retained runtime's barrier state.
Expected waiting while the barrier is closed is status/history, not an alarm.
A definitive initialization failure, or a pose-apply failure after readiness,
is a typed condition with an authoritative clear path. See
[`SCENE_AUTHORITY.md`](SCENE_AUTHORITY.md).

### Scene Viewer data plane

The Scene Viewer is a **non-real-time observer**. It does not read planner or
collision-engine internals directly, advance execution or gripper lifecycles,
mutate the scene, certify validation, or decide stop/recovery outcomes. It
consumes immutable facts already published by their authoritative owners.

```text
Native current observation -> Headless credited socket -> Composer latest slot
                                                               |
                                                       /ws/scene/viewer
                                                               |
                                                    browser stable scene graph
                                                               |
                                                   applied-frame credit back
```

The current native protocol is separate from the historical manifest/frame
worker described in the old viewer schema. `GET /api/source-model` supplies
the retained model/topology on demand. `/ws/scene/viewer` supplies addressed
current joint observations; scene facts and other cold operator state arrive
through `/ws/control`. Neither heartbeat passage nor volatile sample counters
cause unchanged geometry to be rebuilt.

Cold source-file invalidation uses the existing native canonical source reader,
including integrity checks, outside the API event loop and Python GIL. It reads
changed files only and retains small equality hints and exact inode timestamps;
expiry does not parse source files. Corrupt records wake an authoritative
refresh instead of masquerading as healthy pulses. Directory replacement
re-registers the same watcher, and last-client teardown waits for its one
in-flight read. These hints neither establish runtime truth nor grant control
authority. Heartbeat traffic remains independent.

Each browser grants one credit, then confirms the exact frame it applied.
Until then, later observations replace the pending current value; they cannot
form a motion playback queue. A reconnect recaptures current state, never
replays old motion. There are at most 16 observation clients, and an abandoned
credit session expires after 45 seconds. With no credit demand, the upstream
source sleeps; with no clients it closes. There is no periodic HTTP sampler.

The browser applies the newest joint frame to stable model references during
its animation tick. The interpolated display is never execution, collision,
authoring or attachment authority. Stream diagnostics are read on demand at
`GET /api/scene/viewer/status`. Rendering/transport failures surface separately
from owner state; process/resource isolation and response-time claims still
require qualification on the actual platform and workload.

Renderer capability changes presentation policy, never data authority. Every
WebGL path uses the same bounded observer lease, and a software fallback must
still present complete opaque frames at a readable pixel-density floor; it may
not trade frame correctness for lower CPU. Hardware-GPU and software-renderer
measurements remain separate qualification rows because a software browser can
consume several host cores even when the viewer worker and transport are idle.

Presentation rates and quality are observer concerns, not Program execution
rates or deployment pose-source sampling authority. Faster rendering cannot
create newer observed facts. Historical layer negotiation and a supervised
visual-worker process are not current native protocol requirements.

### Runtime resource isolation

The optional strict Linux profile assigns named C++ workers and the
controller-manager update thread explicit scheduler/CPU reservations. The
required priority order is controller/JTC, controller-command callbacks,
runtime collision, stop follow-up, then all planning, validation, Python, DDS,
recording, and viewer work. Composer and the visual worker stay non-real-time;
Python is never promoted into the qualified callback path.

Requested scheduler settings are not evidence. Startup validates declared CPU
sets/priorities and fails when a required request is rejected; live `/proc`
probes verify what Linux applied. PREEMPT_RT, IRQ isolation, hardware
deadlines, and physical stop distance require separate target qualification.
Configure and verify the profile with
[`../operations/REALTIME_RESOURCE_PROFILE.md`](../operations/REALTIME_RESOURCE_PROFILE.md).

______________________________________________________________________

## Authority boundaries — who decides what

The single most important thing to understand about ManyForge is that
execution-critical decisions have **one owner each**, and higher layers observe
rather than override. These software authorities are not functional-safety
functions; external plant safety remains independent.

- **The kernel owns terminal authority.** Whether an execution ends
  `Completed`, `Stopped`, `Failed`, `Interrupted`, or `InterventionRequired`
  is decided inside the C++ kernel, through a single commit path, from the
  authorities active at commit time. A stop request or a fault latch that
  lands mid-decision cannot be laundered into a normal completion — the fault
  outranks the normal terminal. Nothing in the behavior layer, Composer, or an
  LLM can fabricate a terminal state.
- **The addressed kernel owns product Stop semantics.** Scoped Stop and Full
  Stop are first-class cell operations. The owner resolves and captures scope,
  reserves a fixed emergency transaction, atomically fences affected resource
  slots, and hands bounded participant stages to `StopControl`; it then reduces
  immutable results into one certificate and optional fault latch. Journal,
  historian, and client latency cannot delay recognition or containment. A
  controlled stop that a backend refuses does **not** report a stopped-looking
  state—it advances only through the prepared cascade and otherwise remains
  failed/intervention-required. Runtime collision enters this same addressed
  owner; the subordinate engine cannot issue a competing product Stop. Stop
  ownership never lives in a planner, backend, behavior tree, or UI.
- **ManyForge owns collision validation.** Collision decisions come from
  ManyForge's own authoritative collision engine, evaluated per checked
  waypoint — a swapped-in motion planner does not get to self-certify its
  trajectories as collision-free. Bounded, explicitly-configured contact
  (e.g. a gripper finger touching the object it is about to grasp) is allowed
  only through a named contact policy — never by relaxing general collision
  checking. The retained model initializes self-collision pairs before SRDF
  exclusions; prepared queries retain logical-body and selected-group filters,
  including cross-branch mimic dependencies. Cold loading rejects more than
  1024 retained collision geometries before Pinocchio's quadratic index
  allocation. This separate support ceiling does not replace runtime limits
  of 160 logical bodies, 2048 active pairs and 256 joints, or qualify arbitrary
  mesh size and cold-load time.
- **The independent Program runner owns orchestration and run identity.** It
  hosts the validated Behavior tree/node mechanisms, captures one immutable
  Program/Binding selection, sequences nodes, and mints the `ProgramRunId` that
  correlates downstream operations. The kernel consumes that attribution; it
  never invents the Program run. Recovery remains an owner-issued Headless/
  Kernel projection, not a tree, UI, REST, or assistant inference.
- **The Robot Model Authority owns model interpretation.** URDF/SRDF plus
  package paths are interpreted once, into validated metadata and internal
  views — not re-parsed ad hoc per consumer.
- **The execution adapter owns transport progression.** Native, ros2_control,
  and future adapters report command acceptance, feedback, settling, stop, and
  terminal facts through the same port. A viewer pose or behavior tick is not
  controller feedback.
- **Headless owns operator-alarm lifecycle; Composer owns presentation only.**
  Typed source adapters set/clear catalogued conditions and the independent
  Headless alarm owner tracks active/returned and acknowledged/unacknowledged
  states. Composer projects those facts and submits attributed acknowledgement.
  Acknowledgement records awareness only: it cannot clear a condition, clear a
  kernel latch, consume a recovery offer, reset, or move the robot.

This is why the kernel is ROS-free: the governance core must not depend on any
particular robot transport.

### Collision validation

Collision protection is dual-layered and configurable:

- **Plan-time validation** checks the trajectory per waypoint before it is
  dispatched; a trajectory that fails is never sent to the robot.
- **Runtime monitoring** re-checks the lane-selected authoritative execution
  state periodically during execution and faults on a policy violation.

Both layers use the same authority — the motion planner does not get to certify
its own trajectories. Each state query first determines the colliding body-pair
set. When a policy mode needs signed distance/penetration, that query is run
**only for pairs already reported colliding**, never for every possible pair.
This selective rule applies at pre-dispatch validation and runtime monitoring;
the lightweight mode performs no distance query.

General collision checking is never globally relaxed; contact is permitted only
by an **explicit, named contact policy**. Support/release checking offers three
modes:

- **Precise separation** (`signed_penetration`) tracks penetration only for
  colliding allowed pairs. It requires allowed starting contact to improve and
  reach no contact at the end, so sliding without improvement or worsening
  penetration is rejected.
- **Lightweight separation** (`binary_contact`) tracks colliding pair identity
  and count without distance. No new colliding pair is allowed, the set/count
  must strictly decrease when progress is required, and the trajectory must end
  with no contact. It is cheaper but cannot quantify worsening depth inside an
  unchanged mesh-feature contact.
- **Bounded penetration** permits any policy-authorized pair whose measured
  penetration is within its configured limit—including a pair that was not in
  contact at the first waypoint. Unexpected pairs and over-limit penetration
  still fail.

Other contact policies include:

- **Grasp contact** — a gripper finger may touch the object it is about to
  grasp, up to a configured penetration bound. Contact with anything else, or
  beyond the bound, still faults.
- **Support contact** — a release/place motion follows one of the separation or
  bounded-penetration modes above.

Every allowed and rejected contact is reported explicitly in diagnostics, so a
tolerated contact is auditable rather than silent. There is no implicit
minimum-clearance policy: when physical clearance is required, model it in the
collision geometry (for example by conservative mesh inflation) or use an
explicit future policy. A degraded collision-geometry proxy must be identified
in diagnostics rather than used silently.

______________________________________________________________________

## Deployment lanes

The same public contracts drive three lanes; they differ only in what sits
behind the execution backend.

- **ManyForge-only native.** Backend-authored state with RoboPlan/native
  execution. The offline, deterministic lane — used for model-authority and
  collision-validation work with no ROS or hardware in the loop.
- **Isaac commanded simulation.** Stock ros2_control / FollowJointTrajectory
  plus an Isaac topic bridge. Composer/ManyForge remain the command authority;
  Isaac is the streamed plant. The explicit `commanded_sim` profile uses the
  backend-authored command state for reset/planning authority while measured
  Isaac state remains a separately-labelled observation/ghost.
- **Physical ROS-backed robots.** The same public contracts as the Isaac
  ros2_control lane, but controller/driver-observed state is authoritative;
  reset resynchronizes software and never implies physical homing. Vendor
  safety functions and explicit physical qualification remain required.

For a physical deployment, the robot adapter resolves vendor-specific
controller names, namespaces, joints, and interfaces into one immutable
robot-neutral binding. The generic ros2_control backend observes
controller-manager activity, verifies that exact binding outside the update
loop, and requires a fresh lifecycle-admission snapshot at the final bounded
goal-enqueue boundary. Activity or lifecycle-generation loss invalidates the
running authority and cancels the known goal. The lifecycle provider and
production launcher remain robot/deployment adapters; the generic execution
package does not infer vendor names or own robot activation.

The initial Lite6 product shape runs a sole-owner control service separately
from the non-owner Composer service. A committed cell and portable deployment
template resolve into a digest-bound, retained per-run deployment/runtime
record; Composer refuses missing or drifted provenance. Startup configures the
hardware and controllers inactive. Composer or PLC/ROXSIE requests one stable
robot-neutral target intent: `robot.prepare_for_motion` for **Enable robot** or
`robot.make_noncommandable` for **Disable robot**. The lifecycle provider owns
the durable transaction, while the selected vendor adapter owns its resumable
stage plan and proves every stage from fresh observation. Neither client
sequences vendor calls or controller-manager transitions.

Enablement persists while the selected headless authority and its dependencies
remain healthy; it is not a browser/user inactivity lease. For the
`local_operator` profile, an app-lifespan-owned Composer task proves progress
through bounded Kernel/cycle reads and renews the same authority epoch. The
browser is only an HMI. Renewal never creates a session, advances lifecycle
generation, or resets replay identity. Health loss closes admission; a
recovered/restarted component starts observation-only and requires explicit
Enable plus a new execution request. Client and provider reject same-generation
renewal once the prior lease has expired, so a delayed heartbeat/RPC cannot
reopen admission.

The retained runtime record binds the effective heartbeat/lease policy, while
the authority component generates its unpredictable startup epoch inside the
process. Generic admission publication uses a 250 ms lease. The xArm owner's
internal write gate provisionally follows its 1000 ms producer-observation
lease and cannot outlive fresh robot state. Supervision is deliberately
layered:

1. local command, final-dispatch, and active-goal fences close without waiting
   for process management;
1. a ROS-free NRT reducer checks receiver-local heartbeat/progress ages,
   component epochs, generation identities, and the dependency DAG; and
1. an external host supervisor observes the owner and Composer containers,
   consumes Composer's already-reduced critical-health snapshot with its own
   receiver-local freshness/epoch fence, and owns bounded
   dependency-ordered teardown. Composer fences dispatch on the first
   unhealthy reduction; external teardown requires continuous reduced-health
   loss for its recorded persistence budget so a transient cannot destroy a
   healthy owner. Process exit, identity/epoch mismatch, progress regression,
   and stale supervision remain independent terminal paths. A container being
   merely `running` is never sufficient health evidence.

Provider delivery health and robot-observation freshness are deliberately
separate. Advancing, identity-valid lifecycle samples inside the receiver-local
delivery budget prove that the provider component is alive. The observation's
shorter source deadline independently fences Enable, Run, final dispatch, and
active execution. An expired observation never permits motion, but it does not
by itself claim that the provider process died or request restart-domain
teardown. Exact redelivery, stopped progress, delivery timeout, or identity
failure still makes the provider component unhealthy.

The initial external policy is intentionally one-way: an unexpected service
exit, stalled reduced-health channel, component-epoch replacement, or
persistently unhealthy reduced dependency graph is terminal for that launcher
generation. It never auto-restarts, re-enables, or resumes the lane; a later
operator launch creates fresh epochs and starts observation-only. Its
arm/shutdown channel is host-only, while the containers can read the retained
heartbeat evidence. Health polling and process control remain NRT and outside
robot command paths.

The execution environment is one logical, composed fence rather than one
cross-layer lock. Composer's upper pin owns program, deployment, run,
authority-session, and full component-graph identity. The native
ros2_control backend's lower pin owns controller activity/binding,
lifecycle-admission semantics, provider/owner/authority/supervisor epochs, and
its own backend epoch. The planning/validation span, final enqueue, goal
response, active monitor, resume, and successful terminal commit all validate
the applicable immutable pin. Either half changing retires the execution;
ordinary heartbeat/publication generations are correlation evidence and do
not themselves become semantic equality fences.

Reducer, heartbeat, alarm, and process-control work is NRT. None runs in the
controller update, collision, dispatch-linearization, or Kernel stop-critical
path.

The provider projects lifecycle faults through the ordinary alarm/history
system. UI Run/Resume and candidate approval mirror its `canExecute` state, the
backend rejects them while admission is closed, and the execution adapter
rechecks the immutable admission generation at FJT enqueue. The main Stop
remains a motion/tree stop; Disable first stops the cycle and proves it
quiescent, then one provider-owned transaction makes the robot
non-commandable. Cause-specific recovery and controller shutdown remain
separate operations.

Program artifacts remain backend-neutral and can run unchanged when the target
deployment declares the capabilities they require. Validation and
qualification remain lane-, scene-revision-, controller-, and hardware-
specific: a native pass never qualifies an Isaac or physical run by itself.

Arm and gripper lifecycles are deliberately separate. A deployment's gripper
profile selects its command transport, progress/feedback source, dwell or result
timeout, terminal policy, and any grasp-confirmation requirement. Native
preview, Isaac bridges, ROS `GripperCommand`, and future device transports keep
the same `command_gripper` program meaning. The native viewer renders the
authoritative gripper state it receives; it does not progress a command.

Native read-only target resolution retains one verified immutable selection per
composition, keyed by Program Binding, target role and capability. It does not
retain live participants or observations: current provider/route/command and
freshness checks run on every read. Changing the composition discards the slot;
concurrent queries pin their own selected value. This avoids repeatedly
re-verifying unchanged product bytes without adding an observation owner.

Operator grasp confirmation is an explicit, optional qualified source, not
sensor proof or a consequence of successful gripper motion. The native command
owner retains its current-command evidence and expiry. Program waits consume
that evidence with bounded ordinary reads; History records attribution for
traceability but never grants current grasp or attachment authority. The
generated [Headless contract](HEADLESS_REST_API.md) defines the public command.

Assistant-provider selection is a separate axis and never changes an execution
lane's command or state authority:

| Selection                   | Provider path                                              | Operational boundary                                                             |
| --------------------------- | ---------------------------------------------------------- | -------------------------------------------------------------------------------- |
| off                         | Composer without a live model provider                     | No assistant dependency                                                          |
| `direct` (`nemoclaw` alias) | In-tree bridge on `:8100` to an OpenAI-compatible endpoint | Standard launcher uses a host-peer bridge; deployments may sandbox it explicitly |
| `openclaw`                  | NemoClaw-Thor adapter on `:8200` to an OpenClaw gateway    | Sandboxed agent/tool-routing lane; current Thor demo default                     |
| `hermes`                    | NemoClaw-Thor adapter on `:8300` to Hermes sessions        | Sandboxed, opt-in lane with separate provisioning                                |

All engine-backed providers use the same Composer envelope, mode/catalog
checks, callback authority, and review boundary. Provider catalog metadata is
declarative, not live process-placement attestation; verify the selected lane
with [`../operations/LANE_BRINGUP.md`](../operations/LANE_BRINGUP.md).

______________________________________________________________________

## Execution lifecycle

Every motion moves through an explicit, guarded state machine — no implicit
transitions from outside the kernel (simplified below; a resumable `Paused`
state also exists for operator pause/resume):

```
Idle → Planning → Validating → Executing → Completed
                      │            │
                      ├──────► Stopping → Stopped
                      │            │
                      └──► Failed / Interrupted / InterventionRequired
```

- **Planning / Validating / Executing** are phase transitions.
- **Stopped / Completed / Failed / Interrupted / InterventionRequired** are
  *terminal* — reached only through the kernel's terminal-commit path.
- **Reset** is an explicit recovery operation. From moving/stopping states it
  performs the configured bounded stop-before-reset sequence; cycle reset also
  rebuilds the tree and authoritative scene. In a real lane it does not command
  a home pose. Simulator-only profiles may explicitly command their declared
  initial state.

Controlled stops decelerate on-path where the profile allows, with a mandatory
hard-stop floor as the last resort; see
[`STOP_PROFILES_INTERNALS.md`](STOP_PROFILES_INTERNALS.md) and the operator
runbook [`../operations/STOP_PROFILES.md`](../operations/STOP_PROFILES.md).

### Recovery, acknowledgement, and alarms

Terminal recovery is offered, not guessed. The server publishes one current
offer containing exact cycle/kernel/fault identities, revision, and admissible
actions such as retry from current state, replan from stopped state,
`acknowledge_and_continue`, or reset. Stale actions are rejected and return the
new offer without changing state.

`acknowledge_and_continue` is a non-moving recovery action for a classified
`InterventionRequired` boundary. It clears that certified software boundary
without backend reset, leaves the cycle stopped/held, and only makes a later
separate Run/Resume eligible to plan, validate, and dispatch. It never resumes
motion itself.

Operator alarms are a separate supervisory attention system:

| Condition          | Unacknowledged                    | Acknowledged                          |
| ------------------ | --------------------------------- | ------------------------------------- |
| Active             | attention required                | standing active alarm remains visible |
| Returned to normal | return remains attention-required | terminal history only                 |

Only the catalogued source authority can clear a condition. Repeated samples of
one active condition update bounded evidence without flooding the popup; a true
return followed by recurrence creates a new activation and requires a new
acknowledgement. Run/resume and motion-producing recovery remain gated while a
motion-attention alarm is unacknowledged or alarm state is unreconciled. Stop,
Full Stop, and policy-admitted Reset remain available. The bulk acknowledgement
control is shown only when more than one alarm requires acknowledgement.

______________________________________________________________________

## Observability model

ManyForge is built so that a failure at 3 a.m. can be reconstructed. Three
correlation layers, all visible through the public execution API:

- **`program_run_id`** — one operator Run / program-execution session.
- **Action span** — one node attempt (minted when a behavior node enters its
  running state). This is the primary causal key: planning, validation,
  execution, stop, collision, and terminal events for that attempt all carry
  it. Generated child moves (e.g. from a pick-plan node) link back to the
  planning node's span.
- **`contact_policy_id`** — the active contact policy for a contact-tolerant
  move, present on that move's validation and terminal diagnostics.

The kernel stamps these onto every event centrally, so coverage is uniform.
Composer turns them into an **incident model**: a headline cause selected from
the failing action's *own* events (a generic "motion ended in stopped" wrapper
is demoted in favor of the real structured cause, e.g. a planning timeout), a
span-local causal chain, and separately-labelled broader context — so a
previous run's failure can never masquerade as the current one's cause.

Raw events/history and current alarms serve different purposes. Event history
is append-only forensic evidence and may contain repeated warnings/errors.
Current alarms are a bounded condition projection with explicit set, clear,
activation, return, and acknowledgement facts; log severity alone cannot
create or clear one.

Exposed through:

- `GET /api/execution/digest` — active span, headline cause, recovery state.
- `GET /api/cycle/status` — cycle state and the one current versioned recovery
  offer; `POST /api/cycle/recovery-actions` applies an exact offered action.
- `GET /api/execution/history` — filterable by run, span, node, move, and
  contact policy.
- `GET /api/execution/channels` and incident snapshots — span-local chain plus
  labelled context windows.
- `GET /api/execution/alarms` — current four-state alarm snapshot, manager/
  source reconciliation, and bounded lifecycle history; acknowledgement routes
  are revision/activation fenced.
- MCP `manyforge_execution_status` — the same, for assistant/debug tooling.

______________________________________________________________________

## Extending ManyForge

- **New robot** — supply URDF/SRDF and a deployment YAML; see
  [`../operations/ADDING_A_ROBOT.md`](../operations/ADDING_A_ROBOT.md).
- **New planner** — implement `IPlannerBackend` plus the focused
  revision-bound scene projection and model/reference adapters it requires;
  candidate search never replaces ManyForge validation or collision admission.
  **New execution transport** — implement `IExecutionBackend` and its observed
  state/progression contract. The two extension seams are independent, and the
  kernel/program artifacts remain backend-neutral. See
  [`ROBOPLAN_ADAPTER_NOTES.md`](ROBOPLAN_ADAPTER_NOTES.md).
- **Scene / collision authority** — see
  [`SCENE_AUTHORITY.md`](SCENE_AUTHORITY.md).
- **Trusted external behavior node** — use the statically selected
  [`manyforge_example_nodes`](../../manyforge_example_nodes/README.md) package
  as the build/owner-adapter example. Its descriptor, actual source and
  executable binding join the same content-addressed NodeEnvironment as
  built-in nodes. There is no runtime path loader, plugin discovery or
  arbitrary Composer/assistant import.
- **Reusable subtree** — build-owned skill artifacts expand during authoring
  into ordinary Program nodes, with typed arguments, role mapping and retained
  provenance. Expansion does not mutate the Draft or execute the subtree;
  ordinary Program validation and owner admission still apply.

______________________________________________________________________

## Reference contracts

The current generated contracts are:

- [`REST_API.md`](REST_API.md) — the retained structural Composer REST surface,
  generated from the current FastAPI application.
- [`HEADLESS_REST_API.md`](HEADLESS_REST_API.md) — the independent I5/H0
  Manager, runtime, scene, alarm, history, and health projection.

The current versioned WebSocket contract is:

- [`CONTROL_WEBSOCKET_V2.md`](CONTROL_WEBSOCKET_V2.md) — the common
  backend-authoritative execution-control session, fixed heartbeat/freshness
  profile, sequencing, backpressure, reconnect, and integration rules.

The optional MCP adapter is described by the generated
[`MCP_RUNTIME_CONTRACT.md`](MCP_RUNTIME_CONTRACT.md). Tool discovery is not
evidence that an external assistant provider or every historical workflow has
been qualified.

The following files are pinned historical reimplementation inputs. Their
product surfaces are unavailable on the current cutover branch; they are not
generated from, or evidence for, a running current product:

- [`ASSISTANT_PROVIDER_CONTRACT.md`](ASSISTANT_PROVIDER_CONTRACT.md) —
  historical assistant-provider HTTP contract.
- [`VIEWER_DATA_PLANE_V1.schema.json`](VIEWER_DATA_PLANE_V1.schema.json) —
  historical manifest, transform-frame, diagnostics, and lease/control wire
  contracts.

For the repository/component index and assistant-provider setup, see
[`../../AGENTS.md`](../../AGENTS.md).
