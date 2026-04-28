> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Industrial Execution and Readiness Plan

This is the main execution and readiness plan for ManyForge.

It replaces the old split between:

- industrial readiness gap tracking
- verification strategy

The purpose of this document is practical:

- define what still matters before ManyForge can be called industrially
  credible
- define how that proof should be produced
- separate critical-path work from optional or later work

For the interrupt execution model itself, keep using the separate technical
reference:

- [`INTERRUPT_MODEL_PLAN.md`](INTERRUPT_MODEL_PLAN.md)

## Scope

This plan covers:

- real robot adapter integration
- external control integration
- interrupt proof at the real adapter boundary
- verification and qualification strategy
- hardware and cell-level validation
- GUI/browser qualification
- security, governance, and audit-related readiness

It does not cover:

- assistant and OpenClaw integration details
- the broader product roadmap

Use these for that:

- [`AI_ASSISTANT_INTEGRATION_PLAN.md`](AI_ASSISTANT_INTEGRATION_PLAN.md)
- [`PRODUCT_DIRECTION_PLAN.md`](PRODUCT_DIRECTION_PLAN.md)

## Spec Basis For The Priority Order

The sequencing in this plan follows the current contract docs in
`manyforge_specs`, especially:

- `docs/spec/000-system-overview-and-scope.md`
- `docs/spec/100-component-model-and-interactions.md`
- `docs/spec/240-supported-backend-capability-profile.md`
- `docs/spec/500-observability-tracing-and-performance.md`
- `docs/spec/520-operational-logging.md`

The important implication is:

- the industrial adapter story is broader than signals
- the specs define real execution, robot-state, controller-management,
  gripper/tooling, and observability as adapter responsibilities
- PLC/ROXSIE and signal-centric work are downstream of that substrate, not ahead
  of it

## Current Baseline

ManyForge is materially stronger than it was:

- typed blackboard/runtime contracts exist
- interrupt runtime v1 exists
- external signal registry exists
- deterministic, native, and soak qualification tiers exist
- qualification artifacts are retained
- backend/HMI reconciliation and machine-readable visual checks exist
- gripper-aware manipulation and scene editing are now materially stronger

That is a strong engineering baseline.

It is still not enough to claim industrial readiness.

## Core Readiness Themes

### 1. Real robot adapter substrate

This is the first missing dependency.

The specs do not frame industrial integration as “signals first.” They define a
supported adapter matrix that starts with the real robot control surface and its
observable execution semantics.

Before PLC and ROXSIE-specific work matters, ManyForge needs the adapter
families that make real robot control credible:

- execution backend / controller interface
- robot state / recovery
- controller management
- gripper / tooling
- observability and operational logging

Why this comes first:

- without it, ManyForge cannot claim real robot control
- without it, interrupt proof remains mostly local/runtime-only
- without it, signal and PLC work has no serious controlled target
- without it, qualification and traceability cannot represent the real execution
  path that the specs require

Still needed:

- one supported real execution backend family beyond the native/sim path
- wrapper/interfaces for controller-backed execution
- robot readiness and reset/recover surfaces
- controller switching/load-unload surfaces where deployments require them
- gripper command/feedback surfaces aligned with the same runtime contract
- trace/log export at the adapter boundary, not just inside the local runtime

### 2. Repo-wide ROS integration

ROS integration should be treated as a repo-wide adapter integration effort, not
as a narrow signal-adapter task.

Still needed:

- ROS-facing wrappers for the real robot adapter families above
- consistent ROS-side exposure of:
  - execution commands and feedback
  - robot state and recovery actions
  - controller management
  - gripper command/feedback
  - observability/tracing surfaces
- startup/runtime compatibility checks against declared adapter capabilities

### 3. External control integration

Still needed:

- signal and IO/GPIO adapters
- PLC-facing signal adapters
- ROXSIE-specific mapping once the connector contract is fixed
- explicit authority matrices for:
  - commands
  - statuses
  - permits
  - faults
  - acknowledgements
- stale/disconnect/fail-safe behavior proven end to end

This work remains important, but it is downstream of the real robot adapter and
repo-wide ROS integration work.

### 4. Interrupt proof at the real boundary

The core interrupt model is good enough for the cooperative ManyForge runtime
contract. What remains is proof at the real integration boundary.

Still needed:

- adapter- and hardware-proven stop/hold/abort semantics
- stricter resource-claim enforcement across external authority domains
- measured quiesce/abort timing under realistic load
- explicit operator and escalation policy for:
  - auto-resume
- reset-required cases
- operator acknowledgement
- latched fault / lockout

### 5. Qualification system operationalization

The harness exists. The missing work is operationalization.

Still needed:

- CI artifact publishing outside local temp directories
- nightly qualification jobs
- long-duration soak and liveness jobs
- qualification history retention and comparison across commits
- release gating policy tied to scenario matrices
- explicit separation between:
  - deterministic qualification
  - native live evaluation
- hardware-in-the-loop qualification
- operator-reviewed release qualification

### 6. Hardware and cell-level validation

Container and simulated qualification is necessary but not enough.

Still needed:

- hardware-in-the-loop runs
- ROS-integrated runs
- PLC-coordinated runs
- repeated run/stop/reset/interruption on real hardware
- measured timing and latency budgets under realistic load
- restart/reconnect validation after process or network disruption

### 7. GUI and browser qualification

ManyForge can already automate meaningful visual checks. It still does not have
full GUI qualification.

Still needed:

- Playwright or equivalent browser automation
- stable selectors and readiness conditions
- deterministic browser test orchestration
- semantic GUI checks as the main verification style
- limited screenshot/golden-image checks only for curated scenes

This remains important because operator and browser surfaces are part of the
runtime story.

### 8. Security, governance, and compliance hardening

Still needed:

- reviewed authentication and authorization policy for remote control surfaces
- pre-release hardening for development/demo server defaults:
  - localhost binding by default
  - explicit opt-in for unauthenticated non-loopback binds
  - token required or fail-fast before public or shared-network use
- complete operator/API audit coverage
- reproducible release inputs and version pinning across the stack
- requirement traceability:
  - spec requirement
  - test or scenario
  - retained evidence
- dependency and provenance posture:
  - SBOM
  - release provenance
  - dependency review

## Verification Strategy

The verification sequence must follow the adapter dependency chain.

That means:

1. contract-test the real robot adapter substrate
2. verify repo-wide ROS wrapper behavior on top of it
3. then verify PLC/ROXSIE and signal-facing industrial control flows
4. then expand hardware/cell qualification and operator-surface qualification

### Principles

1. Treat serious operator demos as scenario candidates.
2. Verify behavior, not only implementation details.
3. Prefer explicit runtime oracles over screenshots.
4. Retain evidence.
5. Separate deterministic qualification from live evaluation.

### Verification layers

#### Layer 1: Spec and contract tests

Keep and expand:

- unit tests
- API tests
- schema validation tests
- native backend tests
- contract/state-machine tests

#### Layer 2: Deterministic scenario qualification

Run authored program/deployment pairs with:

- explicit triggers
- wait conditions
- expected state transitions
- required history events
- required blackboard values
- forbidden events
- scenario timeout

#### Layer 3: Fault-injection qualification

Cover:

- planning failure
- validation failure
- execution timeout
- interrupt during motion
- interrupt during hold
- stale/disconnected signal source
- reset during stopped/paused execution
- blocked interrupt handler claims

#### Layer 4: Soak and liveness testing

Add repeated run/reset and interrupt/resume cycles with liveness assertions.

#### Layer 5: Release qualification

Before major presentations or releases, run:

- deterministic scenario suite
- selected live-evaluation scenarios
- operator-reviewed visual checks where automation is not yet trustworthy

## Priority Order

### Critical path

1. real robot adapter substrate
2. repo-wide ROS integration across the required adapter families
3. qualification expansion on top of that substrate
4. hardware and cell-level validation
5. browser/HMI qualification sufficient for release credibility

### Important but not blocking the first serious industrial story

1. PLC and ROXSIE integration on top of the robot/ROS substrate
2. richer product-surface polish
3. deeper perception-sidecar work
4. broader ROS/medkit alignment beyond the current trace baseline

### Optional or later

1. broad screenshot-diff testing as a primary method
2. feature growth that outruns qualification coverage
3. industrial-readiness claims before hardware and adapter proof exists

## Recommended Execution Order

1. build the real robot adapter substrate:
   - execution backend / controller interface
   - robot state / recovery
   - controller management
   - gripper / tooling
   - observability and operational logging
2. build repo-wide ROS wrappers around that substrate
3. bind qualification scenarios to those real robot and ROS surfaces
4. expand native, ROS-backed, and hardware qualification
5. only then add PLC/ROXSIE and richer signal-facing industrial control flows
6. add Playwright-based frontend state tests
7. keep semantic HMI checks as the main visual oracle
8. add selective screenshot/golden-image checks only for curated scenes

## Exit Criteria

This plan is in a good state when ManyForge can show:

- real robot adapter boundaries with evidence-backed behavior
- repo-wide ROS integration across the required control and observability
  surfaces
- interrupt and signal semantics proven beyond the local runtime
- repeatable qualification with retained evidence
- meaningful browser/operator-surface verification
- a readiness story that is evidence-backed rather than demo-backed
