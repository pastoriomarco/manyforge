> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Industrial Readiness Gap

This document records what is still missing before ManyForge can be called
industrial-ready.

It is intentionally narrower than the product-direction and verification plans.
Those documents describe where the system is going. This one describes the
remaining gaps between the current baseline and an industrially credible
runtime, verification, and operator surface.

## Current Baseline

ManyForge is materially stronger than it was:

- typed blackboard/runtime contracts are in place
- interrupt runtime v1 exists
- interrupt timing and operator-action observability now exist
- external signal registry exists
- deployment-level authority-matrix scaffolding now exists
- deterministic, native, and soak scenario qualification tiers now exist
- advanced interrupt qualification scenarios now exist
- retained qualification artifacts now exist
- backend/HMI reconciliation and machine-readable visual checks exist

That is a strong engineering baseline.

It is still not enough to claim industrial readiness.

## Remaining Industrial-Readiness Gaps

### 1. External Control Integration

ManyForge still lacks the real industrial integration boundary.

Still needed:

- ROS-facing signal adapters
- PLC-facing signal adapters
- ROXSIE-specific mapping once the connector contract is fixed
- deployment-level authority-matrix scaffolding exists, but real adapter mapping
  and enforcement across external systems does not
- explicit authority matrix for:
  - commands
  - statuses
  - permits
  - faults
  - acknowledgements
- stale/disconnect/fail-safe behavior proven end to end

Current signal and interrupt semantics are useful, but they are still largely
ManyForge-internal until those adapters exist.

### 2. Interrupt Runtime Integration Proof

The core interrupt model is now complete for the cooperative ManyForge runtime
contract:

- queued stacked interrupts
- timeout-bounded handler execution
- motion/scene/model handler execution with explicit runtime preparation
- `restart_subtree` support

What remains is proving those semantics on real adapters and hardware.

Still needed:

- adapter- and hardware-proven stop/hold/abort semantics
- stricter resource-claim enforcement across external authority domains
- explicit authority matrices for interrupt-relevant domains such as:
  - robot controller ownership
  - PLC ownership
  - gripper / end-effector ownership
  - scene/perception-service ownership
- measured quiesce/abort timing under realistic load
- explicit operator and escalation policy for:
  - which interrupts auto-resume
  - which require reset
  - which require operator acknowledgement
  - which escalate to latched fault / lockout
- `ackRequired` remains a reserved API scaffold and is currently always false
  until an explicit acknowledgement workflow is implemented
- future enhancement for true preemption of a blocking handler tick, not just
  timeout-bound failure between cooperative ticks

Important boundary:

- the missing item is no longer “interrupt features inside ManyForge”
- the missing item is proof that the existing interrupt semantics hold at the
  real execution and integration boundaries
- true preemption of arbitrary non-cooperative blocking handler logic is a
  lower-level runtime/adapter enhancement, not a blocker for the current
  cooperative interrupt model

### 3. Hardware and Cell-Level Validation

Container and simulated qualification is necessary, but it is not enough.

Still needed:

- hardware-in-the-loop runs
- ROS-integrated runs
- PLC-coordinated runs
- repeated run/stop/reset/interruption on real hardware
- measured timing and latency budgets under realistic load
- restart/reconnect validation after process or network disruption

Industrial credibility comes from this layer, not from simulated success alone.

### 4. Qualification System Operationalization

The qualification harness now exists, but it is still only a repository-local
verification system.

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

### 5. Security, Governance, and Compliance Hardening

Future regulated or industrial use will require stronger system-level controls.

Still needed:

- reviewed authentication and authorization policy for remote control surfaces
- complete operator/API audit coverage
- reproducible release inputs and version pinning across the full stack
- requirement traceability:
  - spec requirement
  - test or scenario
  - retained evidence
- dependency and provenance posture:
  - SBOM
  - release provenance
  - dependency review
- operator escalation and failure-classification policy

### 6. Product-Surface Gaps

If ManyForge is meant to be the flagship for industrial manipulation plus
physical AI, these workstreams are still open:

- ROS wrappers
- PLC wrappers
- stronger end-effector and gripper packaging model
- production-grade perception sidecar boundary
- at least one real perception path
- polished `ros2_medkit` alignment beyond the current trace export baseline

## Remaining GUI Qualification Gaps

ManyForge can already automate meaningful visual checks, but it does not yet
have full GUI qualification.

### What Already Exists

Current automated visual verification already covers:

- backend/HMI scene reconciliation via `/api/scene/diagnostics`
- reconciliation trace via `/api/debug/scene-trace`
- snapshot endpoint availability via `/api/scene/snapshot`
- machine-readable detection of backend-vs-HMI scene mismatches

This already catches important classes of bugs such as:

- object exists in backend but is not rendered in HMI
- attached object is not parented to the expected robot frame
- reconciliation stops updating

### What Is Still Missing For Real GUI Tests

#### 1. Browser automation stack

Needed:

- Playwright or equivalent browser automation
- containerized browser execution
- deterministic frontend/backend/HMI orchestration

Without this, ManyForge is not running GUI tests. It is running backend-assisted
visual verification.

#### 2. Stable frontend selectors

Needed:

- `data-testid` markers on critical controls and views
- stable roles and labels for control surfaces
- DOM-visible UI state for:
  - active cycle state
  - interrupt mode
  - edit-lock state
  - runtime-out-of-date overlays

Without this, browser tests will be fragile.

#### 3. Deterministic readiness and synchronization

Needed:

- explicit frontend readiness conditions
- stable wait conditions for:
  - program loaded
  - scene synchronized
  - cycle state updated
  - interrupt latched
- possibly a small readiness marker in the DOM or a test-friendly readiness API

Without this, GUI tests will race the app and fail spuriously.

#### 4. 3D/WebGL verification strategy

Needed:

- semantic GUI checks as the default approach
- limited golden-image or screenshot checks only for curated scenes
- ideally, a test-accessible rendered-object/scene-graph state surface

Broad screenshot-diff testing should not be the default. It is too brittle.

#### 5. CI browser test environment

Needed:

- headless browser in the qualification environment
- pinned viewport, fonts, and rendering environment
- reproducible orchestration of:
  - Composer backend
  - frontend
  - HMI
  - browser test runner

### Manual GUI Qualification Boundary

Until full browser qualification exists, ManyForge still needs a formal manual
release gate for HMI/browser semantics.

That is why the current repo now includes:

- [`../guides/HMI_QUALIFICATION_CHECKLIST.md`](../guides/HMI_QUALIFICATION_CHECKLIST.md)

This manual layer is acceptable for release and showcase validation, but it is
not a substitute for real unattended GUI tests.

## Recommended Next Order

The highest-value next steps are:

1. build the ROS/PLC signal adapter substrate into real adapters
2. expand native and hardware qualification
3. add Playwright-based frontend state tests
4. keep semantic HMI checks as the primary automated visual oracle
5. add selective screenshot/golden-image checks only for curated scenes

## Decision Boundary

ManyForge is now:

- strong enough to support serious engineering iteration
- strong enough to support scenario-based qualification
- strong enough to support industrial-facing demos with evidence

It is not yet:

- production-proven on real external-control boundaries
- fully qualified on hardware
- fully GUI-tested
- fully hardened for future compliance or regulated deployment

That is the current industrial-readiness gap.
