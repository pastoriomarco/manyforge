> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Isaac Sim Preview Plan

This is the tactical execution plan for the first simulator-backed ManyForge
preview.

It is not a peer strategic plan. It is a sub-plan of:

- [`PREVIEW_AND_ADOPTION_PLAN.md`](PREVIEW_AND_ADOPTION_PLAN.md)

It should also remain aligned with:

- [`PRODUCT_DIRECTION_PLAN.md`](PRODUCT_DIRECTION_PLAN.md)
- [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)

## Purpose

Use Isaac Sim as the first serious simulator-backed implementation of the
ManyForge preview/adoption story.

This does not mean Isaac-specific lock-in is the long-term target. It means:

- Isaac is the first concrete simulator path to validate the workflow
- the resulting design should remain compatible with a broader simulation
  interface strategy

## Goal

Reach a presentable simulator-backed ManyForge workflow that shows:

- a loaded Isaac Sim scene
- ManyForge reading the relevant scene state
- ManyForge reproducing the relevant scene/resources in its own backend/HMI
- assistant-backed generation of a usable ManyForge program
- execution of that program against the simulated robot and scene

## Architectural Constraints

The Isaac path must not:

- bypass ManyForge runtime authority
- create simulator-only program semantics
- depend on assistant-side uncontrolled mutation
- become a dead-end adapter stack that cannot inform broader simulator support

## Open Integration Decision

The simulator control and scene path is still open.

Candidate implementation paths:

### 1. Direct Isaac adapter path

Pros:

- avoids taking on ROS wrappers immediately
- can be narrower for the first implementation

Cons:

- may be more simulator-specific
- can drift if not designed carefully against a broader abstraction

### 2. ROS 2 wrapper path

Pros:

- aligns with existing ROS-oriented simulation ecosystems
- can potentially leverage cross-simulator standards such as
  `simulation_interfaces`

Cons:

- may pull ROS-wrapper work into the short-term scope earlier than desired
- may be broader than needed for the first proof

### 3. Hybrid path

Pros:

- allows direct work for narrow needs while keeping a future ROS path open

Cons:

- easiest way to create accidental complexity if boundaries are not explicit

Current decision:

- keep all three options open until the next implementation discussion
- explicitly treat ROS 2 `simulation_interfaces` as a strong candidate for the
  broader simulation path
- do not yet commit the preview implementation to the ROS-wrapper route

## Why `simulation_interfaces` Matters

`simulation_interfaces` is relevant because it is becoming a real
cross-simulator standard for simulator/world/entity interaction.

It appears to have current support in:

- Gazebo
- O3DE
- Isaac Sim

That makes it a strong candidate for a future ManyForge simulator adapter
contract.

But it is still a ROS-based path, so using it immediately would also mean
taking on ROS-wrapper work now. That may or may not be the right short-term
choice.

## Practical Work Breakdown

### Phase 0: Decide the minimum preview workflow

Lock the concrete use case:

- pick and place
- bin picking

Lock what “done” means:

- simulator scene can be queried
- ManyForge scene/resources can be reconstructed
- assistant can propose a usable program
- program can run end to end

### Phase 1: Define the simulator-facing capability slice

Define the minimum needed for the first preview:

- list/query scene entities
- get relevant poses and object state
- optionally spawn/reset/update scene entities
- query simulation state
- drive the simulated robot and gripper through the chosen control path

### Phase 2: Scene import/bootstrap path

Build deterministic tools that:

- inspect the simulator scene
- extract the subset relevant to ManyForge authoring
- reconstruct that subset in the ManyForge backend/HMI scene

### Phase 3: Assistant-backed authoring path

Use the deterministic scene tools to support:

- scene summarization
- proposal of ManyForge scene artifacts
- proposal of ManyForge program/tree artifacts
- review/apply in Composer

### Phase 4: Presentable scenario

Deliver one repeatable scenario with:

- gripper-aware manipulation
- clear runtime state
- visible scene alignment
- retained evidence of success/failure

## Relationship To The Broader Simulation Story

Isaac Sim is the first tactical simulator target.

It should also serve as research input for a broader future ManyForge
simulation path, which may later deserve explicit treatment in
`manyforge_specs` as a simulator adapter family.

That broader step is not required to execute the current preview plan, but this
plan must avoid blocking it.
