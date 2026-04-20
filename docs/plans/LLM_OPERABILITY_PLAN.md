> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# LLM Operability Plan for ManyForge

This is the live plan for LLM-facing functionality inside ManyForge itself.
Historical incident notes, cleared audit items, and superseded design branches
have been removed. The goal here is simple: record the current baseline,
document the decisions that are already settled, and track only the work that is
still open.

This plan does not replace:

- [`../../README.md`](../../README.md) for user-facing entry
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md) for bring-up
  and operations
- [`OPENCLAW_INTEGRATION_PLAN.md`](OPENCLAW_INTEGRATION_PLAN.md) for
  NemoClaw/OpenClaw-specific integration
- [`../integration/PLATFORM_REQUIREMENTS.md`](../integration/PLATFORM_REQUIREMENTS.md)
  for upstream platform gaps
- [`../guides/LLM_GUIDE.md`](../guides/LLM_GUIDE.md) and
  [`../reference/MCP_SCHEMA.md`](../reference/MCP_SCHEMA.md) for agent-facing
  usage/reference

## Scope

This document covers:

- bounded model use inside ManyForge runtime nodes
- Composer-side model configuration and assistant work
- runtime safety implications of model/perception integration
- model capability contracts and auditability

This document does not cover:

- general stack bring-up
- OpenClaw transport internals
- user onboarding

## Current Baseline

### Implemented

- `query_model` exists as a catalog node.
- Bounded structured output is supported.
- Response-tool mode is supported and is the preferred contract for bounded
  selection tasks.
- Model-facing namespace outputs now publish as a single root blackboard bundle
  with explicit `meta` and `value` content, not parallel `response/status/...`
  keys.
- Selector-backed blackboard-aware params now use typed `namespace_ref`,
  `key_ref`, and `field_ref` bindings end to end.
- Deployment-configured model endpoint aliases are exposed in Composer.
- Composer can inspect deployment aliases and live `/v1/models` responses for
  OpenAI-compatible providers.
- The semantic selection demo works on the text-only path with
  `Qwen3.5-9B-Claude-Distilled-NVFP4`.
- Event recording, model-call telemetry, and audit structure are substantially
  improved compared with the original baseline.
- Browser-backed image capture no longer blocks the BT tick thread directly.

### Working, but not yet production-grade

- Per-node model selection through deployment aliases
- Response-tool enforcement for bounded tasks
- Text-only semantic scene-object selection
- Composer model/deployment editing for endpoint aliases

### Explicitly experimental

- `scene_snapshot_transport: inline_data_url`
- `scene_snapshot_transport: http_url`
- browser-backed runtime image capture

These image modes are placeholders for experimentation, not accepted runtime
perception paths.

## Settled Decisions

### 1. Hybrid model-routing architecture

ManyForge keeps a direct/provider-pluggable model path for bounded runtime calls
such as `query_model`. NemoClaw/OpenClaw is reserved for agentic flows:

- Composer assistant
- delegated recovery
- supervisory diagnosis
- multi-step/tool-using workflows

We are not forcing every ManyForge LLM call through OpenClaw.

### 2. Response tools over free-form JSON for bounded tasks

For runtime tasks like semantic object selection, the preferred contract is:

- deterministic candidate set built by backend code
- model forced into a response tool / strict structured contract
- deterministic validation on the result

Large free-form diagnostic JSON objects are not the target runtime interface.

### 3. Browser-backed perception is not the production path

The HMI/browser render path is useful for demos and debugging, but it is not an
acceptable industrial image source for runtime logic. Production perception is
expected to move to a dedicated sidecar/service boundary, likely tied to ROS2.

### 4. Tick thread must not perform blocking external I/O

The runtime contract is now explicit:

- no synchronous browser render capture on the BT tick thread
- no unbounded external I/O in node `initialise()` or `update()`

Some legacy synchronous work still exists and is tracked below.

### 5. Blackboard contracts are typed and bundle-only

The runtime/authored contract is now explicit:

- selector-backed blackboard-aware parameters are authored as typed bindings,
  not raw concrete path strings
- namespace-producing nodes publish one root bundle per namespace
- bundle consumers read through typed bindings or bundle-aware helpers

This is now the baseline contract for model/selection/planning flows, not an
optional migration layer.

## Open Work

### 1. Model capability contracts

We need deployment-visible capability flags so ManyForge can decide what a given
endpoint is actually allowed to do.

Required fields:

- `supports_response_tools`
- `supports_json_object`
- `supports_image_input`
- `supported_image_transports`
- optional `tool_call_family`

Why:

- Qwen response-tool support is verified.
- Gemma compatibility is not assumed.
- Composer should warn before assigning an incompatible model to a node.

### 2. Composer assistant

Still missing:

- deployment-level assistant endpoint alias
- `POST /api/assistant/chat`
- Composer assistant panel
- structured proposal/apply flow for tree edits

The assistant should not silently mutate trees. It should return validated edit
proposals that the UI can review and apply explicitly.

### 3. Runtime perception boundary

Current status:

- text-only selection path is the only accepted demo/runtime baseline
- inline/http image transport paths are experimental placeholders
- browser-backed snapshot acquisition is still unreliable
- HTTP image fetch remains topology-sensitive

Expected development:

- keep text-only semantic selection as the default demo path
- stop treating browser-backed image capture as a normal runtime feature
- move production image input to a dedicated sidecar/service boundary
- prefer ROS2-side acquisition for real hardware
- evaluate high-fidelity simulation/rendering sidecars as future alternatives
  for perception and synthetic-sensor workflows, including NVIDIA Omniverse
  modular libraries such as `ovrtx` / `ovphysx`

Evaluation boundary:

- these options are candidates for a future perception/digital-twin sidecar
- they are not candidates to replace ManyForge runtime execution authority
- they are not a replacement for lightweight HMI/debug rendering such as Viser

External architecture note:

- embodied-agent work such as *ABot-Claw: A Foundation for Persistent,
  Cooperative, and Self-Evolving Robotic Agents* reinforces the same boundary:
  perception, memory, and evaluation belong in shared services, not inside the
  deterministic execution core
- that is supporting evidence for moving image-based runtime capabilities out
  of browser/HMI paths and into explicit sidecar/service layers

Reference:

- https://arxiv.org/abs/2604.10096

### 4. Tick-thread and runtime hardening

Still open:

- `preview_motion()` and other heavy synchronous planning paths still need
  review/off-thread isolation
- process-level worker isolation remains the real hard-recovery target
- preflight observability is still incomplete when a run fails before useful
  node progress is visible

The browser-render hang class is mitigated, not the end of runtime hardening.

### 5. Spec and verification follow-through

Still open from the last audit/spec pass:

- catalog/spec governance for additive schema evolution and `ui_editor`
  changes going forward
- FastAPI-backed MCP route verification in the correct environment

These are no longer code-implementation blockers, but they are still required
for a clean contract story.

## Current Failure Modes To Treat As Known

### Browser-backed image capture

Known behavior:

- tick-thread wedging from browser render capture is fixed
- snapshot acquisition can still time out or disable itself
- inline/http image modes can still fail cleanly as ordinary node failures

Implication:

- image-enabled runtime demos remain experimental
- text-only demos are the supported baseline

### HTTP snapshot transport

Known behavior:

- model-reachable fetch URLs depend on topology
- a single global public base URL is not the right abstraction

Required direction:

- per-endpoint fetch-base configuration
- or removal of HTTP snapshot transport from normal runtime use

## Execution Order

### Next implementation batch

1. Add deployment-level model capability fields and Composer warnings.
2. Add assistant endpoint configuration and `/api/assistant/chat`.
3. Move Composer assistant to structured proposal/apply flow.
4. Continue runtime hardening for remaining synchronous planning work.

### Next perception batch

1. Keep text-only semantic selection as the default baseline.
2. Stop expanding browser-backed image features.
3. Define the ROS2/perception sidecar contract for image-based selection.
4. Evaluate candidate backends for that sidecar, including Omniverse modular
   libraries where high-fidelity rendering or synthetic sensing is needed.

## Exit Criteria

This plan is substantially complete when:

- Composer assistant exists with explicit proposal/apply semantics
- deployment model capabilities are declared and enforced in UI/runtime
- bounded runtime model calls use strict response tools where appropriate
- runtime image perception no longer depends on HMI/browser rendering
- remaining tick-thread safety gaps are closed or isolated behind a worker
  process boundary

## Related Documents

- [`../guides/LLM_GUIDE.md`](../guides/LLM_GUIDE.md)
- [`../reference/MCP_SCHEMA.md`](../reference/MCP_SCHEMA.md)
- [`OPENCLAW_INTEGRATION_PLAN.md`](OPENCLAW_INTEGRATION_PLAN.md)
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md)
- [`../integration/PLATFORM_REQUIREMENTS.md`](../integration/PLATFORM_REQUIREMENTS.md)
- [`../reference/REFERENCE.md`](../reference/REFERENCE.md)
