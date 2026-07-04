# ManyForge Architecture

A coherent, mid-depth map of how ManyForge is put together: the components, how
a program flows through them, and — most importantly — which layer owns which
decision. For install and operation see
[`../INDEX.md`](../INDEX.md); for exact wire contracts see the generated
references linked at the end.

This document describes **observable runtime behavior and structure**. It is
not a tutorial and not a spec; it is the picture you need before reading any
single component in depth.

---

## Design principles

Five ideas explain most of the decisions below:

- **One owner per safety-critical decision.** Terminal outcome, stop behavior,
  and collision validation each have exactly one authoritative owner; other
  layers observe, they do not override.
- **Transport-independent safety core.** The execution kernel is ROS-free, so
  the safety-critical logic is identical whether the robot is simulated or
  physical — the transport is swapped underneath it, not woven through it.
- **Explicit state, no implicit transitions.** Execution moves through a
  guarded state machine; nothing outside the kernel mutates execution state.
- **Contract-driven modularity.** Planners and robot transports sit behind
  stable interfaces, so a backend can be swapped without touching the kernel,
  the programs, or the public API.
- **Reconstructable execution.** Every event carries correlation identity, so a
  failure can be reconstructed after the fact rather than reproduced live.

---

## Components

| Component | Language | Responsibility |
|---|---|---|
| `manyforge_core` | C++20 | Execution kernel: lifecycle state machine, terminal authority, stop/pause/reset semantics, collision-validation policy, runtime collision monitoring, execution snapshots and events. **ROS-free.** |
| `manyforge_core/python` | pybind11 | Bindings exposing the kernel to Python (poll-based; no C++→Python callbacks). |
| `manyforge_roboplan` | C++ + pybind11 | Primary motion planner / execution-backend adapter (RRT planning + TOPP-RA time-parameterization) and the native kernel client wrapper. |
| `manyforge_behavior` | Python | Behavior-tree program runner (py_trees), node catalog, program/deployment loader, cycle orchestration, and the Robot Model Authority (canonical URDF/SRDF interpretation). |
| `manyforge_composer` | FastAPI + React + MCP | Operator/authoring surface: REST API, program editor UI with a native 3D viewer, and an MCP server for LLM tool access. |
| `manyforge_ros` / `manyforge_ros_bringup` / ros2_control lane | C++/Python | ROS 2 integration: FollowJointTrajectory execution, gripper-action bridge, controller capability checks, real/sim ROS surfaces. |
| `manyforge_assistant_bridge` | Python | Optional direct-lane LLM assistant provider (HTTP). |

### Layers

The same components stack into four layers, and dependencies point only
downward — each layer depends on the one below it, never the reverse. That
one-way rule is what keeps the safety core auditable: nothing a higher layer
does can reach in and change how the kernel decides.

```
┌─────────────────────────────────────────────┐
│  Operator / authoring     Composer (REST/WS/MCP), UI, LLM tools
├─────────────────────────────────────────────┤
│  Orchestration            Behavior runner: program, node catalog, cycle
├─────────────────────────────────────────────┤
│  Safety kernel (ROS-free) Execution lifecycle, terminal + stop authority,
│                           collision validation, events/snapshots
├─────────────────────────────────────────────┤
│  Backend                  RoboPlan (native) / ros2_control / custom
└─────────────────────────────────────────────┘
              │ depends on ▼   (never the reverse)
```

---

## Data flow

```
Program YAML ──► Behavior Runner ──► ExecutionKernel (C++) ──► Planner / Execution backend
                     │                     │                          │
                Node Catalog          State Machine              RoboPlan (native)
                                      Terminal Authority          or ros2_control / JTC
                                      Trajectory Cache                  │
                                      Collision Monitor           Robot / Isaac / sim
                     │                     │
                     └───────── events, snapshots, action spans ───────┘
                                          │
                                   Composer (REST / WS / MCP) ──► Operator UI / LLM
```

A program (behavior tree authored as YAML) is loaded by the **behavior runner**,
which ticks the tree and, for motion nodes, issues execute requests to the
**execution kernel**. The kernel plans/validates/dispatches through a swappable
**planner-execution backend**, runs a collision monitor during execution, and
publishes state snapshots and events. **Composer** observes that stream and
presents it over REST, a WebSocket status feed, and MCP tools.

---

## Authority boundaries — who decides what

The single most important thing to understand about ManyForge is that
safety-critical decisions have **one owner each**, and higher layers observe
rather than override.

- **The kernel owns terminal authority.** Whether an execution ends
  `Completed`, `Stopped`, `Failed`, `Interrupted`, or `InterventionRequired`
  is decided inside the C++ kernel, through a single commit path, from the
  authorities active at commit time. A stop request or a fault latch that
  lands mid-decision cannot be laundered into a normal completion — the fault
  outranks the normal terminal. Nothing in the behavior layer, Composer, or an
  LLM can fabricate a terminal state.
- **The kernel owns stop semantics.** Stop is a first-class operation with an
  explicit result. A controlled stop that the backend refuses does **not**
  report a stopped-looking state — it escalates to an intervention-required
  condition. Stop ownership never lives in the planner or the UI.
- **ManyForge owns collision validation.** Collision decisions come from
  ManyForge's own authoritative collision engine, evaluated per checked
  waypoint — a swapped-in motion planner does not get to self-certify its
  trajectories as collision-free. Bounded, explicitly-configured contact
  (e.g. a gripper finger touching the object it is about to grasp) is allowed
  only through a named contact policy — never by relaxing general collision
  checking.
- **The behavior layer owns program orchestration and run identity.** It
  decides tree structure and node sequencing, and it mints the identifiers
  that correlate an operator's Run press through every downstream event (see
  Observability). The kernel attaches those identifiers; it never invents them.
- **The Robot Model Authority owns model interpretation.** URDF/SRDF plus
  package paths are interpreted once, into validated metadata and internal
  views — not re-parsed ad hoc per consumer.
- **Composer owns presentation, not safety.** It may surface runtime state,
  collision overlays, and stop history, but the safety-critical authority
  stays in the kernel and backend contracts.

This is why the kernel is ROS-free: the safety core must not depend on any
particular robot transport.

### Collision validation

Collision safety is dual-layered and configurable:

- **Plan-time validation** checks the trajectory per waypoint before it is
  dispatched; a trajectory that fails is never sent to the robot.
- **Runtime monitoring** re-checks the observed robot state periodically during
  execution and faults if reality diverges from what was validated.

Both layers are authoritative — the motion planner does not get to certify its
own trajectories. General collision checking is never globally relaxed; the
only way to permit contact is an **explicit, named contact policy**, and the
operator configures its bounds:

- **Grasp contact** — a gripper finger may touch the object it is about to
  grasp, up to a configured penetration bound. Contact with anything else, or
  beyond the bound, still faults.
- **Support contact** — a release/place motion may start in contact with a
  support surface and is required to separate by the end; unexpected re-contact
  or a non-separating path faults.

Every allowed and rejected contact is reported explicitly in diagnostics, so a
tolerated contact is auditable rather than silent. When a robot's collision
geometry cannot be used at full fidelity, planning falls back to a conservative
proxy and says so in diagnostics rather than proceeding silently.

---

## Deployment lanes

The same public contracts drive three lanes; they differ only in what sits
behind the execution backend.

- **ManyForge-only native.** Backend-authored state with RoboPlan/native
  execution. The offline, deterministic lane — used for model-authority and
  collision-validation work with no ROS or hardware in the loop.
- **Isaac commanded simulation.** Stock ros2_control / FollowJointTrajectory
  plus an Isaac topic bridge. Composer/ManyForge remain the command authority;
  Isaac is the streamed plant.
- **Physical ROS-backed robots.** The same public contracts as the Isaac
  ros2_control lane, but relying on real controller feedback and vendor safety
  functions, with explicit physical qualification.

Because the lanes share contracts, a program validated on the native lane runs
unchanged against Isaac or a real robot — the difference is the backend, not
the program.

---

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
- **Reset** returns to `Idle` from any state and is the operator's recovery
  action out of a stopped or intervention-required condition.

Controlled stops decelerate on-path where the profile allows, with a mandatory
hard-stop floor as the last resort; see
[`STOP_PROFILES_INTERNALS.md`](STOP_PROFILES_INTERNALS.md) and the operator
runbook [`../operations/STOP_PROFILES.md`](../operations/STOP_PROFILES.md).

---

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

Exposed through:

- `GET /api/execution/digest` — active span, headline cause, recovery state.
- `GET /api/execution/history` — filterable by run, span, node, move, and
  contact policy.
- `GET /api/execution/channels` and incident snapshots — span-local chain plus
  labelled context windows.
- MCP `manyforge_execution_status` — the same, for assistant/debug tooling.

---

## Extending ManyForge

- **New robot** — supply URDF/SRDF and a deployment YAML; see
  [`../operations/ADDING_A_ROBOT.md`](../operations/ADDING_A_ROBOT.md).
- **New planner/execution backend** — implement the backend interface behind
  which RoboPlan and ros2_control already sit; the kernel, contracts, and
  programs are unchanged. See
  [`ROBOPLAN_ADAPTER_NOTES.md`](ROBOPLAN_ADAPTER_NOTES.md).
- **Scene / collision authority** — see
  [`SCENE_AUTHORITY.md`](SCENE_AUTHORITY.md).

---

## Reference contracts

The exact, versioned wire contracts (auto-generated from the running code):

- [`REST_API.md`](REST_API.md) — Composer REST surface.
- [`MCP_RUNTIME_CONTRACT.md`](MCP_RUNTIME_CONTRACT.md) — MCP tool surface.
- [`ASSISTANT_PROVIDER_CONTRACT.md`](ASSISTANT_PROVIDER_CONTRACT.md) —
  assistant-provider HTTP contract.

For the component/subsystem index and the three deployment modes, see
[`../../AGENTS.md`](../../AGENTS.md).
