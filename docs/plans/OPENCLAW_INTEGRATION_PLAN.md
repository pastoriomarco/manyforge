> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# OpenClaw Integration Plan for ManyForge

This is the live integration plan for using NemoClaw/OpenClaw with ManyForge.
Historical sprint logs, superseded transport branches, and embedded skill drafts
have been removed. The remaining content is the current integration boundary,
the decisions already made, and the work that is still open.

This document is intentionally narrower than the old version:

- [`../../README.md`](../../README.md) remains the user-facing entrypoint
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md) is the
  operational runbook
- [`LLM_OPERABILITY_PLAN.md`](LLM_OPERABILITY_PLAN.md) owns ManyForge-internal
  LLM/runtime behavior
- [`../integration/PLATFORM_REQUIREMENTS.md`](../integration/PLATFORM_REQUIREMENTS.md)
  owns upstream platform gaps

## Scope

This plan covers:

- how ManyForge and NemoClaw/OpenClaw should interact
- which flows should route through OpenClaw and which should not
- the current mux-based baseline and the intended replacement path

It does not cover:

- all ManyForge LLM behavior
- generic stack bring-up
- detailed MCP schema reference

## Current Baseline

Validated today:

- ManyForge can run fully without NemoClaw/OpenClaw.
- NemoClaw/OpenClaw can talk to ManyForge through the current mux-based path.
- The current validated sandbox path still relies on:
  - `inference.local`
  - the mux proxy
  - generated workspace plugin artifacts
- ManyForge runtime `query_model` calls in the semantic demo are still direct to
  the configured model endpoint, not routed through OpenClaw.
- The current ManyForge runtime blackboard contract is typed and bundle-based:
  declared `namespace_ref` / `key_ref` / `field_ref` bindings on the authored
  side, root namespace bundles with `meta` and `value` on the runtime side.

This is deliberate. The current architecture is hybrid, not all-through-OpenClaw.

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
- low-latency deterministic-ish runtime tasks

We are not forcing every runtime LLM request through OpenClaw.

### 2. ManyForge must remain operable without OpenClaw

Deterministic and non-LLM execution remains a first-class mode. OpenClaw is an
integration layer, not a hard dependency for the whole product.

### 3. The current mux is a bridge, not the desired final architecture

The mux path is acceptable as a current workaround because it validates the
integration concept. It is not the industrial target:

- it couples tool traffic to inference routing
- it is topology-sensitive
- it is harder to explain, audit, and operate than a native service route

## Integration Modes

### Mode A: Composer assistant

Desired shape:

- OpenClaw-hosted assistant
- assistant model separate from runtime node models
- structured proposal/apply flow into Composer

Status:

- not implemented yet in ManyForge UI/backend

### Mode B: Delegated recovery

Desired shape:

- ManyForge raises a typed pending recovery request
- OpenClaw agent diagnoses and proposes/executes an allowed intervention
- authority and resume policy remain explicit

Status:

- policy direction is converged
- full end-to-end typed recovery path is not yet complete

### Mode C: Supervisory operator

Desired shape:

- external OpenClaw-driven monitoring and diagnosis
- non-invasive visibility into state, history, blackboard bundles, and failures

Status:

- partial via existing ManyForge APIs and skills
- still not packaged as a clean productized flow

### Mode D: Per-cycle runtime perception via OpenClaw

Current decision:

- not the primary path
- bounded runtime perception inside ManyForge may remain direct
- image/perception concerns should move toward a dedicated sidecar/service
  boundary rather than flowing through the current mux path
- future sidecar candidates may include ROS2-native acquisition paths and
  high-fidelity simulation/rendering backends such as NVIDIA Omniverse modular
  libraries, but those remain outside the OpenClaw transport core

## Open Work

### 1. Assistant integration

Still needed:

- assistant endpoint alias/config in ManyForge deployment
- `/api/assistant/chat`
- Composer panel
- structured edit proposals with explicit apply/dismiss

### 2. Native transport replacement for the mux

Desired end state:

- direct ManyForge route reachable from sandbox
- native MCP server configuration in embedded OpenClaw agent
- HTTP/SSE MCP transport instead of plugin emulation over completions

This work depends on the upstream/platform gaps tracked in
[`../integration/PLATFORM_REQUIREMENTS.md`](../integration/PLATFORM_REQUIREMENTS.md).

### 3. Delegated recovery productization

Still needed:

- typed recovery contracts
- stricter scope enforcement
- end-to-end recovery workflow validation
- clear operator escalation path

### 4. Capability and model-policy alignment

OpenClaw-facing flows need the same explicit model capability story as the
ManyForge runtime:

- tool-call compatibility must be declared, not guessed
- assistant model and runtime-node models must be independently configurable

## Risks And Boundaries

### The mux does not solve image routing

The current image/perception issues are not fixed by pushing traffic through the
mux. The mux is a tool/text transport workaround, not a media transport design.

### OpenClaw does not solve tick-thread safety

Tick-thread safety is a ManyForge runtime issue. Routing model calls through
OpenClaw does not fix blocking perception or synchronous runtime paths inside
the ManyForge process.

### One product, two authority layers

The integration only works if authority stays explicit:

- ManyForge remains the runtime execution authority
- OpenClaw remains the agent/orchestration authority

No hidden side effects, no silent runtime rewrites, no assistant-driven tree
mutation without explicit user/apply boundaries.

OpenClaw-side tooling should also treat ManyForge blackboard state as declared
artifacts and root namespace bundles, not as ad hoc raw key-path conventions.

## Execution Order

1. Finish Composer assistant integration on the ManyForge side.
2. Keep delegated recovery bounded and typed.
3. Replace mux-era transport assumptions only when upstream platform support is
   available.
4. Keep deterministic mode healthy throughout.

## Exit Criteria

This plan is substantially complete when:

- Composer assistant is usable through OpenClaw with explicit proposal/apply
- delegated recovery is typed, bounded, and auditable
- the mux is no longer required for native ManyForge access from sandboxed
  agents
- ManyForge still supports deterministic operation without OpenClaw

## Related Documents

- [`../integration/PLATFORM_REQUIREMENTS.md`](../integration/PLATFORM_REQUIREMENTS.md)
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md)
- [`LLM_OPERABILITY_PLAN.md`](LLM_OPERABILITY_PLAN.md)
- [`../guides/LLM_GUIDE.md`](../guides/LLM_GUIDE.md)
- [`../reference/MCP_SCHEMA.md`](../reference/MCP_SCHEMA.md)
