> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge AI and Assistant Integration Plan

This is the live plan for bounded AI, assistant, and OpenClaw/NemoClaw
integration around ManyForge.

It replaces the old split between:

- ManyForge-internal LLM operability
- OpenClaw-specific integration planning

The goal is to keep one clear boundary:

- what stays inside the ManyForge runtime
- what belongs to assistant or supervisory layers
- what is active priority versus optional or deferred

## Scope

This document covers:

- bounded model use inside ManyForge runtime nodes
- Composer-side assistant integration
- OpenClaw/NemoClaw integration boundaries
- delegated recovery and supervisory workflows
- runtime-safety implications of model and perception integration

It does not cover:

- the broader product roadmap
- industrial-readiness proof work

Use these for that:

- [`PRODUCT_DIRECTION_PLAN.md`](PRODUCT_DIRECTION_PLAN.md)
- [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)
- [`PREVIEW_AND_ADOPTION_PLAN.md`](PREVIEW_AND_ADOPTION_PLAN.md)

## Current Baseline

Validated today:

- `query_model` exists as a catalog node
- bounded structured output is supported
- response-tool mode is supported and preferred for bounded selection tasks
- runtime blackboard contracts are typed and bundle-based
- deployment-configured model aliases are exposed in Composer
- the text-only semantic selection demo works
- ManyForge can run fully without OpenClaw/NemoClaw
- current integrated agent flows still rely on the mux-based workaround
- ManyForge runtime `query_model` calls are still direct to the configured
  endpoint, not forced through OpenClaw

## Settled Decisions

### 1. Hybrid routing is the target

OpenClaw/NemoClaw should own:

- Composer assistant
- delegated recovery
- supervisory diagnosis
- multi-step tool-using workflows

ManyForge may still call models directly for:

- bounded runtime nodes
- strict structured/response-tool calls
- low-latency runtime tasks with explicit validation

We are not forcing every model request through OpenClaw.

### 2. ManyForge must remain operable without OpenClaw

Deterministic and non-LLM execution remains a first-class mode.

### 2A. Assistant work does not replace the real robot and ROS substrate

Per the specs, the industrial adapter story spans execution, robot state,
controller management, gripper/tooling, perception hooks, and observability.

Assistant integration is not allowed to become a substitute for that work. It
comes after the real robot adapter substrate is credible enough that proposals,
recovery, and diagnosis operate against real runtime surfaces.

### 2B. Simulation is a valid proving ground for assistant work

Simulation is not just a teaser for later real robot capability.

It is a valid place to productize:

- scene understanding and import
- proposal/apply authoring workflows
- bounded tool use
- qualification of assistant-assisted flows

The constraint is the same:

- simulation-side assistant work must still use the normal ManyForge artifact
  and runtime boundaries so it remains reusable later

### 3. Response tools beat free-form JSON for bounded runtime tasks

For runtime tasks like semantic object selection, the preferred contract is:

- backend builds a deterministic candidate set
- model is forced into a strict structured contract
- backend validates the result deterministically

### 4. Browser-backed perception is not the production path

The HMI/browser render path is useful for demos and debugging. It is not an
accepted industrial image source for runtime logic.

### 5. Tick-thread safety is non-negotiable

No unbounded external I/O should run directly on the BT tick thread.

### 6. Assistant changes must remain explicit

The Composer assistant should propose structured edits. It should not mutate
trees silently.

## Main Themes

### 1. Model capability contracts

Still needed:

- deployment-visible capability flags
- compatibility checks in Composer
- clear separation between assistant-model policy and runtime-model policy

Required fields include:

- `supports_response_tools`
- `supports_json_object`
- `supports_image_input`
- `supported_image_transports`
- optional `tool_call_family`

### 2. Composer assistant

Still needed:

- deployment-level assistant endpoint alias
- `POST /api/assistant/chat`
- Composer assistant panel
- structured proposal/apply flow for tree edits
- practical scene-to-program proposal flows useful for simulation-backed
  adoption work

### 3. Delegated recovery

Still needed:

- typed recovery contracts
- stricter scope enforcement
- end-to-end recovery workflow validation
- clear operator escalation path

### 4. Runtime hardening

Still open:

- review/off-thread isolation for remaining heavy synchronous planning paths
- process-level worker isolation as the hard-recovery target
- better preflight observability when a run fails before useful node progress

### 5. Perception boundary

Expected direction:

- keep text-only semantic selection as the supported baseline
- stop expanding browser-backed runtime image features as if they were
  production paths
- move real image input to a dedicated sidecar/service boundary
- prefer ROS2-side acquisition for real hardware
- keep future sim/rendering sidecars as candidates, not as runtime authority

### 6. Transport cleanup

The current mux path is a bridge, not the desired final architecture.

Desired end state:

- direct ManyForge route reachable from sandbox
- native MCP server configuration in the embedded agent
- HTTP/SSE MCP transport instead of plugin emulation over completions

This depends on:

- [`../integration/PLATFORM_REQUIREMENTS.md`](../integration/PLATFORM_REQUIREMENTS.md)

## Priority Order

### Core now

1. continued runtime hardening for remaining synchronous work
2. deployment-level model capability fields and Composer warnings
3. assistant endpoint configuration and `/api/assistant/chat`
4. structured proposal/apply flow in Composer

### Next

1. delegated recovery productization
2. assistant/runtime model-policy alignment
3. clearer perception-sidecar contract
4. transport cleanup when the upstream platform supports it

### Optional or deferred

1. browser-backed image capture as a normal runtime feature
2. forcing all runtime model calls through OpenClaw
3. transport replacement before upstream platform support exists

## Execution Order

1. keep deterministic mode healthy throughout
2. harden runtime boundaries before expanding model-perception features
3. deliver capability contracts and assistant configuration
4. finish the Composer assistant proposal/apply flow
5. support the preview/adoption milestone with bounded scene/program proposal
   workflows
6. productize delegated recovery with explicit authority and escalation
7. replace mux-era transport assumptions only when upstream support exists

## Exit Criteria

This plan is substantially complete when:

- Composer assistant is usable with explicit proposal/apply
- delegated recovery is typed, bounded, and auditable
- bounded runtime model use is governed by explicit capability contracts
- ManyForge still supports deterministic operation without OpenClaw
- the mux is no longer required for native agent access
