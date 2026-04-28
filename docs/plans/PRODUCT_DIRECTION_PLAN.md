> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Product Direction Plan

This is the top-level roadmap for ManyForge.

Use this file to answer the practical product question:

- what ManyForge should become over the next phase of work
- what is core, what is next, and what is optional
- what the first public version should prioritize

This document is intentionally broader than the subsystem plans.

Use these companion plans for the detailed execution layers:

- [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)
- [`PREVIEW_AND_ADOPTION_PLAN.md`](PREVIEW_AND_ADOPTION_PLAN.md)
- [`AI_ASSISTANT_INTEGRATION_PLAN.md`](AI_ASSISTANT_INTEGRATION_PLAN.md)
- [`INTERRUPT_MODEL_PLAN.md`](INTERRUPT_MODEL_PLAN.md)
- [`../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md`](../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md)
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md)

## Product Thesis

ManyForge should become a deterministic industrial application runtime for
manipulation, with bounded AI and perception extensions layered on top.

It should sit between:

- robot execution and planning
- ROS2 ecosystems
- PLC / signal / cell-level automation
- perception and scene understanding services
- bounded assistant and supervisory workflows

It should not become:

- a generic robotics playground
- a simulator-only framework
- an agent shell that bypasses execution authority
- a loose collection of demos without a strong runtime model

## Strategic Positioning

### 1. Runtime authority comes first

ManyForge should own:

- application execution semantics
- scene and task-level orchestration
- bounded runtime state and auditability
- explicit intervention and recovery boundaries

AI, perception, and supervisory layers should extend this runtime, not replace
it.

### 2. ROS and PLC interoperability are core

The serious public story is not “ManyForge plus AI.”

It is:

- ManyForge as the industrial manipulation application layer
- interoperable with ROS2
- interoperable with PLC-oriented factory control paths
- able to fit into Siemens / ROXSIE-style deployments without giving up runtime
  clarity

Important constraint:

- ROS integration is not a narrow signal-adapter task
- per the specs, it is a repo-wide adapter story spanning execution,
  controller management, robot-state/recovery, gripper/tooling, observability,
  and later IO/perception integration
- PLC and ROXSIE work comes after the real robot control and observability
  substrate exists

### 3. Perception must become a real subsystem boundary

Browser-backed HMI capture is useful for demos and debugging. It is not the
production perception path.

The long-term shape should be:

- a dedicated perception/service boundary
- ROS2-compatible when ROS is the right integration path
- usable without ROS when a simpler deployment is justified
- cleanly separated from deterministic execution authority

### 4. Simulation is a first-class deployment mode

Simulation is not just a temporary demo target.

ManyForge can become genuinely useful for:

- simulator scene operation
- scenario automation
- assistant-backed program authoring
- regression and qualification workflows
- sim-first application development before hardware availability

The constraint is:

- simulation work must preserve cross-compatibility with real deployments as
  much as possible
- simulator-specific paths must not introduce throwaway artifacts or bypass the
  normal runtime authority model

Current technical reference for that boundary:

- [`../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md`](../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md)

### 5. AI must stay bounded and auditable

ManyForge should support assistant and physical-AI workflows only where:

- contracts are explicit
- side effects are bounded or reviewable
- deterministic fallbacks remain possible
- auditability is preserved

## Near-Term Objective

The next presentable ManyForge version should show:

- a credible manipulation application runtime
- a useful simulation-operator story
- a real gripper-aware embodiment in simulation
- clear state, errors, and traceability
- a useful but bounded assistant workflow

The first public version does not need full industrial proof. It does need a
credible product shape.

## Priority Bands

### Core now

These themes are on the critical path:

1. industrial execution substrate
2. real robot adapter substrate
3. one polished preview and adoption path
4. qualification and readiness proof

### Next

These matter, but should not destabilize the core path:

1. repo-wide ROS integration on top of the real robot substrate
2. assistant integration
3. delegated recovery
4. production perception boundary definition

### Optional or deferred

These are useful, but not on the immediate critical path:

1. browser-backed runtime image features
2. broad simulator-first expansion
3. forcing all model traffic through an agent layer
4. general “physical AI” breadth before the runtime and adapter story is solid
5. PLC/ROXSIE-specific wiring before the robot-control substrate is complete

## Main Workstreams

### Workstream A: Industrial execution and readiness

This is the main delivery track.

Priority content:

- execution semantics and runtime authority
- real robot adapter contracts
- observability and traceability at the adapter boundary
- interrupt proof at the real integration boundary
- verification and qualification expansion
- GUI/browser qualification
- auditability and compliance posture

Detailed execution plan:

- [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)

### Workstream B: ROS2 and PLC interoperability

This is part of the core path, not an optional add-on.

But the dependency order matters.

Priority content, in order:

1. repo-wide ROS integration for the real robot control surface:
   - execution backend / controller interface
   - robot state / recovery
   - controller management
   - gripper / tooling
   - observability and trace export
2. only after that:
   - IO / GPIO signal adapters
   - PLC-facing permit/fault/ack flows
   - ROXSIE-specific mapping
3. later:
   - perception / pose-source adapters where they support real applications

This should be delivered as part of industrial readiness, not as a separate
demo branch.

### Workstream C: First public preview

The preview track should now be treated as a product milestone with its own
execution detail, not as a peer architecture plan and not as a loose demo
branch.

Target shape:

- one useful simulation-operator workflow
- one real robot embodiment in simulation
- gripper-aware manipulation
- useful Composer/HMI and assistant workflow
- repeatable demo and qualification evidence

High-level steps:

1. define the preview/adoption milestone and success criteria
2. confirm the simulation embodiment/integration path from local evidence
3. keep the gripper-aware manipulation baseline
4. implement a narrow scene-bootstrap / authoring-assistant workflow
5. polish one repeatable, presentable scenario

This work should support the broader product direction, not fork it.

Detailed execution plans:

- [`PREVIEW_AND_ADOPTION_PLAN.md`](PREVIEW_AND_ADOPTION_PLAN.md)
- [`ISAAC_SIM_PREVIEW_PLAN.md`](ISAAC_SIM_PREVIEW_PLAN.md)

### Workstream D: AI and assistant integration

This remains important, but it is not allowed to outrun the runtime and
adapter substrate.

Priority content:

- model capability contracts
- Composer assistant with proposal/apply
- delegated recovery with explicit authority
- runtime hardening for model/perception-adjacent work
- perception/service boundary definition

Detailed execution plan:

- [`AI_ASSISTANT_INTEGRATION_PLAN.md`](AI_ASSISTANT_INTEGRATION_PLAN.md)

## Recommended Execution Order

1. keep finishing the industrial execution and readiness substrate
2. build the real robot adapter substrate:
   - execution
   - robot-state/recovery
   - controller management
   - gripper/tooling
   - observability
3. deliver the preview/adoption milestone without violating the same runtime
   and artifact boundaries
4. build repo-wide ROS integration on top of the substrate where required
5. only then add PLC/ROXSIE and richer signal-facing industrial control work
6. prove the stack through qualification and one polished public preview
7. add broader assistant integration without weakening runtime authority
8. move perception toward an explicit sidecar/service boundary
9. expand broader AI and supervisory workflows only after the above is stable

## Exit Criteria For This Plan

This roadmap is in a good state when:

- the active plan set is small and non-overlapping
- ManyForge has one clear industrial story
- the public-preview target supports that story instead of distracting from it
- AI and perception work are framed as bounded extensions, not as the product
  center of gravity
