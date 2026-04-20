> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Verification Plan

This document defines how ManyForge should be verified as it moves from a
spec-driven, AI-generated codebase toward an industrially credible runtime.

This document is the verification strategy layer.

For the broader industrial-readiness gap register, including non-verification
gaps such as external integration, compliance posture, and GUI-test maturity,
use:

- [`INDUSTRIAL_READINESS_GAP.md`](INDUSTRIAL_READINESS_GAP.md)

The central decision is:

- qualification should stop depending on informal operator demos
- operator-facing demos should be backed by versioned, executable qualification scenarios

That is the right way to prove that the pipeline still works after changes,
that behavior chains remain deadlock-free, and that regressions are caught with
retained evidence rather than operator memory.

## Why This Plan Exists

The existing test suite is necessary but not sufficient.

Current tests already prove a lot:

- unit behavior
- API and schema conformance
- runtime state transitions
- interrupt semantics
- interrupt timing, operator-action semantics, and advanced handler branches
- HMI/backend reconciliation logic
- full container build/test viability

What they do not fully prove is:

- that complete authored applications still work end to end
- that all allowed control-flow ramifications remain valid after updates
- that long-running paths remain free from deadlock or stalled progress
- that visual/runtime/operator-facing behavior still matches backend truth

For future industrial use, ManyForge needs a stronger verification model.

## Verification Goal

ManyForge should be able to say, with evidence:

- this scenario is allowed by the product/runtime contract
- this scenario has been executed in a production-like container/runtime
- the expected states, traces, and side effects occurred
- no forbidden states, errors, or deadlocks occurred
- the same scenario can be rerun after code changes and produce the same class
  of result

This is not formal proof in the mathematical sense.
It is practical requalification.

## Principles

### 1. Treat operator demos as scenario candidates

Every serious operator demo should have a corresponding qualification scenario
that is:

- versioned
- repeatable
- machine-runnable
- explicit about expected outcomes

### 2. Verify behavior, not just implementation

The most important assertions are not:

- "function X returned Y"

They are:

- "the cycle entered `resumable_hold` after the interrupt"
- "the handler completed"
- "paused time did not consume execution timeout budget"
- "the backend object was also rendered in the HMI"

### 3. Prefer explicit runtime oracles over screenshots

Visual validation should first rely on machine-readable observability surfaces:

- `/api/scene/diagnostics`
- `/api/debug/scene-trace`
- `/api/execution/digest`
- `/api/execution/history`
- `/api/debug/export`

Screenshots are useful, but they should not be the primary oracle unless the
behavior cannot be expressed any other way.

### 4. Retain evidence

A scenario run is not complete unless it produces artifacts:

- scenario id and revision
- container image / git SHA
- execution digest timeline
- execution history
- debug export
- blackboard snapshots
- scene diagnostics
- optional screenshots

### 5. Separate deterministic qualification from live evaluation

LLM/perception-backed flows need two tracks:

- deterministic qualification:
  - fixed inputs
  - mocked or recorded model outputs
  - strict pass/fail
- live evaluation:
  - real endpoints
  - looser acceptance envelopes
  - tracked separately from hard qualification

Do not mix those two.

## Verification Layers

### Layer 1: Spec and contract tests

Keep and expand:

- unit tests
- API tests
- schema validation tests
- native backend tests
- contract/state-machine tests

Purpose:

- catch local regressions cheaply
- verify bounded semantics and data contracts

### Layer 2: Deterministic scenario qualification

Build a scenario harness that runs authored program/deployment pairs in a
containerized environment and drives them through explicit steps.

Each scenario should define:

- program
- deployment
- trigger schedule
- wait conditions
- expected state transitions
- required history events
- required blackboard values
- forbidden events
- scenario timeout

Purpose:

- prove end-to-end behavior chains
- turn operator-facing flows into rerunnable scenario tests

### Layer 3: Fault-injection scenario qualification

Scenarios should also cover failure and recovery branches:

- planning failure
- validation failure
- execution timeout
- interrupt during motion
- interrupt during hold
- stale/disconnected signal source
- reset during stopped/paused execution
- blocked interrupt handler claims

Purpose:

- verify allowed ramifications, not only happy paths

Current baseline:

- `core_qualification` covers the standard deterministic control-flow scenarios
- `interrupt_qualification` covers advanced interrupt branches including:
  - timeout-bounded handler failure
  - `restart_subtree`
  - motion-bearing handler execution
  - deferred higher-priority interrupt consumption
- advanced interrupt scenarios run in isolated Composer processes so one failed
  scenario cannot poison the next qualification result

### Layer 4: Soak and liveness testing

Add repeat/soak scenarios that run:

- repeated run/reset cycles
- repeated interrupt/resume cycles
- repeated signal loss/recovery cycles

Add liveness assertions:

- event index continues increasing while active
- state changes or progress advances within configured windows
- no scenario remains in a non-terminal active state beyond the expected bound

Purpose:

- catch deadlocks, stalls, and timing regressions

### Layer 5: Release qualification

Before major presentations or releases, run a gated qualification batch that includes:

- deterministic scenario suite
- selected live-evaluation scenarios
- operator-reviewed visual checks where automation is not yet trustworthy

Purpose:

- provide higher-confidence evidence for external presentations and future
  regulated environments

## Scenario Model

The harness should not execute ad hoc shell scripts as the test definition.

It should use a structured scenario artifact, for example:

```yaml
scenario_id: interrupt_pick_and_place_pause_resume
program: examples/interrupt_pick_and_place.program.yaml
deployment: examples/interrupt_pick_and_place.deployment.yaml
timeouts:
  scenario_s: 60
steps:
  - action: cycle_run
  - wait_for:
      digest.state: running
  - action: publish_signal
    adapter: demo_ui
    signal: pause_request
    value: true
    stale_after_ms: 1500
  - wait_for:
      digest.pendingInterrupt.policyId: operator_pause_hold
  - wait_for:
      digest.state: stopped
  - action: cycle_run
  - wait_for:
      digest.state: running
assertions:
  - history_contains:
      event_type: interrupt_latched
  - history_contains:
      event_type: interrupt_handler_completed
  - blackboard_equals:
      demo/pause_latched: true
forbidden:
  - event_type: kernel_error
```

The scenario runner should:

1. bring up the stack in a known container
2. load program and deployment
3. drive explicit actions
4. poll or stream runtime state
5. persist all evidence artifacts
6. evaluate pass/fail

## Current Implementation Baseline

The first slice of this plan is now implemented:

- scenario runner module:
  - `manyforge_composer/verification/runner.py`
- live runner CLI:
  - `scripts/run-scenarios.py`
- deterministic qualification wrapper:
  - `scripts/qualify-scenarios.sh`
- native live-evaluation wrapper:
  - `scripts/qualify-native-scenarios.sh`
- soak/liveness wrapper:
  - `scripts/qualify-soak-scenarios.sh`
- deterministic qualification matrix:
  - `verification/matrices/core_qualification.yaml`
- native live-evaluation matrix:
  - `verification/matrices/native_live_evaluation.yaml`
- soak/liveness matrix:
  - `verification/matrices/soak_qualification.yaml`
- native-stable live-evaluation program/deployment artifacts:
  - `verification/programs/native_simple_motion_ur5.program.yaml`
  - `verification/deployments/native_simple_motion_ur5.deployment.yaml`
- manual browser/HMI release gate:
  - `docs/guides/HMI_QUALIFICATION_CHECKLIST.md`

The current verification split is:

- deterministic qualification:
  - forced mock kernel
  - strict end-to-end pass/fail
  - suitable for CI gating
- native live evaluation:
  - real RoboPlan/native kernel in simulated execution mode
  - validates the production planner/executor path
- soak/liveness:
  - repeated deterministic scenario runs
  - intended to catch stalls, deadlocks, and state-regression drift
- manual HMI release qualification:
  - used where operator-facing browser semantics are still not robustly
    automatable
- initial qualification scenarios:
  - `verification/scenarios/basic_pick_and_place_complete.scenario.yaml`
  - `verification/scenarios/interrupt_pick_and_place_pause_resume.scenario.yaml`
  - `verification/scenarios/interrupt_pick_and_place_reset_after_hold.scenario.yaml`
  - `verification/scenarios/interrupt_signal_stale_before_run_no_effect.scenario.yaml`
- initial matrix:
  - `verification/matrices/core_qualification.yaml`
- CI/batch entrypoint:
  - `scripts/qualify-scenarios.sh`

The current runner intentionally stays narrow:

- it drives the existing HTTP API rather than a parallel test-only interface
- it supports explicit actions like:
  - load program
  - run/stop/reset cycle
  - publish signal
  - sleep
  - wait for a condition
- it retains final evidence artifacts from:
  - execution digest
  - execution history
  - blackboard snapshot
  - scene diagnostics
  - scene trace
  - debug export
- it records run metadata including:
  - git revision
  - scenario or matrix content hash
  - hostname/platform
  - Python version
  - workspace root

The current live qualification path can now self-launch Composer locally
through `scripts/run-scenarios.py --launch local-process`, which removes the
need for a human to start the stack before running the core deterministic batch.

The deterministic qualification tier intentionally forces mock-kernel mode.
That is deliberate:

- deterministic qualification should validate ManyForge control semantics
- it should not fail because of native planner variability or scene-specific
  planning validity
- native planning and execution should be covered separately by live-evaluation
  batches and backend-native tests

This is the right baseline because it verifies the real control plane and keeps
the qualification path aligned with production behavior.

## Visual Verification Strategy

Visual verification is feasible, but it should be layered.

### Level A: Machine-readable HMI/backend reconciliation

This should be the default automated path.

ManyForge already has the right primitives:

- `/api/scene/diagnostics`
  - compares backend-authoritative scene objects with HMI render state
- `/api/debug/scene-trace`
  - exposes HMI reconciliation events
- HMI-side object render diagnostics
  - used today in tests to assert that attached objects are rendered and parented
    correctly

This is the most reliable way to catch problems like:

- backend object exists but HMI did not render it
- object is attached in backend but not parented to a robot frame in the HMI
- reconciliation stopped updating

For the specific failure class you mentioned, this is already a strong
automation target. It is better than waiting for a person to look at the UI.

### Level B: Rendered image smoke checks

For some classes of UI failures, use:

- `/api/scene/snapshot`
- or equivalent HMI snapshot capture

This can support:

- snapshot existence checks
- image size / non-empty checks
- basic pixel-difference regression checks for stable scenes

This is useful, but more brittle than diagnostics-based verification.

### Level C: Browser/UI regression tests

Use browser automation only where the UI itself is the subject:

- buttons enabled/disabled correctly
- panels show interrupt state
- runtime-out-of-date overlays behave correctly
- operator control visibility and edit locks

This should focus on UI state semantics, not 3D scene truth.

### Level D: Manual qualification batch

Manual verification is still acceptable, but only as an explicit release
qualification layer with a checklist.

Use it when:

- the rendered scene is too complex for reliable automated image comparison
- a human must confirm usability or visual clarity
- the feature is important enough to justify a release checklist

If used, it should be formalized:

- named scenario
- exact operator steps
- expected observations
- pass/fail checklist
- retained notes and artifacts

That is much better than an informal "someone looked at it."

## What Should Be Automated First

Start with scenarios that already exist as operator demos or well-understood flows.

### Phase 1

- basic pick-and-place happy path
- interrupt pick-and-place pause/resume
- interrupt then reset
- scene attach/detach render consistency

### Phase 2

- planning failure and recovery path
- blocked interrupt handler claims
- stale external signal handling
- direct stop vs interrupt hold semantics

### Phase 3

- ROS-facing signal adapter scenarios
- perception-guided deterministic scenarios
- long-run soak scenarios

## CI Tiers

### Tier 1: Per-PR fast gate

Run:

- unit/API/native tests
- a small deterministic scenario subset

Goal:

- fast regression detection

### Tier 2: Main-branch extended suite

Run:

- broader deterministic scenario matrix
- selected visual reconciliation checks

Goal:

- catch cross-subsystem regressions

### Tier 3: Nightly qualification

Run:

- fault-injection scenarios
- soak/liveness scenarios
- artifact retention

Goal:

- detect timing and stability regressions that short runs miss

### Tier 4: Release/presentation qualification

Run:

- full deterministic batch
- selected live-evaluation scenarios
- manual checklist batch if needed

Goal:

- external-facing confidence

## Evidence Format

Each scenario run should retain at least:

- scenario id
- git SHA
- image/container id
- start/end timestamps
- pass/fail result
- step timeline
- execution digest snapshots
- execution history
- debug export
- blackboard snapshot
- scene diagnostics
- optional HMI screenshot

This should be stored in a stable per-run artifact directory.

## Initial Implementation Plan

### Step 1

Define the scenario file format and runner contract.

### Step 2

Convert current operator demo flows into first-class scenarios instead of
relying on shell scripts alone.

Start with:

- pick-and-place
- interrupt pick-and-place

### Step 3

Use existing observability endpoints as the main scenario oracles.

Do not start with screenshot diffs as the primary mechanism.

### Step 4

Add visual reconciliation assertions based on:

- `/api/scene/diagnostics`
- `/api/debug/scene-trace`

### Step 5

Add one release-oriented manual qualification checklist for scenarios that still
need human confirmation.

## Decision

Yes, this is feasible.

The correct implementation is not:

- “run operator demos and hope they look fine”

It is:

- convert operator-facing flows into qualification scenarios
- run them automatically
- assert on runtime truth and traceability
- retain evidence
- add manual qualification only where automation is still not reliable enough

That is the right path for an AI-coded but spec-driven system that needs to
become industrially credible.
