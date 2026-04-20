# Review Checkpoint: 2026-04-20

This document records the milestone that landed after commit `5e5af09`
(`harden interrupt runtime and upgrade roboplan`) and before the next
ROS/PLC-adapter workstream.

It exists for engineering review. It is not a forward-looking plan. It is the
checkpoint of what was actually added, what was intentionally deferred, and
what validation justified merging the milestone.

## Scope Added In This Milestone

### 1. Qualification system became a real repo capability

- Added a scenario runner driven through the real Composer HTTP API.
- Added deterministic qualification matrices and wrapper scripts.
- Added native live-evaluation scenarios on the real RoboPlan/native kernel.
- Added soak/liveness qualification batches.
- Added retained artifact capture and archive support.

Primary implementation surfaces:

- [`manyforge_composer/verification/runner.py`](../../manyforge_composer/verification/runner.py)
- [`manyforge_composer/tests/test_scenario_runner.py`](../../manyforge_composer/tests/test_scenario_runner.py)
- [`scripts/run-scenarios.py`](../../scripts/run-scenarios.py)
- [`scripts/qualify-scenarios.sh`](../../scripts/qualify-scenarios.sh)
- [`scripts/qualify-native-scenarios.sh`](../../scripts/qualify-native-scenarios.sh)
- [`scripts/qualify-soak-scenarios.sh`](../../scripts/qualify-soak-scenarios.sh)
- [`scripts/test-all.sh`](../../scripts/test-all.sh)
- [`verification/scenarios/`](../../verification/scenarios/)
- [`verification/matrices/`](../../verification/matrices/)

### 2. Industrial-readiness and verification docs were formalized

- Added a standalone industrial-readiness gap register.
- Added the verification plan for scenario-driven qualification.
- Added a formal HMI/browser qualification checklist.
- Cross-linked product-direction and verification docs to the gap register.
- Cleaned nomenclature so machine-runnable verification artifacts are
  `scenarios`, while operator-facing launchers remain `demo`s.

Primary implementation surfaces:

- [`docs/plans/INDUSTRIAL_READINESS_GAP.md`](INDUSTRIAL_READINESS_GAP.md)
- [`docs/plans/VERIFICATION_PLAN.md`](VERIFICATION_PLAN.md)
- [`docs/guides/HMI_QUALIFICATION_CHECKLIST.md`](../guides/HMI_QUALIFICATION_CHECKLIST.md)
- [`docs/plans/PRODUCT_DIRECTION_PLAN.md`](PRODUCT_DIRECTION_PLAN.md)
- [`docs/INDEX.md`](../INDEX.md)

### 3. Interrupt runtime moved from conservative baseline to complete cooperative model

- Interrupt handlers now support authored execution budgets via `timeout_s`.
- Timed-out handlers fail closed and are visible in digest/history/export.
- Motion/scene/model-claiming handlers can auto-execute after explicit runtime
  preparation instead of being categorically blocked.
- `restart_subtree` is now a real runtime disposition, not a placeholder
  requiring explicit reset.
- Stacked interrupts are queued and replayed deterministically after the active
  interrupt clears.
- Handler trees are explicitly invalidated on forced exit/reset so they do not
  linger in ambiguous runtime state.

Primary implementation surfaces:

- [`manyforge_behavior/manyforge_behavior/cycle_manager.py`](../../manyforge_behavior/manyforge_behavior/cycle_manager.py)
- [`manyforge_behavior/manyforge_behavior/program.py`](../../manyforge_behavior/manyforge_behavior/program.py)
- [`manyforge_behavior/tests/test_cycle_manager.py`](../../manyforge_behavior/tests/test_cycle_manager.py)
- [`manyforge_behavior/tests/test_program.py`](../../manyforge_behavior/tests/test_program.py)
- [`manyforge_composer/tests/test_execution_observability.py`](../../manyforge_composer/tests/test_execution_observability.py)

### 4. Specs now match the new interrupt-authored/runtime contract

- Program schema now documents:
  - `interrupt_handlers.*.timeout_s`
  - resumability restrictions for motion-claiming handlers
  - first-class `restart_subtree`
  - stacked/deferred interrupt behavior
- Operational logging spec now explicitly requires retained interrupt timeout
  failure detail and deferred-interrupt event coverage.

Primary implementation surfaces:

- [`../../../manyforge_specs/docs/spec/420-program-yaml-schema.md`](../../../manyforge_specs/docs/spec/420-program-yaml-schema.md)
- [`../../../manyforge_specs/docs/spec/520-operational-logging.md`](../../../manyforge_specs/docs/spec/520-operational-logging.md)

## Decisions Taken And Intentionally Kept

### Qualification boundary

- Qualification is now a first-class engineering system, not a loose collection
  of shell demos.
- Machine-runnable verification artifacts are called `scenarios`.
- Operator-facing launchers still keep `demo` terminology where they are
  intentionally interactive/showcase-oriented.

Reason:

- this separates qualification evidence from presentation tooling
- it keeps the industrial verification language precise

### Interrupt-runtime completion boundary

- The interrupt model is now considered complete for the cooperative ManyForge
  runtime contract.
- Remaining interrupt work is treated as integration proof and lower-level
  adapter/hardware work, not missing core interrupt semantics.

Reason:

- the runtime now has bounded handlers, restart semantics, stacked interrupts,
  and full authored handler-domain execution with explicit prep
- what remains is proving those semantics against real external boundaries

## Deferred Review Items

These items remain open, but they are no longer “missing interrupt model.”

### External-control proof

- ROS-facing signal adapters
- PLC-facing signal adapters
- ROXSIE-specific mapping once the connector contract is fixed
- fail-safe stale/disconnect semantics proven end to end against real adapters

### Hardware/runtime proof

- hardware-in-the-loop qualification
- real-cell run/stop/reset/interrupt repetition
- measured stop/hold/abort timing under realistic load
- proof of interrupt semantics on real adapter/kernel stacks, not just mock and
  native local paths

### GUI qualification

- Playwright/browser automation
- stable frontend test selectors
- deterministic frontend/HMI readiness contract
- selective screenshot/golden-image checks for curated scenes

### Future lower-level interrupt enhancement

- true preemption of a blocking, non-cooperative handler tick instead of
  timeout-bound failure between cooperative ticks

## Validation Completed Before This Checkpoint

Validation was done in the Docker `dev` container and, where useful, on host
for faster focused iteration.

Focused container-backed validation completed:

- `pytest -q manyforge_behavior/tests/test_program.py -k interrupt`
- `pytest -q manyforge_behavior/tests/test_cycle_manager.py -k interrupt`
- `pytest -q manyforge_composer/tests/test_execution_observability.py -k pending_interrupt`
- `./scripts/test-all.sh qualification`

Results:

- interrupt program validation: passed
- interrupt runtime behavior tests: passed
- interrupt observability/API slice: passed
- deterministic qualification matrix: passed

Scenario/qualification status at checkpoint time:

- deterministic core qualification tier exists and passes
- native live-evaluation tier exists and passes
- soak/liveness tier exists and passes

## Review Focus For The Next Pass

If this milestone is being reviewed, the highest-value review questions are:

1. Is the scenario/qualification system scoped correctly for future release
   gating?
2. Is the interrupt-runtime contract now coherent enough to treat the remaining
   work as adapter/hardware proof rather than runtime incompleteness?
3. Are the new interrupt-policy validation rules strict enough to prevent
   incoherent authored semantics?
4. Is the retained observability surface sufficient for qualification evidence
   and later `ros2_medkit` correlation?
5. Does the next workstream now clearly belong at the ROS/PLC adapter layer?
