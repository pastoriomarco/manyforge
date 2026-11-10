# ManyForge Architecture

One C++ execution host runs the cell's components through ros2_control with
the stock JointTrajectoryController on every lane (Mock, Isaac, Real). The
Kernel inside it owns execution and Stop; the Manager owns Apply and the
execution host process; the Headless API and the Program runner expose those
owners; Composer remains a client.

A coherent, mid-depth map of how ManyForge is put together: the components, how
a program flows through them, and — most importantly — which layer owns which
decision. For install and operation see
[`../INDEX.md`](../INDEX.md); for exact wire contracts see the generated
references linked at the end.

It is not a tutorial and not a spec.

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

## Processes

`./scripts/compose-dev.sh start` runs the workbench in one ROS (Lyrical)
container; `scripts/dev-workbench.py` starts these processes in it:

| Process | Started as | Port | Role |
| --- | --- | --- | --- |
| Program runner | `python -m manyforge_program_runner` | 9011 | Program cycle: tree ticking, Run/Stop/Reset, cycle journal |
| Headless API | `python -m manyforge_headless_api serve` | 9010 | REST/WebSocket over the owners: components, cell latch, readiness, I/O, cycle, history, alarms |
| Manager | `manyforge_manager_daemon` | (local sockets) | Apply journal and convergence; starts and stops the execution host |
| Execution host | `manyforge_ros2_control_execution_host` (started by the Manager) | — | Kernel, planner, collision, component adapters and their processes (`ros2_control.launch.py`, drivers, bridges, the AI assistant's host and bridge) |
| Composer backend | `python -m manyforge_composer` | 9020 | GUI/REST/MCP shell: Drafts, Scenario files, Apply/Load/New orchestration, proxies to the Headless API |
| Composer frontend | Vite (`npm run dev`) | 3000 | Browser GUI |

The execution host also runs the candidate planner as a separate worker
process (`manyforge_roboplan_candidate_worker`). Isaac Sim runs outside the
container, on the same machine or the LAN, and is reached over ROS 2.

## Components

| Component                                 | Language         | Responsibility                                                                                                                                                                                                              |
| ----------------------------------------- | ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `manyforge_core`                          | C++20            | Execution kernel: lifecycle state machine, terminal authority, stop/reset semantics, collision-validation policy, runtime collision monitoring, execution snapshots and events; the component model types (`ComponentState`, cell latch) and the adapter port. **ROS-free.** |
| `manyforge_deployment`                    | C++20            | ROS-free, non-real-time Deployment decoding, schema normalization, cell composition and compiler; the component build spec (`component_spec`, `cell_context`) and the Apply component delta. It is not a Kernel dependency. |
| `manyforge_content_identity`              | C++20            | The one RFC 8785 canonical-JSON codec and SHA-256 digest framing used for content digests.                                                                                                                                 |
| `manyforge_component_manager`             | C++20            | The Manager: durable Apply journal, retained-graph reconciliation, execution host convergence, site settings, and explicit shutdown.                                                                                       |
| `manyforge_headless`                      | C++20 + pybind11 | The Manager daemon (`manyforge_manager_daemon`), runtime/scene source aggregation, alarm lifecycle owner, asynchronous historian, NodeEnvironment projection.                                                                |
| `manyforge_scene`                         | C++20            | Logical-scene owner: canonical-empty bootstrap, Apply handoff, scene transactions.                                                                                                                                         |
| `manyforge_persistence`                   | C++20            | Shared SQLite preflight primitives for the durable owners.                                                                                                                                                                 |
| `manyforge_headless_api`                  | Python/FastAPI   | Bounded REST/WebSocket projection and one execution-control ingress over the owners; component states, cell latch, Run readiness and I/O. It owns transport freshness and delivery, not runtime, Program, scene, alarm, or robot semantics. |
| `manyforge_program_runner`                | Python/FastAPI   | Independently restartable durable Program-cycle owner: exact Applied capture, tree construction/ticking, backend-owned Resetting/allowed commands, current-state idempotence, and explicit Program Stop/Reset orchestration.   |
| `manyforge_core/python`                   | pybind11         | Bindings exposing the kernel to Python (poll-based; no C++→Python callbacks).                                                                                                                                               |
| `manyforge_collision`                     | C++20            | Planner-neutral collision-world/query implementation behind ManyForge collision ports.                                                                                                                                      |
| `manyforge_roboplan`                      | C++ + pybind11   | The execution host: RoboPlan planning adapter and candidate worker, kernel composition, the ros2_control component adapter, the adapter registry, launch artifacts (`controller_launch_artifacts`) and the Mock, simulation and point-cloud adapter families. |
| `manyforge_ros2_control_execution`        | C++20/ROS 2      | Controller execution backend: FollowJointTrajectory and gripper commands, feedback/settling, bounded stop/reset; the Mock lane folder (`lane/`).                                                                            |
| `manyforge_xarm_control`                  | C++20/ROS 2      | xArm vendor adapter family (xArm 5/6/7, Lite 6, UF850): machine owner, arm and I/O members, launch contribution; vendor folder `vendor/`.                                                                                  |
| `manyforge_isaac_adapter`                 | C++20/ROS 2      | Isaac Sim lane adapter family: lane folder `lane/`, launch contribution, topic-based ros2_control settings, gripper bridges.                                                                                               |
| `manyforge_behavior`                      | Python           | Node catalog/environment plus Program validation, tree, admission, scheduling, and focused client mechanisms hosted by the independent runner; it does not own robot effects.                                               |
| `manyforge_composer`                      | FastAPI + React  | Structural/operator shell, Draft authoring, server-side Apply/Load/New, transparent control/viewer proxies, and backend-state presentation. It owns no execution lifecycle.                                                 |
| `manyforge_ros` / `manyforge_ros_bringup` | C++/Python       | ROS utilities (simulation control over `simulation_interfaces`, standard controller/gripper surfaces) and launch files (`ros2_control.launch.py`, pose source, point-cloud source, AI model, AI assistant host, simulated PLC, Isaac gripper). |
| `manyforge_example_models`                | data             | Component packages (`component.yaml`, URDF/SRDF, meshes, lane rows) for the included robots and tools; machine models in `components/`.                                                                                    |
| `manyforge_assistant_bridge`              | Python           | AI tab model loop against the Deployment's `ai_model` endpoint, strict active-mode enforcement, Draft/read callback routing, cancellation/reconciliation, conversation history, and redacted audit. It owns no runtime effects or catalog truth. |

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
│  Backend                  RoboPlan planner / component adapters
│                            (ros2_control lanes, vendor adapters)
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

Each Apply carries a Scene choice: `initialize` (reset the logical scene and
seed it from the authored Program/Deployment) or `preserve` (keep it). Load,
New and a Deployment Apply initialize it. A failed initialization, or a pose-apply failure after
readiness, is a typed condition, not a silent fallback.

### Scene Viewer data plane

The Scene Viewer is a **non-real-time observer**. It does not read planner or
collision-engine internals directly, advance execution or gripper lifecycles,
mutate the scene, certify validation, or decide stop/recovery outcomes. It
consumes immutable facts already published by their authoritative owners.

```text
Execution host observation -> Headless credited socket -> Composer latest slot
                                                               |
                                                       /ws/scene/viewer
                                                               |
                                                    browser stable scene graph
                                                               |
                                                   applied-frame credit back
```

`GET /api/source-model` supplies
the retained model/topology on demand. `/ws/scene/viewer` supplies addressed
current joint observations; scene facts and other cold operator state arrive
through `/ws/control`. Neither heartbeat passage nor volatile sample counters
cause unchanged geometry to be rebuilt.

Cold source-file invalidation uses the existing C++ canonical source reader,
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
create newer observed facts.

______________________________________________________________________

## Authority boundaries — who decides what

The single most important thing to understand about ManyForge is that
execution-critical decisions have **one owner each**, and higher layers observe
rather than override. These software authorities are not functional-safety
functions; external plant safety remains independent.

- **The kernel owns terminal authority.** Whether an execution ends
  `Completed`, `Stopped`, `Failed`, `Interrupted`, or `InterventionRequired`
  is decided inside the C++ kernel, through a single commit path, from the
  authorities active at commit time. A stop request or a component fault that
  lands mid-decision cannot be laundered into a normal completion — the fault
  outranks the normal terminal. Nothing in the behavior layer, Composer, or an
  LLM can fabricate a terminal state.
- **The addressed kernel owns product Stop semantics.** Scoped Stop and Full
  Stop are first-class cell operations. The owner resolves and captures scope,
  reserves a fixed emergency transaction, atomically fences affected resource
  slots, and hands bounded participant stages to `StopControl`; it then reduces
  immutable results into one certificate. What a Stop leaves behind is read
  from component states (the cell latch, below), not stored. Journal,
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
- **The execution adapter owns transport progression.** The ros2_control
  backend and future adapters report command acceptance, feedback, settling, stop, and
  terminal facts through the same port. A viewer pose or behavior tick is not
  controller feedback.
- **Headless owns operator-alarm lifecycle; Composer owns presentation only.**
  Typed source adapters set/clear catalogued conditions and the independent
  Headless alarm owner tracks active/returned and acknowledged/unacknowledged
  states. Composer projects those facts and submits attributed acknowledgement.
  Acknowledgement records awareness only: it cannot clear a condition, clear the
  cell latch, consume a recovery offer, reset, or move the robot.

This is why the kernel is ROS-free: the governance core must not depend on any
particular robot transport.

### Collision validation

Collision protection is dual-layered and configurable:

- **Plan-time checking** keeps every trajectory collision-free before it is
  dispatched: the planner checks its own paths (edges, start, goal and
  shortcuts, with the clearance below), and pre-dispatch validation checks
  the waypoints of every cached trajectory, of every fresh one whose move has
  an active contact policy (support contacts or a contact grasp, whose pairs
  the planner switches off), and of every other fresh one unless the robot's
  Deployment turns `collision_check_fresh_plans` off. A trajectory that fails
  is never sent to the robot.
- **Runtime monitoring** re-checks the lane-selected authoritative execution
  state periodically during execution and faults on a policy violation.

Pre-dispatch validation and the planner keep a **collision clearance**
(Deployment `collision_clearance_m`, 0 to 0.05 m; 0.001 m (1 mm) by
default, which also covers the executed spline's small deviation from the
planned path; 0 turns it off and only contact is checked). A motion's own
`clearance_m` (Program motion
nodes, Robot Control's Advanced settings) replaces it for that move. Robot and carried bodies must stay at
least that far from every environment body, except where the collision policy
allows contact: static, touch-link and support-contact pairs and the active
contact-grasp pairs keep clearance 0, and their contact tolerances apply as
before. The move's target object gets the clearance unless such a policy
allows the contact. A move that starts inside the clearance may move away but
not closer. Runtime monitoring and lookahead use clearance 0: they stop only on
contact.

Pre-dispatch validation runs for cached trajectories, for fresh ones with an
active contact policy and, while the robot's Deployment keeps
`collision_check_fresh_plans` on (the default), for every other fresh plan.
With it off, a fresh plan without a contact policy stands on the planner's
check: its validation event reports `collision_evidence=planning`, a
validated one `pre_dispatch_validation`. A fresh plan that fails is replanned
within the authoritative-validation attempts, like a refused cached one. It
runs in ManyForge's collision engine, like the runtime checks, on the
trajectory's own waypoints only; the clearance covers the executed spline's
deviation between them. The planner checks its
paths at the Deployment `collision_check_step_rad` (0.05 rad of joint-space
distance by default) on its first attempt, and the finer of it and 0.01 rad
on a replan after a refusal.

Pairs that can never collide are not checked. The composed cell's SRDF takes
each model's own disabled pairs, disables every pair between the rigid link
groups that meet at a mount (reason `Adjacent`), and adds the arm–tool pairs a
Deployment attachment lists under `collision_exclusions` (`{child_link,
parent_link}`, reason `Never`; a link the model lacks is refused). These are
part of the arm's planning model, so changing them restarts nothing.

Both layers use the same collision authority. Each state query first determines the colliding body-pair
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
tolerated contact is auditable rather than silent. Beyond the collision
clearance above, when more physical clearance is required for one body, model
it in the collision geometry (for example by conservative mesh inflation). A degraded collision-geometry proxy must be identified
in diagnostics rather than used silently.

______________________________________________________________________

## Planning

The planner is RoboPlan 0.8.0 (pinned by `ROBOPLAN_PINNED_REF` in
`scripts/setup-docker.sh` and `.docker/roboplan.manyforge-sha`; patches and
source closure in `third_party/roboplan/`). The execution host runs it in a
separate worker process with a deadline; candidate search never replaces
ManyForge's own validation and collision checks. Adapter details:
[`ROBOPLAN_ADAPTER_NOTES.md`](ROBOPLAN_ADAPTER_NOTES.md).

- **Planner block.** Free-space planning is set per robot in the Deployment
  (`planner_search`: `rrt` or `rrt_connect`, default `rrt_connect`;
  `planner_optimize`: `first`, `rrt_star` or `best_of_n` with `planner_n`;
  `planner_max_nodes`; `collision_check_step_rad`; `planner_rng_seed`). A
  motion node's optional `planner` parameter (`search`, `optimize`, `n`,
  `collision_check_step_rad`, `max_nodes`) overrides it for that move.
- **Cartesian timing.** `cartesian_timing` is `constant_speed` (RoboPlan's
  Bounded timing: accelerate, hold the TCP speed, decelerate; the Deployment
  default) or `time_optimal` (as fast as the joint limits allow), set per
  robot and per move. Corners of a multi-waypoint path are rounded by
  `blend_radius_m` (per move, per waypoint) before timing; radius 0 keeps the
  exact corner, where the tool stops.
- **Motion timing.** `motion_timing` (per robot, default `smooth`) times
  joint and pose goals. `smooth`: TOPP-RA within the joint limits, then one
  uniform stretch until the active TCP is at 0.98 of its speed limit at the
  samples (and joint caps and jerk limits, if set, hold): gentle, slower.
  `fast`: TOPP-RA holds the TCP at 0.98 of its limit on its grid:
  quicker, with abrupt acceleration changes. Cartesian paths follow
  `cartesian_timing` only.
- **TCP speed limit.** Either way the active TCP targets 0.98 of its speed
  limit (inside TOPP-RA for `fast` and `time_optimal` Cartesian paths, by the
  final stretch for `smooth`); a check at the trajectory samples stretches the
  trajectory if a sample still exceeds it. The vendor TCP speed limit is never
  exceeded.
- **Clearance.** Every planned motion keeps the collision clearance above
  (1 mm by default); fresh plans get the pre-dispatch waypoint check unless
  the Deployment turns `collision_check_fresh_plans` off, as described under
  *Collision validation*.

Planner settings belong to the cell context (see *Component build spec*): an
Apply that changes them restarts no component.

______________________________________________________________________

## Cell kinematic index and TCP terms

The cell kinematic index is a read-only view over facts the deployment layer
already holds. It is built from the cell graph (what is mounted on what, and
which component owns each model) and from the kinematic frame paths of a
verified model map. Code that needs a mount, owner, child or frame-path answer
asks the index instead of walking the graph by hand. The index is
`deployment::CellKinematicIndex`, in `manyforge_deployment`.

The index owns nothing. Each fact keeps the owner it already has:

| Fact | Owner |
| --- | --- |
| What is mounted on what, who owns each model, the default TCP | The deployment recipe, compiled into the cell graph of the retained bundle |
| Which scenario is live | The Component Manager's applied publication |
| Each component's live status | The execution host's `ComponentState` |
| Frames, targets, compiled links and frame paths | The compiled model map, verified at load |

The index holds no state, authority, lifecycle, id or digest. It is derived
again whenever a verified model map is built, and during Apply once for each
side (the current graph and the candidate graph). It pins nothing: each
consumer keeps its own identity checks and its own typed error codes. Queries
return null or an empty list; they do not raise errors.

The queries are:

- the owner of a model instance (null for a passive model);
- the component a given component is mounted on;
- whether one component carries another, directly or through a chain of mounts;
- a component and everything mounted on it, through the whole chain;
- the direct children of a component, in graph order;
- the tool on a robot interface (the first matching edge in graph order);
- the kinematic frame paths, and the path of one frame (null when there is none
  or more than one).

Two frame role bindings are compared with `same_program_frame`, which checks
role, model, revision and compiled name. The frame-subset checks use this
helper.

### TCP terms

- **Flange**: a robot's mount point, the interface a tool attaches to. A
  robot's flange takes at most one tool. The flange is also a frame role on
  the robot.
- **TCP**: a tool center point, a named frame of a robot or tool that a Program
  or the GUI can target. A component lists its TCPs under `tcps`.
- **Default TCP**: the first `tcps` entry of a robot or tool. While a tool is
  mounted, its default TCP takes precedence over the robot's.

______________________________________________________________________

## Deployment lanes

The same public contracts drive the Mock, Isaac and Real lanes; they differ
only in the ros2_control hardware behind the controllers. All three run stock
ros2_control with the stock JointTrajectoryController (FollowJointTrajectory),
launched by `ros2_control.launch.py`. A component picks its lane with
`components[].lane` in the Deployment; a machine member runs on its owner's
lane.

- **Mock.** ros2_control mock hardware; no simulator or robot. Mock also
  covers offline preparation.
- **Isaac.** ros2_control with topic-based hardware bridged to Isaac Sim
  (`manyforge_isaac_adapter`). ManyForge remains the command authority; Isaac
  is the streamed plant. The simulator is reached through the generic ROS 2
  `simulation_interfaces` services.
- **Real.** ros2_control with the vendor's hardware plugin, through a vendor
  adapter (xArm: `manyforge_xarm_control`). Controller/driver-observed state
  is authoritative; reset resynchronizes software and never implies physical
  homing. Vendor safety functions and the integrator's safety controller
  remain required.

______________________________________________________________________

## Component model

Every robot, tool, I/O device, sensor, pose source, AI model, AI assistant and
simulator in the cell is a **component**. The execution host owns them; the core holds and
publishes one view of each, and everything else reads it.

### Component state

`ComponentState` (`manyforge_core/include/manyforge_core/types/component_state.hpp`)
carries the component's `lifecycle` (`offline`, `not_prepared`, `prepared`,
`enabled`, `fault`), a `fault` when in `fault`, a `reason` when offline or not
prepared, its `activity` (`idle`, `busy`, `unknown`: a command's result has
not come back), stream freshness, declared capabilities, the action in
progress and the last finished action, display-only vendor `details`, and,
only while enabled, the `endpoints` it serves (`{role_id, url}`, for example
the AI assistant's `assistant_request` URL; unlike `details`, clients may act
on them). The execution host publishes every change; the Headless API serves
`GET /api/components` and pushes `component_state` events on `/ws/control`.

### Actions and capabilities

`POST /api/components/{component_id}/actions` (and `POST
/api/components/actions` for several) runs one of `prepare`, `enable`,
`disable`, `recover` or a declared capability: `reset_position`, `power_off`,
`reboot`, `sim_reset` (`io_manual_write` and `io_output_readback` describe I/O
outputs). One action runs at a time per component; a Stop interrupts an
action in progress; an action whose effect is unknown ends `unknown` and is
never resent. Actions stay available during a cell latch and during a run.
Declaring `sim_reset` identifies a simulator; its state never latches the cell
or blocks Execute.

### Faults and the cell latch

A `Fault` has a typed `code` (`fault_estop`, `fault_protective_stop`,
`fault_drive`, `fault_communication`, `fault_stream_stale`,
`fault_process_exited`, `fault_vendor`, …), the vendor's code and text where
there is one, and the `operator_action` that clears it (`enable`, `recover`
or `none`). The adapter maps vendor codes; unmapped ones pass through as
`fault_vendor`.

The **cell latch** is derived, never stored: after a Stop, non-exempt
components in the cell cause it while in `fault` or `offline` or reporting
`activity: unknown`; a fault raised while anything executes can also latch
the cell. A component declaring `sim_reset` is the simulator and is exempt in
every state, including `offline` or `fault`. The existing operator-power and
unavailable-owner-member exemptions also apply. The latch clears by itself
when its causes are gone; there is no recovery endpoint or acknowledgement.
While latched, Program run and resume, every node, interrupt evaluation,
manual motion and manual I/O are refused (`cell_latched`); component actions,
Apply, Load and New are not.
`GET /api/cell/latch` and the `cell_latch` event carry it.

A simulation that isn't playing doesn't itself fault or latch the cell. When
reachable and paused or stopped, the simulator component shows `prepared`; if
unreachable, it stays `offline` with reason `simulator_unreachable`. Declaring
`sim_reset` identifies the simulator, whose own state never latches the cell
or blocks Execute. A simulated robot or tool keeps the lifecycle its
controller reports with reason `simulation_not_playing` and stale streams.
Enable and Execute remain available when otherwise ready, so the Program's
own nodes can play the simulation. New motion and tool commands on a component
showing `simulation_not_playing` are refused with that reason.

### Machines: owners and members

One physical machine can be several components: an **owner** (connection,
driver process, controller state, power) and its **members** (an arm, I/O
banks), with ids `<owner_id>_<role>`. Members run on the owner's lane and are
shown `offline` (`owner_unavailable`) while the owner isn't usable; a
member action is refused (`owner_not_prepared`) while the owner isn't
prepared. The owner has `prepare`, `recover` and its capabilities, no Enable.
Controller faults are the owner's, with the members they concern. Members can
be deactivated (`active: false`) but are never removed. On the real xArm,
Prepare and Recover connect and clear errors with the motors off; the motors
switch on only at the arm's Enable.

### Adapters and the registry

An adapter implements the ROS-free port
`manyforge_core/include/manyforge_core/ports/icomponent_adapter_port.hpp`:
it reports `ComponentState`, runs actions, and declares capabilities. It
reports what the device or controller reports and adds no timers or retries
of its own. Adapter families are shared libraries registered in the ament
resource index `manyforge_component_adapters`; the execution host loads every
family through the registry and links none of them. Families today: Mock,
simulation, point-cloud and AI assistant (`manyforge_roboplan`), Isaac
(`manyforge_isaac_adapter`), xArm (`manyforge_xarm_control`). Components
without a lane (passive tools, I/O-driven grippers) get the host's small
generic adapter.

A family returns a **launch contribution** for its components: the
ros2_control hardware section, controllers, driver parameters and remappings,
built from the model's `lanes.<lane>` row in `component.yaml` and the
family's folder (`vendor/` or `lane/`: `driver.yaml` with the pinned source,
`manyforge_settings.yaml` with ManyForge's additions, `README.md`). The
vendor-free generator (`controller_launch_artifacts`) adds instance values
(namespace, device address from the site settings) and writes the robot
description, controllers YAML and driver parameters that
`ros2_control.launch.py` runs. A machine's process belongs to its owner.

### Component build spec

`manyforge_deployment` splits every compiled fact into two tiers
(`manyforge_deployment/include/manyforge_deployment/component_spec.hpp`):

- **`ComponentSpec`** (per component): what it is launched and constructed
  with, in the sections `adapter`, `address`, `bindings`, `controllers`,
  `description`, `launch`, `owner` and `providers` — its models and mounts,
  its rows of the execution host configuration, the stream budget overrides
  it is constructed with (a robot's `measured_joints`), its control bindings and
  domains, the resolved device address, and for a member its owner's spec
  fingerprint.
- **`CellContext`** (per cell): policies (with the stream budget overrides
  re-read on Apply), takeover budgets, Scene and
  planning model (with collision exclusions), pose sources, default TCPs,
  global rules, planner settings and assistant settings.

`component_spec()` and `cell_context()` are pure functions of the compiled
bundle (and the site's device addresses). A component's **fingerprint** is
the digest of its spec (`component_spec_fingerprint`). On Apply, a component
is replaced exactly when its fingerprint changes, and the replacement
reasons are the spec sections that differ. Everything in the cell context is
refreshed on Apply without restarting anything. The execution host builds
each component (adapter, launch, backend configuration, owner process, I/O
member, sensor, simulated PLC, observers) from its spec only, never from the
whole bundle: its build functions take a component build input (the spec, an
arm member's owner's spec and host handles), which has no bundle, model map
or package root. Those stay on the participation path (planning model and
scene, Program bindings and TCPs, the runtime participant binding, route
open), which hands a build function only what it resolved there.

### Apply, Load and New

Apply, Load and New run server-side on one path in the Composer backend,
used alike by the GUI, REST and MCP:

- `POST /api/deployment/preview` lists each component as `add`, `remove`,
  `replace` or `retain`, with its reasons;
- `POST /api/deployment/apply` applies the Deployment Draft: for each removed
  or replaced component, `disable`, Apply, then (replaced) `prepare` — owners
  prepared before members, members disabled first. The processes of removed
  and replaced components stop before any new one starts; a component whose
  `disable` fails is stopped the same way, and the failure shows on its
  row. Affected components end `prepared`, not enabled, on every lane;
- `POST /api/scenario/new` empties the cell (Apply of the built-in Untitled
  Scenario); it always works;
- `POST /api/scenario/load` is New, then Apply of the Scenario file.

Apply is refused while a Program run is in progress
(`apply_refused_run_in_progress`) and allowed during a cell latch. Global
rules return to their `active_at_start` value at Load.

### Run readiness

One computation in the Headless API (`GET /api/program/readiness`, event
`run_readiness`) says whether Execute and Resume may run, with typed blocked
reasons (`cell_latched`, `component_not_enabled`, `component_unhealthy`,
`rule_holds`, `input_not_fresh`, `run_in_progress`, …). Every component in the
cell must be enabled; an owner counts as ready when `prepared`, as does a
simulator that isn't playing. The GUI's Activate prepares and enables every
active component, then executes once. Every Program starts held and needs
Run.

______________________________________________________________________

## Programs on every lane

Program artifacts remain backend-neutral and can run unchanged when the target
deployment declares the capabilities they require. Validation and
qualification remain lane-, scene-revision-, controller-, and hardware-
specific: a Mock pass never qualifies an Isaac or physical run by itself.

Arm and gripper lifecycles are deliberately separate. A deployment's gripper
profile selects its command transport, progress/feedback source, dwell or result
timeout, terminal policy, and any grasp-confirmation requirement. Mock, Isaac
bridges, ROS `ParallelGripperCommand`, and future device transports keep the same
gripper node meaning. The viewer renders the authoritative gripper state it
receives; it does not progress a command. On Stop, a gripper already within
its finish tolerance of the target (`gripper_stop_finish_tolerance` in the
component package) finishes instead of being cancelled.

The execution host's read-only target resolution retains one verified immutable selection per
composition, keyed by Program Binding, target role and capability. It does not
retain live participants or observations: current provider/route/command and
freshness checks run on every read. Changing the composition discards the slot;
concurrent queries pin their own selected value. This avoids repeatedly
re-verifying unchanged product bytes without adding an observation owner.

The AI assistant is a separate axis and never changes an execution lane's
command or state authority. The Deployment names the model server with an
`ai_model` component; its lane-free `assistant` component (at most one) names
that model and an assistant profile with a chat mode. Headless builds what
the model sees from the node catalog it loaded (node summaries and schemas,
tool descriptors, named types) and publishes it with the model endpoint as the
active assistant mode projection; the authorable nodes are the stable catalog
nodes whose capabilities the Deployment provides. The assistant component's
adapter runs a ROS host node (`assistant_host`) from Prepare; at Enable the
host starts the ROS-free bridge on a loopback socket with an ephemeral port
and the adapter publishes its URL as the endpoint `assistant_request`, which
Composer reads for every call. Disable stops the bridge and cancels its
requests; a host or bridge exit is the component's `fault_process_exited`.
Chat and Program runs never overlap: Composer refuses a chat request while a
run exists and Execute while one of its chat requests runs. The bridge runs
the model loop: it re-verifies that
projection before every model turn, checks each tool call's arguments, and
sends Draft edits to Composer's tool callback, which commits them under the
same fences as manual edits. Assistant edits land only in the Program Draft;
the operator Applies or Discards them. See
[`ASSISTANT_PROVIDER_CONTRACT.md`](ASSISTANT_PROVIDER_CONTRACT.md).

______________________________________________________________________

## Execution lifecycle

Every motion moves through an explicit, guarded state machine — no implicit
transitions from outside the kernel (simplified below). There is no
operation Pause: Stop halts a move, a new move continues toward a target, and
runtime speed scale 0 freezes in place without cancelling:

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
  rebuilds the tree and authoritative scene. It never commands a home pose;
  where a component declares `reset_position` (Mock and Isaac robots), that
  separate action, or Reset's **Reset robot position** option, returns it to
  its initial joint positions.

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

Component faults and the cell latch are separate from alarms: they clear when
the component is healthy again (Recover, Enable), never by acknowledgement.

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

- `GET /api/cycle/status` — cycle state and the one current versioned recovery
  offer; `POST /api/cycle/recovery-actions` applies an exact offered action.
- `GET /api/execution/history` — filterable by run, span, node, move, and
  contact policy.
- `GET /api/execution/alarms` — current four-state alarm snapshot, manager/
  source reconciliation, and bounded lifecycle history; acknowledgement routes
  are revision/activation fenced.
- MCP `manyforge_execution_status` — the same, for assistant/debug tooling.

### Typed errors and the error catalog

A refusal carries one carrier everywhere: `{code, message, details}` plus the
`producer` that produced it (`component_manager`, `kernel`, `headless_api`,
`composer`). The code is stable; the message is the producer's own text.

- **C++ boundaries** throw `manyforge::core::TypedError` (code, message,
  details, optional producer). A boundary catch keeps the code with
  `core::CauseOf`; an exception without a code becomes the boundary's own code.
- **Manager RPC**: every refusal's `error` carries `producer`. A command that
  does not decode is `invalid_request` with `details` mapping the refused field
  path to its field code.
- **HTTP** (headless API and Composer): refusals are
  `{"detail": {code, message, details, producer, request_id}}`. A request
  that fails validation is `invalid_request` (HTTP 422) with `details` mapping
  each refused field path (for example `body.request_id`) to its field code;
  an uncaught headless failure is `headless_api_internal_failure` (HTTP 500).
- **Apply**: a failed Apply's terminal record keeps the failed step as `code`,
  the inner cause's own `cause_code` and `cause_details`, its `producer`, and
  `effect_boundary`. `before_effect` means the Apply made no configuration,
  runtime, component or Scene change; an admission claim, a Kernel fence and
  fail-closed readiness may still exist, and whoever releases them checks those
  facts directly. The operator can acknowledge a proven-before-effect failure
  (`POST /api/manager/apply/acknowledge`); `GET /api/manager/apply/status`
  reports the acknowledgement state. A failure with an unknown boundary keeps
  its fence and claim until New: New releases them
  (`POST /api/manager/apply/release-for-new`) and its own Apply then empties
  the cell, whatever the failed Apply installed. When the
  execution host finds a data or setup inconsistency while composing the
  candidate, it refuses only that Apply (`scenario_composition_refused`, with
  `details.cause`): the Apply ends `failed` with known effects, nothing is
  published, and the runtime keeps running. Like any Apply that fails before
  its first fence, the refusal releases the Apply's fence and admission claim
  at once, so authoring, Load and New work again.

The error catalog `manyforge_core/resources/error_catalog.tsv` has one line per
code: `code`, compact text, full text. The area is the code prefix. The
generator `manyforge_core/tools/generate_error_catalog.py` renders C++, Python
and TypeScript constants (nothing reads the file at runtime), and
`scripts/ci/check-error-catalog.py` refuses a code used at an error-construction
site that has no row. Codes built at runtime are not seen by that check. The GUI
shows a refusal's compact text, with the full text and the producer's message as
detail; a code without a row shows the producer's message.

______________________________________________________________________

## Extending ManyForge

- **New robot, tool or I/O device** — add a component package
  (`component.yaml` with its `lanes.<lane>` rows, URDF/SRDF, meshes; templates
  in `components/templates/`); see
  [`../authoring/ADDING_A_COMPONENT.md`](../authoring/ADDING_A_COMPONENT.md).
- **New vendor or lane** — implement an adapter family (`IComponentAdapter`
  and its launch contribution) with its `vendor/` or `lane/` folder and
  register it in `manyforge_component_adapters`; the core and the execution
  host need no change.
- **New planner** — implement `IPlannerBackend` plus the focused
  revision-bound scene projection and model/reference adapters it requires;
  candidate search never replaces ManyForge validation or collision admission.
  **New execution transport** — implement `IExecutionBackend` and its observed
  state/progression contract. The two extension seams are independent, and the
  kernel/program artifacts remain backend-neutral.
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
- [`HEADLESS_REST_API.md`](HEADLESS_REST_API.md) — the independent Headless
  Manager, runtime, scene, alarm, history, and health projection.

The current versioned WebSocket contract is:

- [`CONTROL_WEBSOCKET_V2.md`](CONTROL_WEBSOCKET_V2.md) — the common
  backend-authoritative execution-control session, fixed heartbeat/freshness
  profile, sequencing, backpressure, reconnect, and integration rules.

The optional MCP adapter is described by the generated
[`MCP_RUNTIME_CONTRACT.md`](MCP_RUNTIME_CONTRACT.md). Tool discovery is not
evidence that an external assistant provider has been qualified.

For the repository/component index and assistant-provider setup, see
[`../../AGENTS.md`](../../AGENTS.md).
