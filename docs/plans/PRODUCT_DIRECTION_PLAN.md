> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Product Direction Plan

This plan records the product and engineering direction that ManyForge should
follow over the next few months. It is intentionally broader than the
LLM-specific and OpenClaw-specific plans.

This document exists to answer a practical question:

- what should ManyForge become to be credible in industrial robotics,
  compelling at ROSCon 2026, and strong enough to serve as the flagship of the
  next phase of work

This plan does not replace:

- [`../../README.md`](../../README.md) for user-facing project entry
- [`LLM_OPERABILITY_PLAN.md`](LLM_OPERABILITY_PLAN.md) for ManyForge-internal
  LLM/runtime behavior
- [`OPENCLAW_INTEGRATION_PLAN.md`](OPENCLAW_INTEGRATION_PLAN.md) for
  NemoClaw/OpenClaw integration
- [`INTERRUPT_MODEL_PLAN.md`](INTERRUPT_MODEL_PLAN.md) for detailed interrupt
  execution-model work
- [`INDUSTRIAL_READINESS_GAP.md`](INDUSTRIAL_READINESS_GAP.md) for the current
  gap register between the present baseline and industrial readiness
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md) for local
  bring-up and operations

## Readiness Baseline

This document is the roadmap and positioning layer.

For the current industrial-readiness gap register, use:

- [`INDUSTRIAL_READINESS_GAP.md`](INDUSTRIAL_READINESS_GAP.md)

That document is the canonical source for what is still missing right now in:

- external integration
- qualification maturity
- GUI qualification
- compliance and governance hardening

## Scope

This plan covers:

- product positioning for ManyForge
- the engineering order of operations for the next 5 months
- the work needed to make ManyForge suitable for real industrial manipulation
  applications
- the role of ROS, PLC integration, perception, and physical AI in that story

This plan does not cover:

- detailed schema-level design for every subsystem
- historical sprint notes
- sandbox transport details

## Product Thesis

ManyForge should become a deterministic industrial application runtime for
manipulation, with bounded physical-AI extensions.

It should sit between:

- low-level robot execution and planning
- ROS2 ecosystems
- PLC / signal / cell-level automation
- perception and scene understanding
- bounded AI assistance

It should not become:

- a generic robotics playground
- a simulator-first framework
- an agent shell that bypasses execution authority
- a loose collection of demos without a strong runtime model

## Strategic Positioning

### 1. Industrial runtime authority comes first

ManyForge should own:

- application execution semantics
- behavior orchestration
- scene and task-level manipulation flows
- bounded runtime state and auditability
- explicit authority boundaries for intervention, recovery, and AI usage

This is the foundation. Physical AI is an extension on top of this runtime, not
its replacement.

### 2. ROS and PLC interoperability are core, not peripheral

The near-term public story is not just “ManyForge plus AI.”

It is:

- ManyForge as the application layer for industrial manipulation
- interoperable with ROS2
- interoperable with PLC-oriented factory control paths
- able to fit into Siemens / ROXSIE-style deployments without giving up runtime
  clarity

### 3. Perception must become a real subsystem boundary

Browser-backed HMI snapshots are not a production perception path. The long-term
shape should be:

- a dedicated perception/service boundary
- ROS2-compatible when ROS is the right integration path
- usable without ROS when a simpler deployment is justified
- capable of consuming modern vision pipelines without destabilizing the
  deterministic execution layer

### 4. Physical AI should be bounded and auditable

ManyForge should support physical-AI-assisted application building and runtime
reasoning only where:

- the contract is explicit
- side effects are reviewable or bounded
- deterministic fallbacks remain possible
- auditability is preserved

### 5. External architecture references support layering, not redirection

Recent embodied-agent work around OpenClaw, including *ABot-Claw: A Foundation
for Persistent, Cooperative, and Self-Evolving Robotic Agents*, is useful as an
external signal that the right long-term structure is layered:

- execution/application authority
- shared services such as perception, memory, and evaluation
- supervisory/agentic orchestration

For ManyForge, that reinforces the current direction:

- ManyForge should stay the industrial execution and application layer
- shared services should become explicit subsystem boundaries
- OpenClaw-style orchestration should remain above that layer, not replace it

Reference:

- https://arxiv.org/abs/2604.10096

## 5-Month Objective

By ROSCon 2026, ManyForge should be able to support and demonstrate:

- real manipulation applications built on a strong behavior/runtime substrate
- ROS2 integration that looks native rather than bolted on
- PLC/signal-aware orchestration that aligns with Siemens and ROXSIE narratives
- credible observability and traceability
- at least one perception-guided application path

The story should be:

- ManyForge is the industrial manipulation application layer
- ROS2 and PLC systems plug into it cleanly
- perception and bounded AI augment it without eroding determinism

## Priority Order

1. Industrial execution core
2. ROS2 and PLC interoperability
3. Real application building blocks and examples
4. Perception / vision pipeline boundary
5. Physical AI and assistant workflows

This order is intentional. The system should first be good at building and
running real applications before it tries to become broader or more agentic.

## Workstreams

### Workstream A: Industrial execution core

This is the highest-priority work.

Required capabilities:

- signal handling
- interrupts
- gripper abstraction
- explicit IO/event semantics
- reusable behavior patterns for real applications

#### A1. Signals and IO model

Still needed:

- typed signal model for inputs, outputs, and events
- level-triggered and edge-triggered semantics
- wait/guard/trigger nodes
- mapping between ManyForge signals and ROS / PLC / operator-originated events
- traceable signal transitions

Why this is critical:

- industrial cells are not driven by motion requests alone
- PLC coordination and operator-facing automation require explicit signal logic
- real applications need waits, guards, interlocks, acknowledgements, and
  handshakes

Current execution-pass decision:

- v1 signal handling will build on declared typed blackboard keys plus
  signal-specific nodes, rather than introducing a separate top-level `signals`
  artifact immediately
- this keeps authored/runtime semantics consistent with the typed binding model
  already in place while still giving applications an explicit signal-oriented
  surface
- a runtime signal registry/provider substrate now exists for external adapter
  integration, including:
  - adapter-scoped signal publication
  - provider health tracking
  - freshness/staleness handling
  - fail-closed blackboard mirroring for stale/disconnected signals
- the remaining review item is not the registry itself, but the deployment/authored
  contract that future ROS/PLC/ROXSIE adapters will target

#### A2. Interrupts

Still needed:

- deterministic interrupt sources
- interrupt policies at program level
- abort/hold/resume semantics
- handler trees with explicit authority boundaries
- runtime and audit visibility for interrupt cause and resolution

This work is tracked in detail in [`INTERRUPT_MODEL_PLAN.md`](INTERRUPT_MODEL_PLAN.md),
but it is a product-level priority, not a side project.

Current execution-pass decision:

- v1 interrupt work now includes explicit deployment/program schema, trace
  surfaces, conservative `CycleManager` arbitration, and named handler-tree
  execution for safe resource claims
- deployment owns `interrupt_sources`
- program owns `interrupt_policies` and `interrupt_handlers`
- catalog entries should expose interruptibility metadata so policy authoring
  does not rely on implicit node-type assumptions
- execution history should expose a stable trace envelope rich enough to align
  with `ros2_medkit`-style observability and later interrupt causality
- handlers that claim unsupported domains such as `motion`, `scene`, or
  `model` remain blocked and latched until stronger preemption/resource
  contracts are implemented

#### A3. Gripper and end-effector abstraction

Still needed:

- pluggable gripper representation
- end-effector-specific URDF/config packaging instead of hardwiring it into each
  robot description
- scene-visible gripper state and graspable-object interactions
- clean attach/detach/contact semantics
- support for swapping end-effectors without rewriting application logic

Why this matters:

- ManyForge should be able to build real manipulation applications, not just arm
  trajectory demos
- end-effector behavior is a first-class part of the application layer

Current execution-pass decision:

- v1 gripper abstraction is a logical-state layer with explicit gripper-state
  nodes and declared state keys
- attachment/detachment remains explicit scene authority through
  `attach_object_to_link` / `detach_object_from_link`
- pluggable URDF packaging and true hardware-driver abstraction remain later
  review items, not solved by this pass

#### A4. Reusable behavior base

Still needed:

- stronger library of industrial manipulation patterns
- reusable pick/place, machine-tending, inspection, and recovery structures
- clearer separation between low-level motion nodes and application-level skills
- examples that demonstrate the intended engineering style, not just isolated
  node usage

### Workstream B: ROS2 and PLC interoperability

This is the second priority because it is central to both adoption and the
ROSCon 2026 story.

#### B1. ROS2 wrappers and integration surface

Still needed:

- ManyForge-facing ROS2 wrappers for topics, services, actions, and structured
  state exchange
- examples that look like real ROS2 applications rather than ad hoc bridges
- a clear decision on what belongs in ManyForge vs what remains in ROS-only
  infrastructure

The target is not “wrap everything in ROS2.” The target is a clean contract
between ManyForge application semantics and ROS2 communication/runtime surfaces.

#### B2. PLC-oriented interoperability

Still needed:

- signal and event mapping that can align with PLC-driven automation
- examples shaped around coordination with external control authority
- ROXSIE-relevant application flows where PLC/state changes drive ManyForge
  behaviors or interrupts

This should be designed so the Siemens / ROXSIE story is credible without
turning ManyForge into a PLC runtime.

#### B3. `ros2_medkit` compatibility

This is now an explicit direction, not an optional nice-to-have.

ManyForge has already leaned on `ros2_medkit` for observability and
traceability. Over the next few months, ManyForge should stay closely
compatible with it and cooperate cleanly with its model.

Required outcomes:

- keep observability and traceability concepts aligned with `ros2_medkit`
- avoid inventing a competing tracing story where `ros2_medkit` already offers
  a good substrate
- make ManyForge events, execution traces, and runtime diagnostics easy to
  relate to `ros2_medkit` timelines and tooling
- prefer integration points that help joint ROSCon 2026 presentation work with
  Siemens and future collaboration

Practical implication:

- when designing ManyForge event schemas, runtime traces, ROS wrappers, and
  execution diagnostics, treat `ros2_medkit` compatibility as a first-class
  design constraint

### Workstream C: Perception and vision pipeline

This is important, but should follow the execution and integration substrate.

#### C1. Perception boundary

Still needed:

- a dedicated perception/service boundary
- deterministic ingestion of perception outputs into ManyForge scene/task state
- typed outputs for selection, localization, or pose-estimation style results
- clear authority boundaries between perception output and execution decisions

#### C2. ROS and non-ROS paths

ManyForge should support:

- a ROS2-native perception integration path for industrial robotics stacks
- a non-ROS path when a simpler deployment is justified

This should be one conceptual subsystem with two integration modes, not two
unrelated perception strategies.

#### C3. NVIDIA and simulation-adjacent options

Given the broader stack context, future evaluation targets may include:

- Isaac ROS-backed perception pipelines
- Isaac Sim / Isaac Lab for development and validation workflows
- high-fidelity sidecar rendering or digital-twin options such as NVIDIA
  Omniverse modular libraries

These are candidates for future development, not replacements for ManyForge
runtime authority.

### Workstream D: Physical AI and assistant workflows

This work is valuable, but it should sit on top of a strong runtime substrate.

Still needed:

- application-building assistance that proposes edits rather than mutating trees
  silently
- bounded runtime model usage for well-scoped tasks
- delegated diagnosis and recovery where authority remains explicit
- AI-assisted application authoring that helps integrators build cells faster
  without weakening the deterministic base

The product message should be:

- ManyForge makes industrial manipulation application building easier
- AI helps where it is bounded and auditable
- AI does not replace runtime discipline

## What Not To Prioritize On The Critical Path

These areas may still matter, but they should not displace the core product
trajectory over the next few months:

- broad generic assistant UX before the industrial runtime gaps are closed
- replacing lightweight HMI/debug tooling with a heavy visualization stack
- making simulation the center of the product story
- adding more demos before the underlying runtime semantics are strong enough

The next milestone should be a smaller number of stronger applications, not a
larger number of weaker ones.

## Recommended Execution Order

1. Finish the industrial execution substrate:
   - signals / IO
   - interrupts v1
   - gripper abstraction
   - reusable behavior patterns
2. Build ROS2 and PLC-facing wrappers and examples.
3. Keep observability and traceability aligned with `ros2_medkit`.
4. Build the real perception boundary and one strong vision pipeline.
5. Layer assistant and physical-AI workflows on top.
6. Use ROSCon 2026 demos as validation targets, not as substitutes for product
   architecture.

## Flagship Demo Targets

The near-term demo portfolio should stay focused.

Recommended targets:

- machine tending or pick-place with signals and interrupts
- perception-guided pick with a real perception handoff
- ROS2 plus PLC-coordinated cell example aligned with Siemens / ROXSIE

These demos should be built to showcase the intended product shape:

- industrial orchestration
- application reuse
- observability and traceability
- bounded AI/perception augmentation

## Exit Criteria For This Direction

This plan is substantially satisfied when:

- ManyForge can support real manipulation applications with signals,
  interrupts, and pluggable end-effectors
- ROS2 and PLC-facing integration flows look productized rather than ad hoc
- observability and traceability stay meaningfully compatible with
  `ros2_medkit`
- at least one perception-guided application path is credible for industrial
  use
- the public story is clearly about industrial application building, not just a
  pile of demos or isolated AI features

## Related Documents

- [`LLM_OPERABILITY_PLAN.md`](LLM_OPERABILITY_PLAN.md)
- [`OPENCLAW_INTEGRATION_PLAN.md`](OPENCLAW_INTEGRATION_PLAN.md)
- [`INTERRUPT_MODEL_PLAN.md`](INTERRUPT_MODEL_PLAN.md)
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md)
