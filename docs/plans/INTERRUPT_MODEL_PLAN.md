> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Interrupt Model Plan

This document is the technical reference for ManyForge interrupt execution.

It is intentionally narrower than the roadmap plans. Use it when the question
is:

- what the interrupt model is
- what semantics it must preserve
- what still needs proof at the real integration boundary

For roadmap priority and readiness sequencing, use:

- [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)
- [`PRODUCT_DIRECTION_PLAN.md`](PRODUCT_DIRECTION_PLAN.md)

## Why This Matters

Industrial manipulators do not just execute long-running motions and waits.
They must react to external events while execution is in progress:

- safety PLC signals
- collision or force thresholds
- fixture or IO faults
- operator stop/hold requests
- timeout conditions
- delegated recovery or supervisory intervention triggers

ManyForge has:

- deterministic motion/action nodes
- intervention and delegated recovery scaffolding
- cycle-level stop/reset behavior

It still needs interrupt semantics that remain explicit, auditable, and safe at
the real execution boundary.

## Scope

This plan covers:

- interrupt sources and semantics
- how interrupts attach to running program execution
- required program/deployment model support
- execution, kernel, eventing, and Composer impact

It does not replace:

- [`AI_ASSISTANT_INTEGRATION_PLAN.md`](AI_ASSISTANT_INTEGRATION_PLAN.md) for
  model, assistant, and OpenClaw-related work
- [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)
  for the broader readiness and proof plan

## Current Implementation Status

The interrupt-adjacent groundwork now in place includes:

- node catalog entries may declare `interruptible` and
  `default_interrupt_scope`
- deployment artifacts may declare `interrupt_sources`
- program artifacts may declare `interrupt_handlers` and
  `interrupt_policies`
- execution events expose a stable envelope suitable for later interrupt
  causality and external trace alignment
- `CycleManager` performs conservative interrupt detection, arbitration,
  latching, and deterministic cycle stop/hold
- execution digest and history expose the pending interrupt and interrupt
  lifecycle events

Current execution-pass decision:

- interrupt runtime v1 is complete for the cooperative ManyForge runtime
  contract:
  - detect sources
  - arbitrate policies
  - latch one active interrupt plus queue deferred interrupts by priority
  - stop/hold the cycle deterministically
  - compile named handler trees separately from the main tree
  - auto-execute handlers across the declared resource-claim model
  - support motion-bearing handlers through explicit kernel/runtime
    preparation
  - enforce optional handler execution budgets
  - support `restart_subtree`
  - publish active interrupt context in `_interrupt/current`
  - expose the latched interrupt to operators and trace tooling
  - fail safe for unhandled non-maskable sources

There is also a concrete runtime demo:

- launcher:
  [`../../scripts/demo-interrupt-pick-place.sh`](../../scripts/demo-interrupt-pick-place.sh)
- operator guide:
  [`../guides/INTERRUPT_DEMO.md`](../guides/INTERRUPT_DEMO.md)

## What Remains Open

The missing work is no longer core interrupt-model invention. It is mainly:

- proof on real adapters and hardware, not only in cooperative runtime and
  mock/native environments
- mapping ROS/PLC/ROXSIE sources cleanly onto deployment interrupt sources
- implementing an explicit acknowledgement workflow if `ackRequired` is meant
  to become load-bearing
- future enhancements such as true preemption of a blocking handler tick
  instead of timeout detection between cooperative ticks

## Functional Target

ManyForge should support this runtime pattern:

1. A long-running node or subtree is executing.
2. An interrupt source emits a trigger.
3. The runtime evaluates configured interrupt policies by priority.
4. The active node/subtree is stopped, cancelled, or held according to policy.
5. A configured interrupt action chain is executed.
6. The interrupt and all downstream actions are recorded with traceable
   causality.
7. The runtime either:
   - resumes the interrupted program at a defined point,
   - transitions to a controlled halted state, or
   - escalates to operator/supervisory handling.

## Core Requirements

### 1. Triggerable from real runtime events

Interrupts must be able to originate from:

- kernel/runtime adapter events
- motion validation/execution feedback
- IO adapters
- operator UI commands
- timeouts/watchdogs
- external service or supervisory requests

### 2. Explicit semantics

Each interrupt definition must state:

- source
- trigger condition
- priority
- latching behavior
- whether it is maskable
- affected execution scope
- preemption mode
- handler chain
- post-handler disposition

### 3. Deterministic action chain

The handler cannot be an opaque callback. It must be a declared action chain
with explicit steps.

### 4. Auditability

The runtime must record:

- which interrupt source fired
- which policy matched
- what node/subtree was active
- what cancellation/stop request was issued
- which handler steps ran
- final disposition: resumed, halted, escalated, failed

### 5. Safety over convenience

The runtime must prefer fail-stop over optimistic resume when the interrupt
leaves execution state ambiguous.

## Recommended Delivery Order

1. keep the current cooperative interrupt runtime stable
2. prove stop/hold/abort semantics through real adapters
3. prove the same semantics through hardware and cell-level qualification
4. add explicit acknowledgement workflows if they are meant to become
   operationally load-bearing
5. treat true preemption of arbitrary blocking work as a later runtime
   enhancement, not as the definition of interrupt v1

## Relationship To The Active Plan Set

This plan stays separate because it is sensitive and easy to dilute. It should
remain the normative reference for interrupt semantics while the broader
execution/readiness plan owns priority and delivery sequencing.
