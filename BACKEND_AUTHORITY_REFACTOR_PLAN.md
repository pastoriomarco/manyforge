# Backend-Authoritative Pick-And-Place Refactor Plan

Date: 2026-04-07

## Purpose

This document defines the refactor plan for the ManyForge pick-and-place demo so that:

- the backend is the single source of truth for runtime robot and scene state,
- the HMI/Composer passively reflects backend state and never becomes authoritative,
- pick poses are derived from the current object pose at runtime,
- attach/detach logic uses fresh backend data,
- reset semantics are explicit and compatible with future real-robot execution.

This plan is intentionally more specific than the general audit in
[`ANALYSIS_REPORT.md`](/home/tndlux/workspaces/dev_ws/src/manyforge/ANALYSIS_REPORT.md).

## Non-Negotiable Rules

- Runtime robot state is backend-owned.
- Runtime scene/object state is backend-owned.
- HMI/Composer/blackboard values may cache, display, or parameterize, but they must not become
  authoritative runtime state.
- Run/Stop/Reset are operator intents. Each layer reacts to them according to its own contract:
  execution backend, kernel, orchestrator/BT, and HMI must each implement their own coherent
  behavior.
- Reset must not "teleport" a real robot. Returning to rest/home is an explicit motion/recovery
  behavior, not a side effect of clearing software state.
- Any mechanism that allows HMI/backend drift is itself a defect and must be replaced.

## Relevant Specs

The current specs already point strongly in this direction:

- Backend-owned scene/state and no ManyForge-owned runtime scene copy:
  [`220-core-ports-and-threading.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/220-core-ports-and-threading.md#L25)
- Execution-owned runtime state has one authoritative writer; observers consume immutable
  snapshots/accessors:
  [`220-core-ports-and-threading.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/220-core-ports-and-threading.md#L27)
- `GetCurrentState()` is authoritative backend state, but not the main HMI observer surface:
  [`220-core-ports-and-threading.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/220-core-ports-and-threading.md#L98)
- Execution backend provides observer-safe state snapshots:
  [`220-core-ports-and-threading.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/220-core-ports-and-threading.md#L138)
- HMI/diagnostics must use `GetStatus()` / `GetExecutionSnapshot()` and not inspect mutable backend
  internals directly:
  [`230-kernel-contracts-state-machine.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/230-kernel-contracts-state-machine.md#L84)
- Runtime scene ownership remains backend/adaptor-owned:
  [`400-program-model-and-artifacts.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/400-program-model-and-artifacts.md#L204)
- Composer live mode may visualize the backend-owned scene via the planning adapter:
  [`ADR-0014-orchestration-tooling-logic-builder-gui.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/adr/ADR-0014-orchestration-tooling-logic-builder-gui.md#L43)
- Relative object/frame/link references are the intended long-term abstraction:
  [`210-core-dtos-and-errors.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/210-core-dtos-and-errors.md#L375)

## Current Logic Status

### What is correct today

- Robot joint visualization is driven from backend execution snapshots:
  [`manyforge_hmi/server.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_hmi/server.py#L135)
- The execution plane is already async and publishes observer-safe joint-state snapshots.

### What is structurally wrong today

- HMI status text/buttons reflect `CycleManager` state, not backend execution state:
  [`manyforge_hmi/control_panel.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_hmi/control_panel.py#L100)
- Pick poses are hardcoded absolute YAML poses:
  [`examples/pick_and_place.program.yaml`](/home/tndlux/workspaces/dev_ws/src/manyforge/examples/pick_and_place.program.yaml#L60)
- Attach reads object pose from blackboard `scene_objects`, not from backend scene:
  [`manyforge_behavior/nodes/scene.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/nodes/scene.py#L219)
- The runtime object scene shown in Viser is updated through a callback path from behavior-layer
  scene edits:
  [`manyforge_composer/__main__.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_composer/__main__.py#L94)
- Composer currently seeds the planner scene by directly assigning robot joint positions on program
  load:
  [`manyforge_composer/__main__.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_composer/__main__.py#L69)
- The runner builds a shadow object registry from the program YAML:
  [`manyforge_behavior/runner.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/runner.py#L110)

### Practical consequence

Current runtime truth is split:

- robot pose: backend-driven
- object pose for behavior logic: blackboard-driven
- object pose for HMI: callback-driven
- coarse status/buttons: supervisory-state-driven

That split is the root architectural defect behind the attach drift bug.

## ManyMove Takeaways

ManyMove is not a hard reference, but it handles this sequence in a healthier way:

- START/STOP/RESET are operator commands written into a supervisory control plane:
  [`manymove_hmi/README.md`](/home/tndlux/workspaces/dev_ws/src/manymove/manymove_hmi/README.md#L39)
- The behavioral coherence is implemented in the BT/decorator logic itself:
  [`action_nodes_logic.cpp`](/home/tndlux/workspaces/dev_ws/src/manymove/manymove_cpp_trees/src/action_nodes_logic.cpp#L44)
- Reset forces the current branch to fail so the startup sequence reruns and the scene is rebuilt:
  [`tutorial_01.md`](/home/tndlux/workspaces/dev_ws/src/manymove/manymove_cpp_trees/tutorials/tutorial_01.md#L559)
- Pick targets are derived from the current object pose at runtime using `GetObjectPose`, then
  consumed by the move nodes:
  [`bt_client_ur.cpp`](/home/tndlux/workspaces/dev_ws/src/manymove/manymove_cpp_trees/src/bt_client_ur.cpp#L86)
- Attach/detach are object-manager operations, not GUI-side state tricks:
  [`object.hpp`](/home/tndlux/workspaces/dev_ws/src/manymove/manymove_cpp_trees/include/manymove_cpp_trees/object.hpp#L118)

The relevant lesson is not "copy ManyMove". The lesson is:

- control intent may live in the supervisory plane,
- but dynamic runtime facts must be queried from the runtime system before use.

## Target Workflow For ManyForge Pick-And-Place

### 1. Program/startup

- Load the program and deployment.
- Apply the initial scene to the backend.
- Establish runtime observer channels from backend to HMI.
- No HMI-side or blackboard-side runtime scene is authoritative.

### 2. Run

- Run starts or resumes orchestration.
- The behavior tree issues motion/object actions against the backend.
- If the program needs a robot at rest before entering the cycle, that is a motion sequence or
  explicit recovery action, not a direct joint-state assignment.

### 3. Before pick

- Query the current object pose from the backend scene.
- Derive:
  - pick target from the current object pose
  - approach-pick target from the same current object pose
- Use those resolved targets for the current cycle only.

### 4. Attach

- Query the current object pose from the backend.
- Query the current link pose from the backend.
- Compute or delegate the attach relation from those fresh values.
- The backend scene becomes authoritative for the attached object's runtime state.

### 5. Transport

- Robot motion is backend-authoritative.
- Attached object motion is backend-authoritative.
- HMI renders robot and object state from backend observer/query surfaces only.

### 6. Detach

- Detach uses the current backend attached relation and current backend link pose/state.
- The resulting world pose is committed to the backend scene.
- If the program wants to remove or respawn the object later, that is an explicit scene/object step.

### 7. Stop

- Execution backend performs controlled stop/pause semantics on the current trajectory.
- Orchestration halts/suspends the current branch coherently.
- HMI only reflects the resulting backend/orchestrator states.

### 8. Reset

- Reset is a coordinated software-state reset, not a robot teleport.
- In-flight execution is canceled/reset according to kernel/backend contracts.
- Runtime scene/object edits are cleared and the initial scene is reapplied.
- The tree is rebuilt fresh.
- If the desired operator behavior is "return robot to rest", that must be implemented as:
  - an explicit reset-to-rest motion/recovery routine, or
  - a startup sequence that moves from current state to rest before the next cycle begins.

## Architecture Decisions

### A. Backend scene query surface

Required.

The backend/planning adapter must expose a query surface for runtime object state, not just FK for
robot links.

Minimum recommended API:

- `GetObjectPose(object_id, base_frame) -> Pose3DTO`
- `CheckObjectState(object_id) -> {exists, attached, frame_id, ...}` or equivalent

Better long-term form:

- a bounded scene/object observer/query contract for HMI plus behavior-layer use
- object-level polling/query is enough for the demo; a full scene snapshot may come later

### B. Blackboard role

Blackboard should not hold authoritative runtime object poses.

Allowed blackboard uses:

- static metadata
- parameter overrides
- resolved per-cycle targets
- supervisory flags

Not allowed:

- long-lived authoritative runtime scene/object poses
- HMI-owned mirror state that can drift from backend

### C. CycleManager future

`CycleManager` may remain as a supervisory orchestration controller, but only if its scope is
reduced and made explicit:

- it may own tree lifecycle, run/stop/reset intent handling, and parameter plane state
- it must not be presented as the runtime source of truth for robot/scene state
- it should not define execution semantics that properly belong to kernel/backend

If that becomes awkward, replace it with a clearer `RunnerSession`/`ProgramSession` abstraction.

### D. Relative object-based pick logic

Use the long-term form.

The preferred model is:

- `relative_move` / `ReferenceDTO(type=object, id=...)`
- resolution at execute time against the backend-owned scene

This is cleaner than freezing hardcoded world poses or manually copying object poses through the UI.

## Concrete Refactor Plan

## Phase 0: Spec and contract cleanup

Goal: make the target architecture explicit.

- Add a planning-context scene query contract for runtime object state.
- Clarify that HMI live scene visualization must be derived from backend scene/query surfaces.
- Clarify demo scope:
  - either officially allow `scene.edit.attach_detach` for the pick/place demo profile, or
  - mark the demo as using `v0.x`/`v1` scene capabilities.
- Clarify run/stop/reset semantics:
  - reset does not imply robot teleport/home assignment
  - reset rebuilds software/runtime scene state
  - any return-to-rest behavior is explicit

Deliverables:

- spec updates in `manyforge_specs`
- short decision note if the demo is promoted beyond strict `v0.0`

## Phase 1: Backend scene query support

Goal: remove blackboard/YAML as the runtime source of object pose.

- Add backend object-pose query support to the planning adapter.
- Expose it through the native bundle and Python client.
- Keep the API small and explicit.

Likely code areas:

- [`manyforge_core`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_core)
- [`manyforge_roboplan`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_roboplan)
- [`manyforge_behavior/kernel_client.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/kernel_client.py)

Success criteria:

- behavior nodes can query current object pose from backend
- HMI can query current object pose from backend

## Phase 2: Remove runtime scene shadow ownership

Goal: make blackboard runtime object state non-authoritative or remove it.

- Remove `scene_objects` as authoritative runtime pose storage.
- Keep only static metadata if needed:
  - shape type
  - dimensions
  - stable object ids
- If blackboard caching remains, define it as a disposable cache refreshed from backend before use.

Likely code areas:

- [`manyforge_behavior/runner.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/runner.py)
- [`manyforge_behavior/nodes/scene.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/nodes/scene.py)

Success criteria:

- stale blackboard object pose can no longer cause attach/detach errors

## Phase 3: Object-driven pick planning

Goal: make the demo compute pick poses from the current object pose.

Preferred end state:

- implement `relative_move` / `ReferenceDTO(object, id)` resolution at execute time

Interim acceptable path:

- add a `GetObjectPose`-style behavior node that:
  - queries the backend object pose
  - applies configured transforms
  - writes resolved `pick_target` / `approach_pick_target` for the current cycle only

Then update the demo to use those resolved targets instead of fixed pick world poses.

Likely code areas:

- program/schema support
- behavior nodes
- demo YAML in [`examples/pick_and_place.program.yaml`](/home/tndlux/workspaces/dev_ws/src/manyforge/examples/pick_and_place.program.yaml)

Success criteria:

- moving the object before run changes the computed pick target
- the demo no longer assumes a fixed pick pose

## Phase 4: Backend-owned attach/detach

Goal: make attachment semantics depend only on fresh backend state.

- Change attach node so it uses:
  - backend object pose
  - backend link pose
- Better: implement backend-native attach/detach operations and let behavior nodes become thin
  wrappers.
- Detach must derive world pose from the current backend attached relation.

Success criteria:

- attached object never jumps to a stale prior-cycle pose
- detach world pose matches the actual carried pose

## Phase 5: HMI/backend authority cleanup

Goal: eliminate HMI/backend drift.

- Keep HMI robot pose on execution snapshots.
- Add backend-driven object visualization path.
- Demote `scene_edit_callback` from "truth channel" to either:
  - invalidation hint, or
  - optional fast-path that is always reconciled against backend query state
- Make visible status reflect backend status primarily.
- If supervisory state is still shown, label it separately as program/orchestration state.

Success criteria:

- robot and objects rendered in HMI match backend runtime state
- HMI cannot display object state that backend does not currently own

## Phase 6: Run/Stop/Reset refactor

Goal: make component responsibilities explicit and future-proof for real robot control.

Define the contract per layer:

- Execution backend:
  - owns real/simulated trajectory progression
  - reacts to stop/pause/reset according to execution semantics
- Kernel:
  - owns state machine and execution correctness
- Orchestrator / BT:
  - owns branch traversal, retries, and scene-reset/startup sequencing
- HMI:
  - sends operator intent
  - reflects resulting backend/orchestrator state

Important rule:

- no direct assignment of robot state as a reset mechanism in the long-term runtime path
- simulator-only bootstrapping must be clearly isolated from runtime reset semantics

Concrete cleanup:

- isolate or remove direct `set_joint_positions(...)` bootstrapping from the normal runtime control
  path
- if simulation needs an initial pose bootstrap, make it an explicitly labeled simulator-only setup
  step, not part of reset semantics

Evaluate whether to:

- keep `CycleManager` with reduced scope, or
- replace it with a cleaner session controller

## Phase 7: Demo/profile cleanup

Goal: make the demo honest and reproducible.

- Update the pick-and-place demo to the final object-driven logic.
- Ensure reset reruns a fresh scene/object cycle.
- Align demo claims with actual supported profile/capabilities.

## Acceptance Criteria

The refactor is complete when all of the following are true:

- HMI never uses non-authoritative object pose as runtime truth.
- Attaching an object after reset or repeated cycles always uses the current backend object pose.
- The object does not jump to a previous drop pose on attach.
- Pick targets are resolved from current object pose, not hardcoded.
- Reset rebuilds runtime scene state, but does not teleport the robot.
- A future real controller can replace the simulated execution backend without changing these
  ownership rules.
- The operator-visible runtime status is backend-first, with supervisory state separated if shown.

## Recommended Implementation Order

1. Phase 0: spec/contract clarification
2. Phase 1: backend object query API
3. Phase 2: remove runtime scene shadow ownership
4. Phase 4: backend-owned attach/detach freshness
5. Phase 3: object-driven pick planning
6. Phase 5: HMI/backend authority cleanup
7. Phase 6: run/stop/reset lifecycle cleanup
8. Phase 7: demo/profile cleanup

This order fixes the authority bug before redesigning the demo.

## Immediate Next Step

Before code changes, decide and document one point explicitly:

- whether the demo path should introduce an interim `GetObjectPose` behavior node first,
  or go straight to `relative_move` / `ReferenceDTO(object, id)` support.

Given the desired long-term architecture, the preferred answer is:

- go straight to `relative_move` / `ReferenceDTO(object, id)` if the implementation scope is
  acceptable,
- otherwise introduce `GetObjectPose` only as a short-lived migration step.
