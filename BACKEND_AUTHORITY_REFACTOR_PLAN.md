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

## Current Status

Status as of 2026-04-07:

- Phase 0 is complete.
- Phase 1 is complete.
- Phase 2 is complete.
- Phase 3 is complete.
- Phase 4 is complete.
- Phase 5 is complete.
- Phase 6 is complete.
- Phase 7 is complete.
- The backend-authoritative **read path** for runtime object state now exists end-to-end.
- The behavior layer now resolves attach/detach state from backend object/link state rather than
  blackboard runtime pose.
- The pick-and-place demo now resolves pick approach and pick targets at execute time through
  `relative_move` / `ReferenceDTO(object, id)`.
- Attach/detach are now backend-native scene operations with stable object ids.
- The HMI now reconciles object rendering from backend object state/pose queries and shows
  backend-first execution status.
- Cycle lifecycle preparation is now explicit:
  - `Reset` rebuilds runtime scene/tree state even from `IDLE`
  - `Run` from `IDLE` prepares a fresh cycle when the previous tree already completed
  - reset semantics remain independent from any direct robot-state assignment
- The demo/operator path is now aligned with the object-relative `v0.1` profile:
  - `scripts/demo.sh` loads a real `v0.1` deployment artifact
  - simulated native startup uses an explicit deployment-scoped initial-joint bootstrap
  - first-run and rerun operator flow have been qualified through Composer + Viser

Latest verification snapshot:

- Host Python:
  - `manyforge_behavior/tests/test_capabilities.py manyforge_behavior/tests/test_deployment.py -q`
    passed (`38 passed`)
  - `manyforge_behavior/tests/test_nodes.py` passed (`23 passed`)
  - `manyforge_behavior/tests/test_kernel_client.py` passed
  - `manyforge_behavior/tests` passed (`210 passed`)
  - `tests/test_pick_and_place.py` passed (`14 passed`)
  - `manyforge_behavior/tests/test_cycle_manager.py -q` passed (`17 passed`)
  - `manyforge_core` focused `ExecutionKernel|ContractsCompile` tests passed (`79/79`)
  - `manyforge_hmi/tests/test_hmi.py -q` passed (`30 passed, 2 skipped`)
- Dev container:
  - `./scripts/test-all.sh bindings` passed (`41 passed`)
  - `./scripts/test-all.sh cpp` passed
  - `./scripts/test-all.sh roboplan` passed (`223/223`)
  - `pytest manyforge_roboplan/test/test_native_e2e.py -q -k set_joint_positions_refreshes_idle_execution_snapshot`
    passed (`1 passed, 28 deselected`) when run against the rebuilt native module
  - `pytest manyforge_roboplan/test/test_native_e2e.py -q -k relative_move` passed (`2 passed`)
  - `pytest manyforge_roboplan/test/test_native_e2e.py -q` passed (`28 passed`)
  - `pytest manyforge_hmi/tests/test_hmi.py -q` passed with no skips (`31 passed`)
  - `pytest manyforge_composer/tests/test_backend.py -q` passed (`30 passed`)
  - `scripts/demo.sh` live qualification passed:
    - `/api/program/load` returned `200`
    - Composer health reported `program=loaded`
    - first `run` executed `idle -> running -> idle`
    - `reset` from `idle` succeeded
    - second `run` executed the same cycle again without the old immediate planner failure

Interpretation:

- The backend object query API is stable enough to build on.
- The consumer-side runtime object ownership defect has been removed from attach/detach logic.
- The demo no longer depends on fixed world-frame pick poses.
- Attach/detach frame transitions are now backend-native rather than behavior-computed.
- HMI runtime object rendering and visible primary status are now backend-first.
- Lifecycle preparation semantics are explicit and remain independent from simulator-only bootstrap.
- The simulated native demo now uses an explicit deployment-scoped initial-joint bootstrap for
  reproducible startup, without reintroducing teleport semantics into `Reset`.
- The main planned authority/lifecycle refactor is complete; remaining work is normal follow-up
  hardening or product evolution rather than this refactor program.

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
- Attach/detach behavior logic now resolves runtime object pose from backend queries rather than
  blackboard runtime state:
  [`manyforge_behavior/nodes/scene.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/nodes/scene.py#L213)
- HMI object rendering now reconciles from backend object state/pose queries:
  [`manyforge_hmi/server.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_hmi/server.py#L124)
- The control panel now shows backend-first execution state from the latest execution snapshot:
  [`manyforge_hmi/control_panel.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_hmi/control_panel.py#L95)

### What is structurally wrong today

- The runner still builds a static object metadata registry from the program YAML:
  [`manyforge_behavior/runner.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/runner.py#L110)
- The simulated native demo still relies on an explicit deployment-scoped initial joint bootstrap
  to place the virtual robot in a valid start configuration before operator use:
  [`manyforge_composer/__main__.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_composer/__main__.py#L100)
  This is acceptable for the simulated demo profile, but it remains intentionally segregated from
  reset semantics and should not be generalized to real-robot runtime behavior.

### Practical consequence

Current runtime truth is split:

- robot pose: backend-driven
- object pose for attach/detach behavior logic: backend-driven
- pick approach/pick targets: backend-resolved at execute time
- object pose for HMI: backend-driven
- visible primary status: backend-driven

The original attach drift root cause and the main HMI authority split have been removed. The
remaining open questions are product-level evolution questions, not unresolved ownership bugs in
the current demo path.

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

Status: completed on 2026-04-07

Phase 0 decisions recorded in specs:

- Added backend-authoritative runtime object query surfaces to the planning-context contract:
  - `GetObjectPose(object_id, base_frame) -> Pose3DTO`
  - `GetObjectState(object_id) -> ObjectStateDTO`
- Added `ObjectStateDTO` to the core DTO spec for object existence / attachment / frame identity.
- Clarified that live runtime robot/object visualization in HMI/Composer must be derived from
  backend-authoritative query/snapshot surfaces, never from callback-only or blackboard-only state.
- Clarified that UI callbacks, blackboard mirrors, and local caches may exist only as invalidation
  hints or short-lived render caches and must be reconciled against backend state before becoming
  operator-visible runtime truth.
- Added an explicit additive `v0.1` capability profile for backend-authoritative object-relative
  manipulation demos. The reference pick-and-place demo that derives pick poses from current object
  pose and relies on attach/detach is therefore treated as `v0.1`, not strict `v0.0`.
- Clarified Run/Stop/Reset semantics:
  - they are supervisory/operator commands
  - reset rebuilds software/runtime scene state
  - reset does not imply robot teleport or implicit return-to-rest
  - any return-to-rest behavior must be an explicit motion/recovery sequence

Primary spec references for Phase 0 outcome:

- [`210-core-dtos-and-errors.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/210-core-dtos-and-errors.md#L245)
- [`220-core-ports-and-threading.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/220-core-ports-and-threading.md#L63)
- [`240-supported-backend-capability-profile.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/240-supported-backend-capability-profile.md#L122)
- [`240-supported-backend-capability-profile.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/240-supported-backend-capability-profile.md#L258)
- [`400-program-model-and-artifacts.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/400-program-model-and-artifacts.md#L219)
- [`400-program-model-and-artifacts.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/400-program-model-and-artifacts.md#L347)
- [`500-observability-tracing-and-performance.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/500-observability-tracing-and-performance.md#L32)
- [`ADR-0014-orchestration-tooling-logic-builder-gui.md`](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/adr/ADR-0014-orchestration-tooling-logic-builder-gui.md#L43)

## Phase 1: Backend scene query support

Goal: remove blackboard/YAML as the runtime source of object pose.

- Add backend object-pose query support to the planning adapter.
- Expose it through the native bundle and Python client.
- Keep the API small and explicit.

Decision locked on 2026-04-07:

- Phase 1 will implement the **long-term read path** now, but with a deliberately narrow surface.
- The runtime backend query API will be **read-only**, **backend-authoritative**, and
  **object-focused**.
- Phase 1 will **not** introduce a generic full-scene mirror/snapshot API for HMI consumption.
- Phase 1 will **not** attempt to keep blackboard runtime object poses auto-synchronized.
- Phase 1 will **not** refactor `CycleManager`; that remains a later-phase ownership/lifecycle
  decision.

Required Phase 1 surface:

- `get_object_pose(object_id, base_frame) -> Pose3DTO`
- `get_object_state(object_id) -> ObjectStateDTO`

Minimum `ObjectStateDTO` fields relied on by the refactor:

- `object_id`
- `exists`
- `attached`
- `frame_id`

Phase 1 design rationale:

- **Reliability:** behavior nodes and HMI obtain the same object-state facts from the same
  backend-owned source.
- **Stability:** the API surface is small, explicit, and easy to test and preserve.
- **Flexibility:** it is enough for object-driven picking, fresh attach/detach logic, and
  backend-synchronized HMI rendering, while remaining compatible with a future broader
  scene-snapshot contract if needed.

Explicit non-goals for Phase 1:

- no full scene mirror API
- no UI-specific backend contract
- no callback-driven truth channel
- no blackboard-owned runtime object pose
- no demo YAML redesign yet
- no run/stop/reset lifecycle refactor yet

Likely code areas:

- [`manyforge_core`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_core)
- [`manyforge_roboplan`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_roboplan)
- [`manyforge_behavior/kernel_client.py`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/kernel_client.py)

Success criteria:

- behavior nodes can query current object pose from backend
- HMI can query current object pose from backend
- the same object query path is shared by behavior and HMI
- no new runtime-authoritative object cache is introduced outside the backend

Status: completed on 2026-04-07

Phase 1 implementation outcome:

- Added `ObjectStateDTO` and object-query methods to the C++ `IPlanningContext` contract in the
  codebase.
- Implemented backend-owned object query support in
  [`manyforge_roboplan`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_roboplan):
  - object state is tracked inside `RoboPlanPlanningContext`
  - object pose queries resolve through backend-owned frame/FK state
  - frame-relative objects (e.g. objects attached to `tool0`) resolve through the backend, not
    through blackboard or HMI state
- Exposed the same read-only query path through:
  - native pybind bundle
  - [`NativeKernelClient`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_roboplan/python/native_kernel_client.py)
  - [`KernelClient`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/kernel_client.py)
  - [`CycleManager`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_behavior/manyforge_behavior/cycle_manager.py)
  - [`StateBridge`](/home/tndlux/workspaces/dev_ws/src/manyforge/manyforge_hmi/state_bridge.py)
- The current adapter implementation keeps runtime authority in the backend by updating object
  state only from backend scene-edit operations. No new blackboard-owned runtime object cache was
  introduced.

Phase 1 verification:

- Host Python:
  - `manyforge_behavior/tests/test_kernel_client.py`
  - `manyforge_hmi/tests/test_hmi.py`
- Dev container:
  - `./scripts/test-all.sh cpp`
  - `./scripts/test-all.sh roboplan`
  - `./scripts/test-all.sh bindings`
  - `pytest manyforge_roboplan/test/test_native_e2e.py -q`

Notes / intentional limits:

- Phase 1 establishes the authoritative read path only.
- It does **not** yet remove blackboard runtime object pose usage from attach/detach nodes.
- It does **not** yet make the HMI consume backend object queries for live object rendering.
- Those are Phase 2 and Phase 5 concerns.

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

Immediate implementation order for Phase 2:

1. Reproduce the current drift with a focused behavior-layer test.
2. Change attach/detach nodes to query backend object pose/state before computing transforms.
3. Reduce blackboard `scene_objects` to static metadata only:
   - shape type
   - dimensions
   - stable object id / nominal frame metadata if needed
4. Ensure attach/detach no longer write authoritative runtime object pose back into blackboard.
5. Re-run pick-and-place attach/detach tests plus the live demo path.

Phase 2 definition of done:

- attach uses fresh backend object pose and fresh backend link pose
- detach derives world pose from backend-owned current attached relation / frame state
- blackboard no longer owns runtime object pose truth
- repeated cycle runs cannot reintroduce stale object pose through behavior-layer state reuse

Status: completed on 2026-04-07

Phase 2 implementation outcome:

- `AttachObjectToLink` now reads:
  - static object shape metadata from blackboard
  - current object pose from backend `get_object_pose(object_id, base_frame)`
  - current link pose from backend `get_link_pose(link_frame, base_frame)`
- `DetachObjectFromLink` now reads:
  - static object shape metadata from blackboard
  - attached-object existence from backend `get_object_state(object_id)`
  - current attached-object world pose from backend `get_object_pose(object_id, "universe")`
- Attach/detach no longer write authoritative runtime object pose back into blackboard.
- The runner now seeds `scene_objects` as static metadata only:
  - `shape_type`
  - `box_dimensions_m`
  - `sphere_radius_m`
- The `MockKernelClient` now resolves object poses relative to non-world frames so behavior-layer
  tests exercise the same authority boundary as the real backend path.

Phase 2 verification:

- Focused regression tests:
  - attach ignores stale blackboard pose and uses backend object pose
  - detach succeeds without attached-object runtime pose in blackboard
- Host Python:
  - `pytest manyforge_behavior/tests/test_nodes.py -q` (`22 passed`)
  - `pytest manyforge_behavior/tests -q` (`208 passed`)
  - `pytest tests/test_pick_and_place.py -q` (`14 passed`)
- `git diff --check` passed

Notes / intentional limits:

- Phase 2 fixes the behavior-layer root cause of the observed attach drift.
- Phase 2 does **not** yet make pick targets object-relative; the demo still uses fixed pick poses.
- Phase 2 does **not** yet make HMI object rendering backend-passive; the scene-edit callback path
  still exists and should be treated as a later cleanup item.

## Phase 3: Object-driven pick planning

Goal: make the demo compute pick poses from the current object pose.

Preferred end state:

- implement `relative_move` / `ReferenceDTO(object, id)` resolution at execute time

Likely code areas:

- program/schema support
- behavior nodes
- demo YAML in [`examples/pick_and_place.program.yaml`](/home/tndlux/workspaces/dev_ws/src/manyforge/examples/pick_and_place.program.yaml)

Success criteria:

- moving the object before run changes the computed pick target
- the demo no longer assumes a fixed pick pose

Status: completed on 2026-04-07

Phase 3 implementation outcome:

- Added execute-time backend reference resolution to the core planning path through
  `IPlanningContext::resolve_reference_pose(...)`.
- Implemented backend resolution for:
  - `ReferenceDTO(type=object, id=...)`
  - `ReferenceDTO(type=frame, id=...)`
  - `ReferenceDTO(type=robot_link, id=...)`
- The kernel now resolves `RelativeMoveDTO` into a concrete `PoseGoalDTO` immediately before
  planning, so the planner always consumes backend-current reference state.
- Native bindings and the Python client now support `motion_type: relative_move`.
- The node catalog now exposes `relative_move` for `move_manipulator_action`.
- The pick-and-place demo now uses backend-resolved `relative_move` for:
  - `move_to_pick_approach`
  - `move_to_pick`
- The demo profile was promoted to `v0.1` to reflect object-relative manipulation support.

Phase 3 verification:

- Host:
  - `pytest manyforge_behavior/tests/test_nodes.py -q` (`23 passed`)
  - `pytest manyforge_behavior/tests -q` (`209 passed`)
  - `ctest --test-dir /tmp/manyforge_core_phase3 --output-on-failure -R "ExecutionKernel|ContractsCompile"`
    (`79/79 passed`)
- Dev container:
  - `./scripts/test-all.sh bindings` (`41 passed`)
  - `./scripts/test-all.sh roboplan` (full suite passed)
  - `pytest manyforge_roboplan/test/test_native_e2e.py -q -k relative_move` (`2 passed`)
  - `pytest manyforge_roboplan/test/test_native_e2e.py -q` (`26 passed`)

Phase 3 definition of done:

- the demo no longer encodes fixed world-frame pick targets
- object-relative references are resolved from backend-current scene state at execute time
- moving the object before run changes the resolved pick target without changing the YAML

## Phase 4: Backend-native attach/detach operations

Goal: make attach/detach operations backend-native rather than behavior-computed scene rewrites.

- Keep the Phase 2 freshness guarantees.
- Move attach/detach transform ownership into the backend/planning adapter.
- Let behavior nodes become thin request wrappers around backend-native attach/detach commands.
- Detach world pose must still derive from current backend attached relation.

Success criteria:

- behavior nodes no longer compute the authoritative attach transform themselves
- attached object world/relative pose is owned entirely by backend state transitions
- detach world pose matches the actual carried pose

Status: completed on 2026-04-07

Phase 4 implementation outcome:

- Extended the scene-edit contract with backend-native:
  - `attach(object_id, frame_id)`
  - `detach(object_id, frame_id)`
- `IPlanningContext` now exposes `attach_object(...)` and `detach_object(...)`.
- `RoboPlanPlanningContext` now performs attach/detach as backend-owned reparent operations that:
  - preserve current world pose
  - update the object's parent frame inside backend state
  - keep a stable object id rather than renaming to `*_attached`
- The behavior attach/detach nodes are now thin wrappers around those backend operations.
- Behavior no longer computes the authoritative attach transform; it only emits post-op
  remove/add callback hints for the current transitional HMI path.
- The mock kernel and native Python binding path now implement the same attach/detach contract.

Phase 4 verification:

- Host:
  - `pytest manyforge_behavior/tests/test_kernel_client.py manyforge_behavior/tests/test_nodes.py tests/test_pick_and_place.py -q`
    (`58 passed`)
  - `pytest manyforge_behavior/tests -q` (`210 passed`)
  - `ctest --test-dir /tmp/manyforge_core_phase4 --output-on-failure -R "ExecutionKernel|ContractsCompile"`
    (`79/79 passed`)
- Dev container:
  - `./scripts/test-all.sh bindings` (`41 passed`)
  - `./scripts/test-all.sh roboplan` (`223/223 passed`)
  - `pytest manyforge_roboplan/test/test_native_e2e.py -q` (`28 passed`)

Phase 4 definition of done:

- behavior nodes no longer own the attach/detach transform
- stable object ids survive attach/detach transitions
- attach/detach preserve world pose through backend-native frame transitions
- the contract is implemented consistently in mock, native binding, and RoboPlan backend

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

Status: completed on 2026-04-07

Phase 5 implementation outcome:

- `ViserHmiServer` now owns backend-passive object reconciliation:
  - static object metadata comes from the loaded program
  - runtime existence/frame/pose comes from backend queries via `StateBridge`
  - object rendering is synchronized from backend state in the update loop
- `SceneRenderer` now has id-stable sync helpers so HMI updates only when authoritative object
  render state changes.
- `ControlPanel` now shows backend-first execution status derived from the latest execution
  snapshot instead of presenting `CycleManager` state as the primary runtime truth.
- Composer and demo `scene_edit_callback` paths were demoted from rendering channels to scene-sync
  invalidation hints; object rendering is now reconciled from backend state instead of callback
  payloads.
- `run from idle` HMI refresh now clears the scene and resyncs from backend state instead of
  reloading object poses directly from program YAML.

Phase 5 verification:

- Host:
  - `pytest manyforge_hmi/tests/test_hmi.py -q` (`29 passed, 2 skipped`)
- Dev container:
  - `pytest manyforge_hmi/tests/test_hmi.py -q` (`31 passed`)
  - `pytest manyforge_composer/tests/test_backend.py -q` (`29 passed`)

Phase 5 definition of done:

- robot pose rendering remains driven by backend execution snapshots
- object rendering is reconciled from backend object state/pose queries
- visible primary execution status is backend-first
- callback payloads no longer act as the authoritative object-rendering path

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

- isolate direct `set_joint_positions(...)` bootstrapping from the normal runtime control path
- if simulation needs an initial pose bootstrap, make it an explicitly labeled simulator-only setup
  step, not part of reset semantics

Evaluate whether to:

- keep `CycleManager` with reduced scope, or
- replace it with a cleaner session controller

Status: completed on 2026-04-07

Phase 6 implementation outcome:

- `CycleManager` now treats lifecycle preparation as explicit software/runtime state management:
  - `reset()` rebuilds tree + runtime scene even from `IDLE`
  - `reset()` no longer depends on synthetic robot-state assignment
  - `run()` from `IDLE` prepares a fresh cycle when the previous tree already reached a terminal
    state
- Runtime scene cleanup during reset/run no longer assumes authoritative `*_attached` object ids.
- Composer and the standalone pick-and-place demo no longer use a hardcoded rest-pose injection as
  part of normal startup. Later demo/profile cleanup replaced that with an explicit
  deployment-scoped simulator bootstrap step, preserving the same separation from reset semantics.
- `ViserHmiServer` now has an immediate backend-robot sync path so startup/bootstrap uses backend
  execution snapshots rather than a synthetic UI pose.
- Composer program-load wiring now prepares a fresh cycle through `CycleManager.reset()` and then
  synchronizes HMI scene/robot views from backend state.
- The reset control is now exposed in all cycle states so operators can rebuild runtime scene/tree
  state after completion without implying robot teleport semantics.

Phase 6 verification:

- Host:
  - `pytest manyforge_behavior/tests/test_cycle_manager.py -q` (`17 passed`)
  - `pytest manyforge_hmi/tests/test_hmi.py -q` (`30 passed, 2 skipped`)
- Dev container:
  - `pytest manyforge_composer/tests/test_backend.py -q` (`29 passed`)

Phase 6 definition of done:

- no normal runtime path injects a hardcoded robot rest pose into backend state
- `Reset` is meaningful from `IDLE` and rebuilds software/runtime cycle state
- completed-tree reruns from `IDLE` do not reuse terminal tree state
- HMI robot bootstrap is backend-driven rather than a direct UI pose assignment
- lifecycle preparation remains compatible with future real-robot control semantics

## Phase 7: Demo/profile cleanup

Goal: make the demo honest and reproducible.

- Update the pick-and-place demo to the final object-driven logic.
- Ensure reset reruns a fresh scene/object cycle.
- Align demo claims with actual supported profile/capabilities.

Status: completed on 2026-04-07

Phase 7 implementation outcome:

- Added a real example deployment artifact for the pick-and-place demo:
  - `examples/pick_and_place.deployment.yaml`
- Added code-side `v0.1` capability requirements so deployment/program validation matches the
  object-relative demo profile.
- Updated `scripts/demo.sh` so it:
  - loads the correct `v0.1` deployment
  - passes well-formed JSON into `/api/program/load`
  - fails honestly if startup/load fails
- Added an explicit simulated-native startup bootstrap driven by deployment config:
  - `execution.config.initial_joint_positions_rad`
  - applied only during simulated program-load/bootstrap
  - propagated into the published backend snapshot immediately after scene assignment
- Verified that the live Composer + Viser path now:
  - loads successfully
  - plans the first cycle from the configured simulated start state instead of invalid zero joints
  - supports `idle -> run -> idle -> reset -> run -> idle`

Phase 7 verification:

- Host:
  - `pytest manyforge_behavior/tests/test_capabilities.py manyforge_behavior/tests/test_deployment.py -q`
    (`38 passed`)
- Dev container:
  - `pytest manyforge_composer/tests/test_backend.py -q` (`30 passed`)
  - rebuilt native-module check:
    - `pytest manyforge_roboplan/test/test_native_e2e.py -q -k set_joint_positions_refreshes_idle_execution_snapshot`
      (`1 passed, 28 deselected`)
  - live operator path:
    - `./scripts/demo.sh`
    - verified successful `/api/program/load`
    - verified first run completes
    - verified reset from idle
    - verified second run completes

Phase 7 definition of done:

- demo program/deployment profile pairing is honest and validated
- live operator path is reproducible from a clean startup
- simulated bootstrap is explicit, deployment-scoped, and kept out of reset semantics
- first-run and rerun qualification are green through Composer + Viser

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
4. Phase 3: object-driven pick planning
5. Phase 4: backend-native attach/detach operations
6. Phase 5: HMI/backend authority cleanup
7. Phase 6: run/stop/reset lifecycle cleanup
8. Phase 7: demo/profile cleanup

This order establishes backend authority for object state, pick resolution, and attach/detach
before tackling the remaining operator-visible HMI authority split.

## Immediate Next Step

Planned refactor phases are complete.

Immediate follow-up options, if needed:

- capture operator-facing documentation/screenshots for the new `v0.1` demo workflow
- add broader non-regression coverage around demo startup/bootstrap if the simulated profile
  continues to evolve
- evaluate longer-term replacement or narrowing of `CycleManager` as a separate architectural track
