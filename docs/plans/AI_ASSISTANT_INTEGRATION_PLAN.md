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

External model-serving profiles, memory budgets, and Thor/Orin deployment
experiments are tracked in NemoClaw-Thor:

- `/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor/MANYFORGE-ASSISTANT-DEPLOYMENT-PLAN.md`

That document decides how NemoClaw serves models. This ManyForge plan decides
what provider contract, tool contract, and workflow tests those models must
satisfy before being trusted by Composer or runtime workflows.
Current examples should use role-based endpoint aliases and resolve concrete
model IDs through deployment-owned profile registries that track the
NemoClaw-Thor-selected profiles.

## Current Baseline

Validated today:

- `query_model` exists as a catalog node
- bounded structured output is supported
- response-tool mode is supported and preferred for bounded selection tasks
- runtime blackboard contracts are typed and bundle-based
- deployment-configured model aliases are exposed in Composer
- deployment-configured model capabilities are parsed, validated, exposed in
  Composer, and surfaced as compatibility warnings
- deployment-configured assistant provider settings can select deterministic
  local mode or a NemoClaw/OpenClaw endpoint alias
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

Current status:

- deployment-visible capability flags exist in deployment artifacts
- compatibility warnings are shown in Composer for selected endpoint/model
  capabilities
- assistant-model policy and runtime-model policy are separated by deployment
  aliases and provider settings

Still needed:

- live provider capability negotiation against the active NemoClaw/OpenClaw
  endpoint
- live smoke coverage for response-tool support, image input, latency budget,
  and tool-call parser family using the NemoClaw-Thor-selected profile

Required fields include:

- `supports_response_tools`
- `supports_json_object`
- `supports_image_input`
- `supported_image_transports`
- optional `tool_call_family`

### 2. Composer assistant

Current status:

- a model-free `POST /api/assistant/chat` scaffold exists
- the route returns reviewable draft proposals only
- `mock` mode does not call NemoClaw/OpenClaw, invoke tools, or mutate Composer
  state; explicit `provider` mode can call a configured live adapter
- deterministic assistant tool endpoints exist under `/api/assistant/tools`
- the first tools are read-only Isaac scene inspection and non-mutating
  scene-resource proposal generation
- deterministic fixture-backed smoke coverage exists for the
  `inspect_isaac_scene` -> `propose_scene_objects` path, so normal tests do
  not require a live Isaac Sim process
- fixture-backed proposal lifecycle smoke coverage exists for
  `inspect_isaac_scene` -> `propose_scene_objects` -> `stage proposal` ->
  `materialize session patch`, proving the review/apply boundary without
  mutating the loaded program
- backend assistant provider routing exists:
  - `mock` mode remains the deterministic default
  - `provider` mode is explicit and fails closed when no provider is configured
  - provider results are normalized into messages, tool-call records, warnings,
    and reviewable proposals
  - `deterministic-scene-authoring` is available as a local stand-in for the
    future NemoClaw/OpenClaw tool loop; it consumes an Isaac scene report from
    request context and does not call live model or simulator services
  - `nemoclaw`/`openclaw` live HTTP provider adapters exist behind the same
    provider interface and reject mutation claims or malformed response records
  - a fake live HTTP provider fixture exists for exercising the real
    NemoClaw/OpenClaw adapter contract without running model inference,
    including negative mutation, malformed response, timeout, and HTTP-error
    cases
- v0.1 assistant mode plumbing exists on the ManyForge side:
  - deployment artifacts parse `assistant_modes` and `tool_catalog`
  - skill manifests parse optional `requires_tools`
  - deployment load/update paths fail closed on unknown mode skills/nodes,
    unknown tools, query-mode non-read-only tools, incompatible
    skill/tool requirements, and instrument ID namespace collisions
  - provider requests for mode-declared deployments carry
    `schemaVersion: "0.1.0"`, `assistantMode`, and the resolved mode catalog
  - provider requests and responses are rejected when they reference tools
    outside the active mode catalog
- backend proposal staging exists:
  - assistant proposals can be staged server-side without changing the program
  - scene-resource add proposals can be materialized into validated session
    patches for Composer's normal review/apply path
  - staged proposals can be listed and discarded
- deterministic tree-node proposal tooling exists for scene-object operations:
  - add collision object
  - remove collision object
  - attach object to link
  - detach object from link
  - generated nodes are proposals only; insertion point selection and program
    application remain explicit user-reviewed steps

Still needed:

- live NemoClaw/OpenClaw bridge endpoint smoke with the real model/tool loop
- model capability negotiation against the live endpoint, including
  response-tool support, image input, latency budget, and tool-call parser
  family
- NemoClaw/OpenClaw live provider validation against the same deterministic
  workflow harness before model-specific behavior is accepted
- Composer assistant panel
- frontend wiring for proposal review/materialize/apply/discard
- tree-edit proposal materialization with explicit insertion point selection
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
2. live bridge smoke coverage for model-backed `/api/assistant/chat`
3. Composer assistant panel and active-mode selection UI
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
