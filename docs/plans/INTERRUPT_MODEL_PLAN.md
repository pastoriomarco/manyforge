> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Interrupt Model Plan

This document defines the execution feature ManyForge is currently missing for
industrial manipulator control: event-triggerable interrupts with explicit,
auditable handling chains.

The goal is not to bolt ad hoc callbacks onto long-running nodes. The goal is
to introduce a runtime interrupt model that can:

- stop or cancel motion safely
- trigger deterministic follow-up actions
- preserve auditability
- support both local runtime recovery and higher-level escalation flows

## Why This Matters

Industrial manipulators do not just execute long-running motions and waits.
They must react to external events while execution is in progress:

- safety PLC signals
- collision or force thresholds
- fixture or IO faults
- operator stop/hold requests
- timeout conditions
- delegated recovery or supervisory intervention triggers

ManyForge today has:

- deterministic motion/action nodes
- intervention and delegated recovery scaffolding
- cycle-level stop/reset behavior

It does not yet have a first-class interrupt model that can bind runtime events
to a configured action chain.

That gap is acceptable for demos. It is not acceptable for industrial
deployment.

## Scope

This plan covers:

- interrupt sources and semantics
- how interrupts attach to running program execution
- the program/deployment spec changes required
- execution, kernel, eventing, and Composer impact

This plan does not replace:

- [`LLM_OPERABILITY_PLAN.md`](LLM_OPERABILITY_PLAN.md) for model/runtime-LMM work
- [`OPENCLAW_INTEGRATION_PLAN.md`](OPENCLAW_INTEGRATION_PLAN.md) for OpenClaw
  integration boundaries

## Functional Target

ManyForge should support this runtime pattern:

1. A long-running node or subtree is executing.
2. An interrupt source emits a trigger.
3. The runtime evaluates configured interrupt policies by priority.
4. The active node/subtree is stopped, cancelled, or held according to the
   policy.
5. A configured interrupt action chain is executed.
6. The interrupt and all downstream actions are recorded with traceable
   causality.
7. The runtime either:
   - resumes the interrupted program at a defined point,
   - transitions to a controlled halted state, or
   - escalates to operator/supervisory handling.

## Core Requirements

### 1. Triggerable From Real Runtime Events

Interrupts must be able to originate from:

- kernel/runtime adapter events
- motion validation/execution feedback
- IO adapters
- operator UI commands
- timeouts/watchdogs
- external service or supervisory requests

### 2. Explicit Semantics

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

### 3. Deterministic Action Chain

The handler cannot be an opaque callback. It must be a declared action chain
with explicit steps, for example:

- stop active motion
- open gripper
- retreat to a safe pose
- set blackboard flags
- publish operator message
- escalate to human

### 4. Auditability

The runtime must record:

- which interrupt source fired
- which policy matched
- what node/subtree was active
- what cancellation/stop request was issued
- which handler steps ran
- final disposition: resumed, halted, escalated, failed

### 5. Safety Over Convenience

The runtime must prefer fail-stop over optimistic resume when the interrupt
leaves execution state ambiguous.

## Proposed Model

## 1. Interrupt Sources

Introduce deployment-defined interrupt sources, for example:

```yaml
interrupt_sources:
  safety_stop:
    adapter: plc
    signal: safety.stop
    kind: level
    priority: 100
    maskable: false
  collision_detected:
    adapter: kernel
    signal: collision_detected
    kind: edge
    priority: 90
    maskable: false
  fixture_timeout:
    adapter: timer
    signal: fixture_wait_timeout
    kind: edge
    priority: 40
    maskable: true
  operator_pause:
    adapter: composer_ui
    signal: pause_request
    kind: edge
    priority: 30
    maskable: true
```

Required semantics:

- `kind`: `edge` or `level`
- `priority`: higher wins
- `maskable`: whether node/subtree policy may defer or ignore it

Optional later additions:

- debounce window
- rate limit
- correlation keys
- source payload schema

## 2. Interrupt Policies

Add program-level interrupt policies that bind a source to execution behavior.

Example:

```yaml
interrupt_policies:
  - id: collision_abort_and_retreat
    source: collision_detected
    applies_to:
      node_kinds: [move_manipulator_action]
    preemption_mode: abort_active_node
    handler: collision_safe_retreat
    after_handler: halted

  - id: operator_pause_hold
    source: operator_pause
    applies_to:
      subtree_root: pick_sequence
    preemption_mode: hold_cycle
    handler: pause_announce
    after_handler: resumable_hold
```

Fields needed:

- `id`
- `source`
- `applies_to`
- `preemption_mode`
- `handler`
- `after_handler`
- optional `condition` using source payload / blackboard state

## 3. Handler Chains

Handler chains should be declared as normal ManyForge trees or typed action
lists, not embedded script snippets.

Pragmatic v1 approach:

- handler references a named recovery/intervention subtree
- runtime executes it in a dedicated interrupt context

Example:

```yaml
interrupt_handlers:
  collision_safe_retreat:
    root:
      kind: sequence
      children:
        - kind: stop_active_motion
        - kind: move_manipulator_action
          params:
            motion_type: relative_move
            relative_move: ...
        - kind: set_blackboard
          params:
            key: runtime/interrupt_state
            value: collision_retreated
```

This should reuse existing subtree/skill/intervention infrastructure where
possible, but the execution semantics must be runtime-native and deterministic.

## 4. Execution Scope

Interrupt policies need explicit scope targeting. Required scope levels:

- active node only
- active subtree
- whole cycle
- kernel motion only

Examples:

- `abort_active_node`
- `abort_subtree`
- `hold_cycle`
- `kernel_stop_only`
- `latched_fault`

## 5. Post-Handler Disposition

After the handler runs, the runtime must choose a declared disposition:

- `resume_node`
- `resume_subtree`
- `restart_subtree`
- `halted`
- `failed`
- `escalate`
- `resumable_hold`

Not every preemption mode can legally pair with every disposition. For example:

- `latched_fault` should not auto-resume
- `kernel_stop_only` may be compatible with `resume_node` only if the node
  declares resumability

## Spec Changes Required

## 1. Deployment Schema

Deployment needs interrupt source and adapter definitions:

- `interrupt_sources`
- optional adapter-specific settings
- optional default priority/maskability rules

This belongs in deployment, not the program, because source wiring is an
environment/runtime concern.

## 2. Program Schema

Program schema needs interrupt policy and handler definitions.

Minimum additions:

- `interrupt_policies`
- `interrupt_handlers`

Likely node-level metadata additions:

- `interrupt_mask`
- `supports_resume`
- `interrupt_scope_override`

We should avoid pushing the full policy definition down into every node. Keep
policies centralized and let nodes expose capabilities.

## 3. Node Catalog Schema

Catalog entries need interrupt capability metadata so Composer and runtime know
what is legal.

Suggested fields:

- `interruptible: bool`
- `supports_cancel: bool`
- `supports_hold: bool`
- `supports_resume: bool`
- `default_interrupt_scope`

Examples:

- `move_manipulator_action`
  - interruptible: true
  - supports_cancel: true
  - supports_hold: adapter-dependent
  - supports_resume: false initially

- `query_model`
  - interruptible: true
  - supports_cancel: cooperative only
  - supports_resume: false

- pure leaf computation nodes
  - interruptible: effectively no-op or deferred

## 4. Event/Trace Schema

Event recording must gain explicit interrupt event types, for example:

- `interrupt_detected`
- `interrupt_policy_selected`
- `interrupt_preemption_requested`
- `interrupt_handler_started`
- `interrupt_handler_step`
- `interrupt_handler_completed`
- `interrupt_resume_requested`
- `interrupt_escalated`

Required event fields:

- `interrupt_id`
- `source`
- `policy_id`
- `active_node_id`
- `active_move_id`
- `preemption_mode`
- `disposition`
- correlation to the interrupted run

## Implementation Impact

## 1. Cycle Manager

`CycleManager` needs an explicit interrupt path, not just stop/reset.

Required additions:

- interrupt queue or dispatcher
- priority arbitration
- latched interrupt state
- handler-chain execution state
- resume/halt disposition logic

This is the core implementation change.

## 2. Runner / Node Contract

Nodes need an interrupt contract beyond `terminate()`.

At minimum:

- a way to receive interrupt intent
- a declared capability set
- cooperative cancellation state for long-running nodes

The current node contract is too binary:

- tick/update
- terminate

For industrial interrupts we need something closer to:

- `request_interrupt(mode, payload)`
- `poll_interrupt_state()`
- `supports_resume()`

We should still keep the public programming model simple. Most node authors
should not implement policy logic themselves. They should only advertise and
honor capabilities.

## 3. Kernel Client and Motion Adapters

This is the most safety-critical implementation area.

Required capabilities:

- stop active motion
- hold active motion if supported
- publish execution snapshot rich enough to correlate interrupt timing
- expose adapter events as interrupt sources

Without reliable kernel stop/hold semantics, interrupt policies collapse into
best-effort software wishes.

## 4. Blackboard and State Management

Interrupt handler execution must not corrupt normal program state.

We need:

- explicit interrupt context namespace on the blackboard
- deterministic promotion rules if handlers publish values back to the parent
  context
- a record of interrupted node/subtree state if resume is attempted

Resume should be treated as opt-in, not assumed.

## 5. Composer UI

Composer needs interrupt authoring and visibility:

- view deployment interrupt sources
- define program interrupt policies
- attach handler chains
- see node interrupt capabilities
- inspect interrupt events in trace/history

The UI must also make it obvious when a handler path is not resumable or when a
policy masks another lower-priority policy.

## 6. OpenClaw / Recovery Integration

Interrupts should integrate with, but not depend on, OpenClaw.

Good division:

- runtime interrupt detection and first-line handler execution remain ManyForge
  native
- delegated diagnosis/escalation may be one possible handler outcome

That keeps safety-critical behavior deterministic while still allowing
OpenClaw-backed workflows after the interrupt is stabilized.

## Proposed Delivery Phases

## Phase 1: Runtime Stop Interrupts

Implement the minimum safe path:

- interrupt sources in deployment
- program interrupt policies
- `abort_active_node` / `hold_cycle`
- handler chains that can call deterministic stop-safe actions
- interrupt events in recorder

This phase should target motion stop, timeout abort, and operator pause/stop.

## Phase 2: Handler Trees and Resumable Holds

Add:

- dedicated interrupt handler subtrees
- resumable hold state
- better Composer authoring support

## Phase 3: Capability-Driven Resume and Supervisory Escalation

Add:

- node capability metadata
- explicit resumability contracts
- OpenClaw-assisted escalation as a non-primary handler outcome

## Design Constraints

### 1. Do Not Hide Safety Logic Inside LLM Flows

Interrupt first response must stay deterministic. An LLM may assist diagnosis or
escalation later, but it must not be the primary interrupt controller.

### 2. Do Not Overload `stop()`

Generic cycle stop is not enough. Interrupts need typed semantics:

- stop
- hold
- abort subtree
- latch fault

### 3. Resume Must Be Explicit

Auto-resume after an interrupt should be rare and capability-driven. It must
not be the default.

### 4. Policy Must Be Centralized

Interrupt policy belongs in deployment/program configuration, not scattered
across arbitrary node code.

## Recommended Next Step

Implement a narrow v1 that solves the real industrial gap without over-design:

1. deployment-defined interrupt sources
2. program-defined interrupt policies
3. runtime support for `abort_active_node`, `hold_cycle`, and `halted`
   dispositions
4. deterministic handler subtrees
5. interrupt event recording

That will give ManyForge a real interrupt model and a clean base for later
resume/escalation work.
