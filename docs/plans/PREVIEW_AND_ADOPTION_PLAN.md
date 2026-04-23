> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Preview and Adoption Plan

This is the short-term milestone plan for making ManyForge presentable and
useful before full industrial readiness is complete.

It is a detail layer between:

- the product roadmap in [`PRODUCT_DIRECTION_PLAN.md`](PRODUCT_DIRECTION_PLAN.md)
- the readiness/dependency plan in
  [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)

It must not conflict with either of them.

## Purpose

The goal is not “make a flashy demo.”

The goal is:

- define the first useful ManyForge story that people can try and understand
- make simulation a real adoption path, not just a teaser
- prove assistant-assisted authoring on top of the real ManyForge artifact and
  runtime model
- avoid short-term work that will later be thrown away

## Target Outcome

The target presentable workflow is:

1. a simulator such as Isaac Sim has a loaded scene
2. ManyForge reads the relevant scene state through deterministic tools
3. ManyForge reproduces the relevant scene/resources in its own backend/HMI
   model
4. a NemoClaw/OpenClaw-served assistant uses predefined tools to propose a
   ManyForge scene/program
5. the proposed program remains reviewable and editable in Composer
6. ManyForge runs the resulting program and controls the simulated scene

Reference use cases:

- pick and place
- bin picking

## Non-Goals

This milestone does not attempt to:

- prove full industrial readiness
- complete all real robot adapter families
- complete full ROS integration across the repo
- complete PLC/ROXSIE integration
- finalize the long-term perception architecture
- turn the assistant into a general autonomous runtime agent

## Hard Constraints

This plan is only valid if it preserves these boundaries:

- no simulator-only artifact model
- no assistant-side silent mutation
- no bypass of ManyForge execution authority
- no preview-only semantics that contradict the specs
- no browser-perception path presented as a production perception contract
- no throwaway adapter work that must later be deleted to follow the main plan

## Why This Fits The Specs

The current specs define ManyForge as:

- a motion governance layer
- a ROS-free core with adapter boundaries
- a portable program/deployment artifact system
- a runtime with explicit observability and traceability expectations

This plan therefore treats simulation as:

- a first-class deployment mode
- not a separate product architecture

## Main Tracks

### Track A: Simulation operator path

This track owns:

- scene reading and bootstrap
- simulator-side world/entity operations
- ManyForge-side scene/resource reconstruction
- simulator execution of the authored/generated program

Detailed tactical plan:

- [`ISAAC_SIM_PREVIEW_PLAN.md`](ISAAC_SIM_PREVIEW_PLAN.md)

### Track B: Assistant-backed authoring path

This track owns:

- scene summarization and interpretation
- proposal of ManyForge scene/program artifacts
- proposal/apply workflow in Composer
- bounded tool use and deterministic validation

Detailed domain plan:

- [`AI_ASSISTANT_INTEGRATION_PLAN.md`](AI_ASSISTANT_INTEGRATION_PLAN.md)

## Shared Technical Requirements

Both tracks must reuse the same:

- program and deployment artifact model
- scene/resource model
- runtime authority boundaries
- HMI and observability surfaces
- qualification and evidence story

That is what prevents the preview path from turning into dead-end work.

## Simulation Path Options

The simulator control path is still an open implementation decision.

Valid options include:

1. direct simulator adapter path
2. ROS 2 wrapper path
3. hybrid path

One especially relevant option is the emerging ROS 2
`simulation_interfaces` standard, because it already has support across
multiple simulators and could reduce simulator-specific throwaway work.

But this plan does **not** commit ManyForge to building the ROS wrapper path
immediately. It stays an option, not a locked implementation decision.

## Recommended Short-Term Order

1. define the concrete preview scenario and success criteria
2. restore and sharpen the Isaac/simulation tactical plan
3. implement the minimum assistant subset needed for scene/program proposal
4. build only the shared infrastructure both tracks need
5. qualify one repeatable scenario with retained evidence

## Exit Criteria

This milestone is successful when ManyForge can show:

- a useful simulation-operator workflow
- assistant-backed but reviewable authoring
- one or two repeatable scenarios that actually run
- behavior that aligns with the long-term runtime and artifact model rather
  than contradicting it
