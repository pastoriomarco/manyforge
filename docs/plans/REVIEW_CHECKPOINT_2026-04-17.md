# Review Checkpoint: 2026-04-17

This document records the full milestone that landed after commit `68955d2`
(`finalize typed blackboard bundle runtime`) and before the next signal-adapter
/ ROS / PLC workstream.

It exists for engineering review. It is not a forward-looking plan. It is the
checkpoint of what was actually added, what was intentionally deferred, and what
validation justified merging the milestone.

## Scope Added In This Milestone

### 1. Typed blackboard bundle runtime is now the authoritative model

- Namespace outputs publish one root bundle instead of parallel
  `response` / `status` / `confidence` / `fields/...` keys.
- Runtime bindings stay typed through:
  - `namespace_ref`
  - `key_ref`
  - `field_ref`
- Raw blackboard reference strings are no longer an accepted authored/runtime
  mechanism.
- Recovery/status-style outputs were aligned to the bundle model.

Primary implementation surfaces:

- [`manyforge_behavior/manyforge_behavior/blackboard_namespace.py`](../../manyforge_behavior/manyforge_behavior/blackboard_namespace.py)
- [`manyforge_behavior/manyforge_behavior/blackboard_bindings.py`](../../manyforge_behavior/manyforge_behavior/blackboard_bindings.py)
- [`manyforge_behavior/manyforge_behavior/parameters.py`](../../manyforge_behavior/manyforge_behavior/parameters.py)
- [`manyforge_behavior/manyforge_behavior/program.py`](../../manyforge_behavior/manyforge_behavior/program.py)
- [`manyforge_behavior/manyforge_behavior/nodes/model.py`](../../manyforge_behavior/manyforge_behavior/nodes/model.py)
- [`manyforge_behavior/manyforge_behavior/nodes/selection.py`](../../manyforge_behavior/manyforge_behavior/nodes/selection.py)
- [`manyforge_behavior/manyforge_behavior/nodes/pick_planning.py`](../../manyforge_behavior/manyforge_behavior/nodes/pick_planning.py)
- [`manyforge_behavior/manyforge_behavior/nodes/recovery.py`](../../manyforge_behavior/manyforge_behavior/nodes/recovery.py)

### 2. Signal and gripper v1 substrate landed

- Added signal-oriented nodes:
  - `check_signal_bool`
  - `set_signal_bool`
  - `wait_for_signal_bool`
  - `wait_for_signal_edge`
- Added gripper-state nodes:
  - `check_gripper_state`
  - `set_gripper_state`
  - `wait_for_gripper_state`
- Migrated core authored pick/place flows to the new application-level gripper
  abstraction instead of generic bool-key writes.

Primary implementation surfaces:

- [`manyforge_behavior/manyforge_behavior/nodes/logic.py`](../../manyforge_behavior/manyforge_behavior/nodes/logic.py)
- [`manyforge_behavior/manyforge_behavior/runner.py`](../../manyforge_behavior/manyforge_behavior/runner.py)
- [`manyforge_behavior/resources/node_catalog.yaml`](../../manyforge_behavior/resources/node_catalog.yaml)
- [`manyforge_behavior/resources/skills/pick_object.skill.yaml`](../../manyforge_behavior/resources/skills/pick_object.skill.yaml)
- [`manyforge_behavior/resources/skills/place_object.skill.yaml`](../../manyforge_behavior/resources/skills/place_object.skill.yaml)

### 3. Interrupt runtime v1 landed

- Deployment artifacts can declare `interrupt_sources`.
- Program artifacts can declare:
  - `interrupt_handlers`
  - `interrupt_policies`
- Catalog entries expose interruptibility metadata.
- `CycleManager` now:
  - detects and latches runtime interrupts
  - arbitrates by priority
  - injects `_interrupt/current` reserved context
  - can auto-execute conservative handler trees with bounded resource claims
  - exposes pending interrupt state to control/observability surfaces
- Runtime uses a dedicated `interrupt_handler` execution state.

Primary implementation surfaces:

- [`manyforge_behavior/manyforge_behavior/deployment.py`](../../manyforge_behavior/manyforge_behavior/deployment.py)
- [`manyforge_behavior/manyforge_behavior/program.py`](../../manyforge_behavior/manyforge_behavior/program.py)
- [`manyforge_behavior/manyforge_behavior/catalog.py`](../../manyforge_behavior/manyforge_behavior/catalog.py)
- [`manyforge_behavior/manyforge_behavior/cycle_manager.py`](../../manyforge_behavior/manyforge_behavior/cycle_manager.py)
- [`manyforge_behavior/manyforge_behavior/runner.py`](../../manyforge_behavior/manyforge_behavior/runner.py)

### 4. Observability / traceability was strengthened

- Execution events now carry a richer, stable envelope with:
  - event index
  - severity
  - component
  - schema version
  - trace source
  - run identifiers
  - intervention identifiers
- Added interrupt-specific event projection and digest visibility.
- Added normalized `ros2_medkit`-style trace export.
- Execution history filtering gained:
  - time range
  - severity
  - component
  - event type
  - execution/run identifiers

Primary implementation surfaces:

- [`manyforge_composer/backend/event_recorder.py`](../../manyforge_composer/backend/event_recorder.py)
- [`manyforge_composer/backend/routes_execution.py`](../../manyforge_composer/backend/routes_execution.py)
- [`manyforge_composer/backend/routes_debug.py`](../../manyforge_composer/backend/routes_debug.py)
- [`manyforge_composer/backend/models.py`](../../manyforge_composer/backend/models.py)

### 5. Runtime write/edit guards were tightened

- Scene edits are blocked while execution is active, including
  `interrupt_handler`.
- Blocking responses now expose explicit machine-readable cycle state.
- MCP scene-edit behavior is aligned with the REST route.
- Program/deployment mutation routes enforce active-execution restrictions more
  consistently.

Primary implementation surfaces:

- [`manyforge_composer/backend/routes_scene.py`](../../manyforge_composer/backend/routes_scene.py)
- [`manyforge_composer/backend/mcp_server.py`](../../manyforge_composer/backend/mcp_server.py)
- [`manyforge_composer/backend/routes_program.py`](../../manyforge_composer/backend/routes_program.py)
- [`manyforge_composer/backend/state.py`](../../manyforge_composer/backend/state.py)

## Decisions Taken And Intentionally Kept

These were deliberate decisions, not unfinished accidents.

### Signal model v1

- Signals are currently represented through declared typed blackboard keys plus
  signal-specific nodes.
- A separate top-level `signals:` artifact was deferred.

Reason:

- it keeps the authored/runtime model aligned with the already-stabilized typed
  blackboard system
- it avoids inventing a second symbol registry before the external signal
  adapter layer exists

### Gripper abstraction v1

- Gripper handling is currently a logical-state abstraction plus explicit scene
  attach/detach authority.
- Pluggable hardware-driver abstraction and dedicated URDF packaging were
  deferred.

Reason:

- this gives application authors an immediate and explicit gripper-oriented
  surface
- it avoids prematurely freezing the end-effector packaging contract before real
  adapter requirements are known

### Interrupt auto-execution scope

- Runtime auto-executes only conservative handler claims:
  - `blackboard`
  - `signals`
  - `gripper`
  - `planner`
  - `timers`
- Runtime does not auto-execute handlers that claim:
  - `motion`
  - `scene`
  - `model`

Reason:

- fail-safe behavior is preferable to optimistic concurrent authority in an
  industrial runtime
- this keeps the first implementation deterministic and auditable

## Deferred Review Items

These items were intentionally left for later review and are still open.

### Interrupt runtime

- nested or stacked interrupts
- full handler subtree execution for `motion` / `scene` / `model`
- richer restart semantics beyond the current conservative dispositions
- broader kernel/adapter hold/abort contracts
- explicit timeout policy for long-running or stalled handlers

### External control integration

- generic signal adapter/runtime registry
- stale-source and disconnect semantics for external signal providers
- ROS2-facing signal wrappers
- PLC/ROXSIE-specific adapter implementation after connector contract review

### End-effector structure

- pluggable/changeable end-effector URDF packaging
- driver abstraction and deployment-facing end-effector metadata

## Validation Completed Before This Checkpoint

Validation was done in the Docker `dev` container, not just on the host.

Full command:

```bash
docker compose run --rm dev bash -lc './scripts/test-all.sh all'
```

Result at checkpoint time:

- all 8 container suites passed

Notable suite summaries:

- `manyforge_core` C++ tests: all passed
- `manyforge_roboplan` C++ tests: all passed
- pybind bindings tests: all passed
- `manyforge_behavior` Python tests: `390 passed, 1 skipped`
- `manyforge_hmi` tests: `40 passed`
- integration tests: `39 passed`
- Composer backend tests: `266 passed`
- Composer frontend production build: passed

Additional focused coverage landed for:

- interrupt handler lifecycle
- interrupt observability export
- scene-edit blocking during `RUNNING`
- scene-edit blocking during `INTERRUPT_HANDLER`
- MCP alignment for interrupt-handler edit blocking

## Review Focus For The Next Pass

If this milestone is being reviewed, the highest-value review questions are:

1. Is the interrupt runtime conservative enough for industrial deployment?
2. Are the signal/gripper v1 abstractions the right substrate before ROS/PLC
   adapters are introduced?
3. Is the event/trace envelope sufficient for `ros2_medkit` correlation?
4. Are the current active-execution mutation guards explicit enough across REST,
   MCP, and UI surfaces?
5. Does the next workstream belong at the signal-adapter/runtime-registry layer
   before any ROXSIE-specific implementation?
