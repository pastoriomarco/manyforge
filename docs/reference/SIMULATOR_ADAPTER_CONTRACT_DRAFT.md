> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Simulator Adapter Contract Draft

Status: Draft reference

This document defines the current reference starting point for a ManyForge
simulator integration contract.

It is intentionally not a frozen spec yet.

Use it to:

- evaluate direct Isaac, ROS-wrapped, and hybrid simulator approaches
- keep simulator work aligned with the existing ManyForge runtime model
- avoid throwaway simulator-specific architecture

If the contract proves correct through implementation spikes, the stable parts
should later move into `manyforge_specs`.

## Purpose

ManyForge needs a simulator-facing contract for three concerns:

1. initial live scene capture
2. runtime world/entity operations
3. simulator-backed robot/gripper operation

The goal is to support simulation as a first-class deployment mode without
creating a parallel control architecture.

## Non-Goals

This contract does not:

- replace `IPlanningContext`
- replace `IExecutionBackend`
- define planner APIs
- define assistant/chat APIs
- define browser/HMI-only behavior
- define simulator-specific program semantics

It also does not make offline USD/stage parsing part of the baseline simulator
path.

Offline/static import may become a later extension, but the initial route
should rely on one live observation interface rather than two different scene
sources.

## Architectural Position

The simulator path should add one new simulator-specific family while reusing
the existing ManyForge families for execution and scene-aware runtime logic.

Recommended shape:

- new simulator family:
  - `ISimulatorEnvironment`
- existing ManyForge families, simulator-backed:
  - `IPlanningContext`
  - `IExecutionBackend`
  - gripper/tooling adapter family
  - optional robot-state / controller-management / observability adapters

This keeps the kernel-facing architecture unchanged:

- the kernel still talks to `IPlanningContext`, `IExecutionBackend`, and the
  gripper/tooling family
- simulator-specific APIs stay behind adapter boundaries

## Authority Layers

The simulator path should be described with three different truth layers:

1. environment truth
- the live simulator world

2. ManyForge operational truth
- the backend/planning-context scene ManyForge actually uses for planning,
  validation, references, attach/detach logic, and runtime diagnostics

3. presentation truth
- HMI / Composer / Viser state mirroring what ManyForge currently knows

Normative direction for the simulator path:

- the live simulator world is an external environment truth source
- ManyForge should still expose one operational scene authority through its
  normal backend contracts
- presentation layers must mirror ManyForge operational truth, not read the
  simulator as their own direct source of truth

This keeps the simulator path aligned with the real-world path, where external
reality is also not read directly by the UI as kernel truth.

## Sync Policy

The baseline simulator workflow should use one live observation path for both:

- initial bootstrap
- ongoing runtime query/update

So, for the first implementation:

- bootstrap = first live snapshot from the simulator
- runtime state = continued live query/update from the same simulator
  interface

This avoids:

- separate USD/stage parsing plus live-state reconciliation
- duplicated simulator-specific work for static and dynamic sources
- startup mismatch between offline scene content and live physics state

Optional sync policies that may exist later:

- `bootstrap_only`
- `manual_refresh`
- `continuous_observe`
- `controlled_entities_only`
- `diagnostic_compare_only`

But the baseline preview route should be `continuous_observe` through one live
simulator interface.

## Contract Split

| Concern | Primary contract | Main consumers |
|---|---|---|
| Initial live scene capture | `ISimulatorEnvironment` | Composer, assistant tools, import workflow |
| Runtime world/entity operations | `ISimulatorEnvironment` | scene sync, diagnostics, simulator tools |
| Robot motion execution | existing `IExecutionBackend` | kernel |
| Runtime robot/object queries | existing `IPlanningContext` | kernel, HMI, behavior runtime |
| Gripper command/feedback | existing gripper/tooling family | kernel, behavior nodes |

## `ISimulatorEnvironment`

This is the new simulator-specific contract.

It should cover:

1. discovery and feature query
2. initial live scene capture
3. runtime world/entity operations

### 1. Discovery and Feature Query

Purpose:

- detect what the simulator can honestly do
- avoid hidden assumptions in Composer, assistant tools, and adapters

Minimum operations:

- `GetSimulatorInfo() -> SimulatorInfo`
- `GetSupportedFeatures() -> SimulatorFeatureSet`
- `ListWorlds() -> [WorldRef]`
- `GetActiveWorld() -> WorldRef | null`

`SimulatorInfo` should include:

- simulator name
- simulator version
- adapter implementation name/version
- transport kind
- time mode if known

`SimulatorFeatureSet` should include flags or capability keys for:

- world query
- world pause/resume
- world reset
- world step
- initial live scene capture
- frame inventory export
- entity list/query
- entity spawn/delete
- entity pose/state mutation
- reparent/attachment support
- robot control bridge availability
- gripper control bridge availability

### 2. Initial Live Scene Capture

Purpose:

- extract a deterministic ManyForge-relevant live scene view from the simulator
- support reviewable scene/program authoring

Minimum operations:

- `DescribeWorld(world_id) -> WorldDescription`
- `CaptureSceneBootstrap(request) -> SceneBootstrapSnapshot`
- `ListImportableEntities(request) -> [ImportableEntity]`
- `ResolveFrameInventory(request) -> FrameInventory`

`SceneBootstrapSnapshot` should include:

- `world_id`
- `world_revision` or equivalent snapshot identity
- capture timestamp
- declared base frame
- robots discovered in the scene
- grippers discovered in the scene if known
- importable entities
- frame inventory
- warnings / unsupported elements

Each `ImportableEntity` should include:

- stable simulator entity id
- simulator name / path / namespace
- entity kind:
  - `robot`
  - `gripper`
  - `rigid_object`
  - `sensor`
  - `frame`
  - `environment`
  - `unknown`
- pose in a declared frame
- parent/attachment state if known
- primitive geometry if known
- mesh/asset reference if known
- dynamic/static/kinematic classification if known
- tags or semantic labels if known

Important rule:

- bootstrap is the first explicit live capture/query surface
- it must not silently mutate the ManyForge live scene or program artifact
- it should come from the same live simulator interface used later for runtime
  updates

For the baseline route, this capture should not depend on a second offline
scene source such as USD/stage parsing.

### 3. Runtime World / Entity Operations

Purpose:

- query and, when supported, mutate simulator world state during a ManyForge
  session

This is the same live observation family used for initial bootstrap.

World operations:

- `GetWorldState(world_id) -> WorldState`
- `PauseWorld(world_id) -> MutationResult`
- `ResumeWorld(world_id) -> MutationResult`
- `ResetWorld(world_id, policy) -> MutationResult`
- `StepWorld(world_id, steps | duration) -> MutationResult` when supported

Entity operations:

- `ListEntities(request) -> [EntityRef]`
- `GetEntityState(entity_id, frame_id) -> EntityState`
- `BatchGetEntityState(entity_ids, frame_id) -> [EntityState]`
- `SetEntityState(entity_id, desired_state) -> MutationResult`
- `SpawnEntity(spawn_request) -> SpawnResult`
- `DeleteEntity(entity_id) -> MutationResult`
- `ReparentEntity(entity_id, parent_frame) -> MutationResult` when supported

`EntityState` should include:

- existence
- pose
- linear velocity if known
- angular velocity if known
- parent frame / attachment state if known
- articulation/joint state if applicable
- dynamic/static/kinematic flags if known
- simulator-side metadata
- last update timestamp

## Identity and Binding Rules

The contract must distinguish simulator identity from ManyForge identity.

Required distinction:

- `sim_entity_id`
- `manyforge_id`

Recommended binding record:

- `EntityBinding`
  - `sim_entity_id`
  - `manyforge_id`
  - `binding_kind`: `observed` | `imported` | `controlled`
  - `source`: `bootstrap` | `manual` | `runtime`

This avoids conflating:

- an entity that exists in the simulator
- a scene object imported into ManyForge
- a runtime-controlled object ManyForge believes it owns

## Robot and Gripper Integration

Robot and gripper actuation should remain on the existing ManyForge contracts.

That means:

- robot execution goes through `IExecutionBackend`
- runtime robot/object scene queries go through `IPlanningContext`
- gripper command/feedback goes through the gripper/tooling adapter family

### Optional Internal Bridge

Implementations may use an internal helper such as:

- `ISimulatorRobotBridge`

Possible operations:

- `ListRobots()`
- `DescribeRobot(robot_id)`
- `GetRobotState(robot_id)`
- `ExecuteTrajectory(robot_id, trajectory)`
- `StopRobot(robot_id)`
- `PauseRobot(robot_id)`
- `ResumeRobot(robot_id, trajectory)`
- `ResetRobot(robot_id)`
- `CommandGripper(gripper_id, command)`
- `GetGripperStatus(gripper_id)`

This bridge is implementation-facing only.

It should not become a new kernel-facing control path.

## Relationship to Existing ManyForge Contracts

### `IPlanningContext`

Simulator-backed planning context implementations should remain authoritative
for:

- current robot state
- FK
- reference pose resolution
- object pose/state queries
- backend-owned scene edits
- trajectory validation and collision checks

`ISimulatorEnvironment` may feed those implementations, but should not replace
them.

### `IExecutionBackend`

Simulator-backed execution backends should still own:

- trajectory execution
- stop/pause/resume/reset semantics
- execution phase events
- immutable execution snapshots

The simulator contract may provide lower-level hooks, but the execution
semantics remain the ManyForge execution contract.

### Gripper / Tooling

Simulator gripper implementations should still populate the existing ManyForge
gripper/tooling surface. Simulator-specific transport is an implementation
detail.

## Invariants

These invariants should hold for any simulator implementation path.

1. No simulator-only kernel path
- The kernel must keep using the normal ManyForge ports.

2. No second authoritative scene in the core
- Scene ownership remains adapter-owned.

3. Stable identity within a session
- Simulator entity identifiers and ManyForge bindings must remain stable enough
  for runtime reconciliation and diagnostics.

4. Explicit authority mode per entity
- Each entity should be classifiable as:
  - `observe_only`
  - `manyforge_controlled`
  - `simulator_controlled`

5. Bounded latency
- World/entity queries must have bounded latency suitable for interactive use.
- Stop/control paths must not block on world-scene operations.

6. Traceability
- Simulator mutations initiated by ManyForge must be observable and loggable.

7. Reviewable bootstrap
- Scene extraction must produce explicit, inspectable results before they become
  authored artifacts.

8. Single baseline observation path
- The first simulator implementation should use one live simulator interface
  for both initial scene capture and ongoing runtime scene/state observation.
- Offline/static scene parsing is optional future work, not a baseline
  dependency.

## Candidate Capability Families

If this later moves into `manyforge_specs`, a new simulator capability family
could look like this:

- `sim.discovery.query`
- `sim.bootstrap.live_capture`
- `sim.bootstrap.frame_inventory`
- `sim.world.query`
- `sim.world.pause_resume`
- `sim.world.reset`
- `sim.world.step`
- `sim.entity.list`
- `sim.entity.get_state`
- `sim.entity.set_state`
- `sim.entity.spawn_delete`
- `sim.entity.reparent`
- `sim.robot.bridge`
- `sim.gripper.bridge`

These would describe simulator-facing behavior only.

They should not replace the existing capability families for:

- execution
- planning context
- gripper/tooling
- observability

## Implementation Routes Still Open

This contract does not commit ManyForge to one implementation path yet.

Viable routes still under evaluation:

1. direct simulator adapter
2. ROS 2 wrapper path
3. hybrid path

ROS 2 `simulation_interfaces` is an important candidate for the live
world/entity-oriented part of this contract, but it is not yet the chosen
ManyForge implementation route.

## Open Questions

These questions still need implementation-driven refinement:

- whether any offline/static scene import path is worth adding after the live
  observation route exists
- whether reparent/attachment should be required in the simulator contract or
  optional
- how much joint/articulation detail should live in `EntityState` versus the
  execution/robot-state adapters
- what minimum feature set is required for the first preview milestone
- when this draft is mature enough to promote into `manyforge_specs`

## Current Use

Use this draft as the reference boundary for:

- the Isaac preview implementation discussion
- comparison of direct vs ROS-wrapped vs hybrid approaches
- future simulator-family discussion in `manyforge_specs`

It should be refined after the first concrete simulator spike, not treated as
frozen now.
