# LLM Operability Discussion

**Purpose**: reset the discussion around the current unresolved parts of the
LLM operability work. This document is for analysis and convergence. The
operative document remains `LLM_OPERABILITY_PLAN.md`.

**Companion document**: [LLM_OPERABILITY_PLAN.md](./LLM_OPERABILITY_PLAN.md)

**Current status**: CONVERGED — applied to plan
**Last update**: 2026-04-11
**Discussion focus**: only unresolved items that still require product,
architecture, or contract decisions.

---

## 1. How To Use This Document

- Treat this file as the discussion scratchpad.
- Treat `LLM_OPERABILITY_PLAN.md` as the operative plan.
- When a discussion point converges, apply the agreed change to the plan and,
  if needed, to the relevant spec documents.
- Do not reopen already-implemented items unless a new conflict, safety issue,
  or external requirement appears.

---

## 2. Reference Documents

These are the documents that should be cited when discussing or changing the
remaining open points.

### 2.1 Governing project rules

- [AGENTS.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/AGENTS.md)
  - precedence, ADR/spec discipline, testing expectations

### 2.2 Operative plan

- [LLM_OPERABILITY_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md)
  - roadmap, success criteria, and open points

### 2.3 Core specs already affected by the plan

- [100-component-model-and-interactions.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/100-component-model-and-interactions.md)
  - lifecycle, authority boundaries, component responsibilities
- [420-program-yaml-schema.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/420-program-yaml-schema.md)
  - program artifact schema, including node-instance metadata such as
    `modification_policy`
- [440-node-catalog-format-and-versioning.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/440-node-catalog-format-and-versioning.md)
  - node catalog contract and metadata rules
- [450-composer-ui-session-and-artifact-workflow.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/450-composer-ui-session-and-artifact-workflow.md)
  - Composer/HMI workflow, runtime-vs-saved artifact semantics
- [460-skill-catalog-and-intervention.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/460-skill-catalog-and-intervention.md)
  - skills, intervention sessions, delegated recovery, authority provenance

### 2.4 Integration reference

- [OPENCLAW_INTEGRATION_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/OPENCLAW_INTEGRATION_PLAN.md)
  - optional external orchestration layer
  - relevant for `query_model`, `delegate_to_llm`, MCP transport, operator
    escalation, and model/delegation observability

---

## 3. What Is Already Considered Settled

These areas should be treated as baseline unless new evidence contradicts them:

- scene state/edit, kernel snapshot/metrics, validation, history, digest, and
  debug export are part of the plan baseline
- intervention blackboard promotion exists and remains explicit-first
- intervention blackboard scoping is catalog-driven rather than hardcoded
- MCP response normalization uses snake_case
- direct control and intervention provenance are first-class requirements
- runtime overrides and delegated recovery exist as real product concepts
- ManyForge remains independently operable without OpenClaw
- ROS2 remains optional; if present, integration is sidecar-style rather than
  baked into the ManyForge core

The authoritative source for this settled baseline is
[LLM_OPERABILITY_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md).

---

## 4. Current Open Points

This section intentionally mirrors the still-open items in
`LLM_OPERABILITY_PLAN.md`, but adds context for discussion.

### 4.1 OP-4: Direct-control authority model is only partially defined

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#op-4-direct-control-authority-gating-is-now-partially-implemented](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#op-4-direct-control-authority-gating-is-now-partially-implemented)

**Related specs**:
- [100-component-model-and-interactions.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/100-component-model-and-interactions.md)
- [460-skill-catalog-and-intervention.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/460-skill-catalog-and-intervention.md)

**What is already decided**:
- `IDLE -> DIRECT_CONTROL` is not generally free-form.
- Human-approved `manual_intervention` is the conservative baseline.
- Saved-program execution, manual intervention, and delegated recovery must be
  distinguishable in provenance and history.

**What remains unresolved**:
- the full product-wide approval model
- whether any non-human `IDLE -> DIRECT_CONTROL` entry is allowed at all
- how approval tokens/policy grants should be represented across REST, MCP,
  HMI, and external orchestration

**Why this matters**:
- this is the boundary between deterministic runtime control and LLM authority
- a weak model here becomes a safety and audit problem, not just a UX problem

**Discussion prompt**:
- should approval be modeled as a simple mode bit, as explicit signed/granted
  intents, or as a richer policy object tied to deployment and operator role?

**Round 1 proposal**:

Model approval as **scoped authority grants**, not a mode bit and not a full
policy engine. Three concrete grant types:

| Grant type | Source | Scope | Lifetime |
|---|---|---|---|
| `human_approved` | Operator via REST/MCP/HMI | Single session | Expires on session close |
| `policy_granted` | `modification_policy.intervention_delegate` on a specific node instance | Single delegated-recovery session for that node's failure | Expires on session close or pending-request consumption |
| `deployment_granted` | Deployment artifact field (future) | Deployment-scoped, all sessions matching a declared pattern | Deployment lifetime |

Rules:

- `IDLE -> DIRECT_CONTROL` requires `human_approved` or `deployment_granted`.
  Never implicit.
- `STOPPED -> DIRECT_CONTROL` with `session_kind=manual_intervention` requires
  `human_approved`.
- `STOPPED -> DIRECT_CONTROL` with `session_kind=delegated_recovery` requires a
  matching pending `DelegatedRecoveryRequest` — the grant is `policy_granted`
  and the allowed authority sources come from the request itself.
- No global "LLM may act" switch. Authority is always scoped to a session and
  traceable to a grant.
- `deployment_granted` is a future extension for production deployments that
  want pre-approved autonomous patterns (e.g., "this deployment allows
  LLM-serviced recovery for any node with `llm_allowed`"). Not needed for
  Phase 3.

**Why**: A mode bit is too coarse — it cannot distinguish "operator approved
this specific intervention" from "the system is generally in autonomous mode."
A full policy engine is premature — we don't have enough deployment patterns
yet to design one well. Scoped grants are the middle ground: each session
carries its own provenance, and the authority surface is exactly as wide as the
grant, no wider.

**Impact on plan**: OP-4 remaining items become: (1) formalize the three grant
types and add `deployment_granted` as a reserved/future path, (2) update
intervention entry logic to validate grant type, not just `approval_mode`
string.

**Impact on specs**: 460 §6.2 and §6.3 need the grant-type taxonomy added. The
`approval_mode` field already exists in the session record — this proposal
formalizes its allowed values and enforcement semantics.

**Implementation impact**: Low for Phase 3. The current `approval_mode` field
on `InterventionSession` already carries `human_approved` / `policy_granted` /
`not_required`. The change is to make `CycleManager` entry logic enforce these
as checked grants rather than advisory labels. `deployment_granted` can remain
a reserved string until a deployment artifact field is designed.

**Risks / tradeoffs**: `deployment_granted` defers the "fully autonomous loop"
case. That's intentional — the first deployments should require explicit human
or node-policy grants. If a deployment-scoped autonomous grant is needed
sooner, it can be added to the deployment artifact schema without breaking the
session model.

### 4.2 OP-5: Runtime overrides exist, but the full product semantics are not done

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#op-5-node-level-editability-policy-and-runtime-overrides-are-partially-implemented](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#op-5-node-level-editability-policy-and-runtime-overrides-are-partially-implemented)

**Related specs**:
- [420-program-yaml-schema.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/420-program-yaml-schema.md)
- [450-composer-ui-session-and-artifact-workflow.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/450-composer-ui-session-and-artifact-workflow.md)
- [460-skill-catalog-and-intervention.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/460-skill-catalog-and-intervention.md)

**What is already decided**:
- runtime overrides are not hidden edits to the saved artifact
- `modification_policy` is part of the program contract
- active/staged override lifecycle matters and must be visible

**What remains unresolved**:
- final UI/HMI affordances for promoting/saving materialized overrides
- editing semantics when a loaded program already has active overrides bound
  into the runner
- whether controlled structural edits should ever be allowed, or whether the
  product stays at parameter-patch level only

**Why this matters**:
- this is the main path for “the saved move is no longer valid, adapt it now”
- if the runtime-vs-saved distinction becomes fuzzy, auditability collapses

**Discussion prompt**:
- should structural adaptation be allowed as a later capability, or should it
  remain explicitly out of scope in favor of node-level parametric repair only?

**Round 1 proposal**:

Stay at **parameter-patch level only** for now. Declare structural adaptation
as a future capability gated by an explicit ADR.

Rules for the current end-state:

- Runtime overrides remain per-node parameter patches. No tree topology changes
  through the override mechanism.
- If a loaded program already has active overrides when editing begins, the
  system must present a clear choice: **discard overrides** (edit the base
  program) or **materialize first** (promote overrides into the base, then
  edit). Never silently merge an override into an edit.
- HMI affordances needed: a visible "overrides active" indicator, a one-click
  promote/save, and a clear warning when starting edits against a program with
  active overrides.

**Why**: Structural adaptation (adding/removing/reordering nodes at runtime)
crosses a line from "patch a parameter" to "rewrite the program." That line is
exactly where auditability gets hard. The current parameter-patch model is
auditable because the diff is always a key-value map against a known node
instance. Structural changes would require tree-diff semantics, which adds
significant complexity with no current product need.

If structural adaptation becomes necessary (e.g., for contact-rich recovery
that needs to insert a retract move), it should be designed as a first-class
ADR with explicit safety analysis, not slipped in as "runtime overrides but
bigger."

**Impact on plan**: OP-5 remaining items narrow to: (1) HMI affordances for
promote/save, (2) edit-with-active-overrides conflict resolution UX. Both are
UI tasks, not architecture decisions.

**Impact on specs**: 460 §8.3 already says "runtime overrides are param-level
runtime patches." Add an explicit statement: "structural tree modifications are
not runtime overrides; they are program edits requiring the full authoring
workflow."

**Implementation impact**: Low. The current `apply_runtime_override` in
`state.py` already enforces per-node parameter patching. The main work is
HMI — showing override state clearly and handling the edit-with-overrides
conflict.

**Risks / tradeoffs**: If a recovery scenario genuinely needs to insert a node
(e.g., insert a retract move before retry), the parameter-patch model cannot
handle it. The fallback is: use an intervention session to run a temporary
subtree, not to modify the saved program's structure through overrides. This is
a deliberate constraint, not a gap.

### 4.3 OP-6: Delegated recovery exists as stop-and-request, but not yet as a full execution mode

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#op-6-delegated-recovery-has-a-first-policy-hook-implementation-but-not-the-full-product-shape](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#op-6-delegated-recovery-has-a-first-policy-hook-implementation-but-not-the-full-product-shape)

**Related specs**:
- [420-program-yaml-schema.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/420-program-yaml-schema.md)
- [460-skill-catalog-and-intervention.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/460-skill-catalog-and-intervention.md)
- [OPENCLAW_INTEGRATION_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/OPENCLAW_INTEGRATION_PLAN.md)

**What is already decided**:
- a saved program may raise a delegated-recovery request
- that request must carry provenance, scope, and authority constraints
- public delegated recovery should not be implicitly free-form

**What remains unresolved**:
- exact trigger taxonomy and mapping from runtime failures
- the product shape of `program_only` delegated recovery
- whether automatic in-tree recovery should use dedicated recovery nodes,
  `delegate_to_llm`, or both
- completion semantics:
  - retry
  - skip
  - apply runtime override
  - open draft patch
  - reset/stop

**Why this matters**:
- this is the bridge between deterministic execution and intelligent recovery
- if under-specified, it becomes an uncontrolled escape hatch

**Discussion prompt**:
- should `delegate_to_llm` be a generic node, or should recovery be expressed
  through narrower, typed recovery nodes with smaller authority surfaces?

**Round 1 proposal**:

Use **typed recovery nodes** with constrained authority surfaces, not a generic
`delegate_to_llm` node.

Recovery node taxonomy:

| Node | Authority surface | Phase |
|---|---|---|
| `retry_node` | Re-execute the failed subtree with optional parameter patch. No model call. | Phase 3 (deterministic) |
| `fallback_override_node` | Apply a pre-declared runtime override from a fallback parameter set. No model call. | Phase 3 (deterministic) |
| `recovery_skill_node` | Execute a cataloged skill from a pre-declared recovery skill list. No model call. | Phase 3 (uses skill catalog) |
| `query_model_node` | Single structured model query for perception/decision. Writes blackboard. No direct control. | Phase 6 (model-in-the-loop) |
| `delegate_to_llm_node` | Opens a scoped delegated-recovery session for an external agent. Full authority enforcement. | Phase 6 (external orchestration) |

The current stop-and-request model becomes the **external servicing path**:
when no in-tree recovery node handles the failure, the runtime stops with a
pending `DelegatedRecoveryRequest` and an external human or agent services it
via a `DIRECT_CONTROL` session.

Completion semantics (all recovery paths):

- `retry` — re-tick the failed subtree (with optional param patch applied)
- `apply_runtime_override` — apply a validated override and retry
- `open_draft_patch` — create a patch proposal without applying it; stop
- `stop` / `reset` — terminal

Note: `skip` is **rejected** as a recovery outcome. A failed node either gets
retried, overridden, patched, or the run stops. Silently marking a failed node
as SUCCESS violates process guarantees in industrial settings. Any future
reconsideration requires an explicit ADR.

**Why**: A generic `delegate_to_llm` node is an unbounded authority surface. In
industrial settings, every recovery path should have a known worst-case scope.
Typed recovery nodes let the program author declare *exactly* what recovery is
allowed for each failure point, and the runtime can enforce that the recovery
stays within the declared surface.

The existing `_build_delegated_recovery_request` already generates
`allowed_actions` from node policy — this proposal makes those actions
correspond to concrete node types rather than abstract labels.

**Impact on plan**: Split OP-6 into two tracks: (1) Phase 3: `retry_node`,
`fallback_override_node`, `recovery_skill_node` — all deterministic, no model
dependency. (2) Phase 6: `query_model_node`, `delegate_to_llm_node` —
model-dependent, requires OP-7 and OP-8.

**Impact on specs**: 460 §6 needs the recovery-node taxonomy added. 420 needs
`recovery_policy` as a new node-instance field alongside
`modification_policy`, declaring which recovery nodes are allowed for that
instance.

**Implementation impact**: Moderate. The three Phase 3 recovery nodes are
simpler than `delegate_to_llm` — they are deterministic py_trees behaviors
that compose existing capabilities. The trigger taxonomy
(`planning_failed`, `validation_failed`, `timeout`, `scene_mismatch`,
`execute_failed`) maps naturally to selector/fallback tree patterns.

**Risks / tradeoffs**: More node types means more catalog surface to maintain.
But each node has a small, well-defined contract — this is preferable to one
large node with a mode parameter that changes its authority characteristics.

### 4.4 OP-7: `query_model` is still only a concept, not a contract

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#op-7-query_model-needs-a-concrete-node-contract-before-model-in-the-loop-execution](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#op-7-query_model-needs-a-concrete-node-contract-before-model-in-the-loop-execution)

**Related specs**:
- [440-node-catalog-format-and-versioning.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/440-node-catalog-format-and-versioning.md)
- [460-skill-catalog-and-intervention.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/460-skill-catalog-and-intervention.md)
- [OPENCLAW_INTEGRATION_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/OPENCLAW_INTEGRATION_PLAN.md)

**What is already decided**:
- `query_model` is not an intervention session
- it is a regular tree node
- it must write structured outputs, not free-form chat text

**What remains unresolved**:
- node schema: inputs, prompt variables, optional image attachment, output
  parsing mode, confidence handling, timeout behavior
- blackboard namespacing rules for repeated perception cycles
- what failure means: retry, tree failure, fallback trigger, or degraded mode

**Why this matters**:
- without a concrete contract, “model-in-the-loop” is not an implementation
  task, it is only an aspiration

**Discussion prompt**:
- should `query_model` be a narrowly typed structured-query node from day one,
  or should it support multiple parsing modes at the cost of more complexity?

**Round 1 proposal**:

Start with a **single structured-query mode** (JSON schema output). Add
additional parsing modes only when a concrete product need justifies the
complexity.

Concrete catalog contract:

```yaml
node_id: query_model
category: perception
parameters:
  model_endpoint:          # string — deployment-configured endpoint alias
  prompt_template:         # string — template with ${bb:key}, ${scene:snapshot} vars
  scene_attachment:        # enum: none | state_json | snapshot_image
  output_schema:           # JSON Schema defining expected response structure
  output_key_prefix:       # string — bb write namespace (e.g., "perception/pick_target")
  min_confidence:          # float 0.0-1.0 — below this, node returns FAILURE
  timeout_ms:              # int — wall-clock timeout; on expiry, returns FAILURE
  retry_on_parse_failure:  # bool, default false
bb_writer_params: [output_key_prefix]
```

Behavior:

- Template expansion happens at tick time against current blackboard and scene
  state.
- Response is validated against `output_schema`. If validation fails and
  `retry_on_parse_failure` is false, the node returns FAILURE. If true, one
  retry with a repair prompt.
- Validated response fields are written to blackboard under
  `{output_key_prefix}/{field_name}`.
- `min_confidence` checks the `confidence` field in the response schema (if
  declared). Below threshold = FAILURE.
- Timeout is wall-clock, not tick-count. On expiry, the node returns FAILURE
  and the tree handles it like any other node failure.
- Repeated perception cycles use distinct `output_key_prefix` values to avoid
  stomping — this is enforced by the blackboard namespacing, not by the node.

**Why**: Multiple parsing modes (free-text, regex extraction, JSON, custom
parser) sound flexible but create a contract that is impossible to validate at
load time. A single JSON-schema mode is:

- Validatable: the loader can check that the output schema is well-formed and
  that downstream nodes reference valid output keys.
- Auditable: the response is structured data, not free text that might or might
  not contain what you expected.
- Compatible with OpenClaw: OpenClaw's structured output support maps naturally
  to JSON schema validation.

If a future use case needs free-text output (e.g., a VLM description), it can
be wrapped in a trivial JSON schema: `{"description": "string"}`.

**Impact on plan**: OP-7 resolves to a concrete schema. Phase 6 implementation
can proceed against this contract.

**Impact on specs**: 440 needs the `query_model` catalog entry added. A new
section in 460 or a dedicated `470-model-node-contracts.md` should define the
prompt-template variable syntax and output-schema validation rules.

**Implementation impact**: Moderate. The node is a py_trees behavior that:
(1) expands the template, (2) calls an HTTP endpoint, (3) validates the JSON
response against the schema, (4) writes to blackboard. The hard parts are
template variable resolution (needs access to blackboard and scene state at
tick time) and timeout handling (needs an async pattern with a RUNNING return
until response or timeout).

**Risks / tradeoffs**: JSON-schema-only means the model must produce valid
JSON. Modern models handle this well with structured output modes, but older
or smaller models may struggle. Mitigation: the `retry_on_parse_failure` flag,
and the recommendation to use models with native structured output support
(which OpenClaw/NemoClaw provide).

### 4.5 OP-8: External transport and authentication are not yet a product decision

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#op-8-external-agent-transport-and-authentication-need-a-product-decision](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#op-8-external-agent-transport-and-authentication-need-a-product-decision)

**Related docs**:
- [LLM_OPERABILITY_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md)
- [OPENCLAW_INTEGRATION_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/OPENCLAW_INTEGRATION_PLAN.md)

**What is already decided**:
- stdio MCP is useful locally
- if external orchestration is targeted, ManyForge needs a real external-agent
  transport

**What remains unresolved**:
- whether the blessed path is HTTP/SSE MCP, another HTTP-native tool surface,
  or a documented REST-only fallback
- how auth/session identity should map to that transport
- how to validate this in a sandboxed runtime rather than only locally

**Why this matters**:
- external orchestration is impossible to productize without this decision

**Discussion prompt**:
- should transport/auth be a hard dependency before `query_model` and
  `delegate_to_llm`, or can internal model-node work proceed first?

**Round 1 proposal**:

**HTTP/SSE MCP** as the blessed external transport. REST-only fallback is
always available but is not the recommended integration path. Internal
model-node work proceeds first — it does not depend on external transport.

Concrete decisions:

- **Transport**: `GET /api/mcp/sse` (SSE event stream) + `POST /api/mcp` (tool
  invocations) using the MCP SSE transport spec. OpenClaw already supports
  SSE/HTTP natively — only ManyForge needs to add the endpoint.
- **Auth**: The existing `SecurityConfig.api_token` bearer-token mechanism
  applies to MCP transport the same way it applies to REST. No separate auth
  model for MCP.
- **Session identity**: Each SSE connection is a session. The MCP transport
  layer maps the bearer token to an operator/agent identity. Session identity
  propagates into intervention `authority_source` and event attribution.
- **Validation**: End-to-end test with OpenClaw connecting from inside a
  NemoClaw sandbox to ManyForge's SSE endpoint over the container network.
- **Dependency ordering**: `query_model` and the deterministic recovery nodes
  (OP-6 Phase 3 track) do NOT depend on external transport. They use
  deployment-configured model endpoints directly. Transport/auth is a
  Phase 5-6 concern.

**Why**: OpenClaw already supports `"url": "http://..."` in its `mcp.servers`
config with SSE/HTTP transport. This is a solved problem on their side. The
only work is on ManyForge's side: exposing the existing MCP tool schema over
an HTTP/SSE endpoint instead of only stdio.

REST-only fallback remains because the REST API is already complete — an agent
that can't use MCP can always call REST endpoints directly. But MCP is the
recommended path because it carries tool schemas, structured errors, and
session semantics that REST-only lacks.

**Impact on plan**: OP-8 resolves. Phase 5 scope confirmed: add SSE/HTTP MCP
endpoint, validate from sandboxed OpenClaw agent.

**Impact on specs**: No spec changes needed — this is a transport/deployment
decision, not a contract change.

**Implementation impact**: Low-moderate. The `mcp` Python package already
supports SSE transport. The work is: (1) add a FastAPI route that bridges SSE
transport to the existing MCP server, (2) wire bearer-token auth, (3) write the
end-to-end test. Tool schemas don't change.

**Risks / tradeoffs**: SSE has known limitations with some corporate proxies
and load balancers. The REST fallback covers that case. If the MCP spec moves
to streamable-HTTP as the recommended transport, ManyForge should follow — but
SSE is the stable choice today.

### 4.6 OP-9: Prompt/response audit policy is still underspecified

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#op-9-promptresponse-audit-policy-must-be-explicit](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#op-9-promptresponse-audit-policy-must-be-explicit)

**Related docs**:
- [LLM_OPERABILITY_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md)
- [OPENCLAW_INTEGRATION_PLAN.md](/home/tndlux/workspaces/dev_ws/src/manyforge/OPENCLAW_INTEGRATION_PLAN.md)

**What is already decided**:
- model-facing flows need auditable provenance
- industrial deployment cannot rely on ad hoc logging choices

**What remains unresolved**:
- full text vs hash/template-only storage
- redaction rules for sensitive process data
- retention and access-control policy
- what debug export should expose by default

**Why this matters**:
- not a motion-safety blocker, but a deployment, privacy, and audit blocker

**Discussion prompt**:
- what is the minimum retained record that still lets an investigator
  reconstruct why the model influenced robot behavior?

**Round 1 proposal**:

**Tiered retention** based on the model-call surface.

| Surface | Default retention | Redaction |
|---|---|---|
| `query_model` (in-tree perception) | Full prompt + response stored as structured event | Redact fields marked `sensitive` in the output schema |
| Delegated recovery (external agent session) | Full MCP tool call/response log with session correlation | Redact process parameters marked sensitive in deployment config |
| Composer assistant (design-time chat) | Hash of prompt + template ID + response summary | Full text opt-in per deployment |
| External supervisory agent | MCP tool call log (same as delegated recovery) | Same redaction rules |

Core rules:

- Every model call generates a durable `model_query_event` with:
  `timestamp_ns`, `run_id` or `session_id`, `model_endpoint`,
  `prompt_template_id` (not the expanded text by default), `response_hash`,
  `response_structured_fields` (the parsed output), `latency_ms`,
  `token_counts` (if available), `outcome` (success/failure/timeout/
  parse_error).
- Full prompt text is stored by default for `query_model` because it is part of
  the execution trace and needed for debugging perception failures. For other
  surfaces, full text is opt-in.
- `GET /api/debug/export` includes model events with the same redaction rules
  as the event recorder. A deployment can configure
  `audit_policy.model_calls.full_text = true` to include expanded prompts in
  exports.
- Retention period is a deployment configuration, not a product default. The
  product provides the storage and redaction mechanism; the deployment decides
  how long to keep it.
- Access control: model-call events follow the same auth rules as all other API
  data. No special access tier for model data in the first implementation.

**Minimum record for investigator reconstruction**: prompt template ID +
template variables (blackboard keys, not values, unless `full_text` is
enabled) + response structured fields + outcome + correlation IDs. This lets
an investigator see: "at this point in the run, this model was queried with
this template against these blackboard keys, and it returned this structured
result, which caused this tree decision."

**Why**: Full-text-by-default for all surfaces is a privacy and storage
liability. Template-reference-by-default is auditable enough for most
investigations — you can reconstruct the prompt from the template + the
blackboard state at that timestamp (which is already in the event stream).
Full text is available when needed via deployment opt-in.

**Impact on plan**: OP-9 resolves. SEC-R3 gets a concrete remediation path.

**Impact on specs**: Needs a new section in 460 or in
`500-observability-tracing-and-performance.md` defining the
`model_query_event` schema and the redaction/retention deployment
configuration.

**Implementation impact**: Moderate. The `EventRecorder` already handles
structured events. The new work is: (1) define the `model_query_event` schema,
(2) add redaction logic driven by deployment config, (3) include model events
in debug export with redaction applied.

**Risks / tradeoffs**: Template-reference-by-default means an investigator
cannot reconstruct the exact prompt if the template was changed after the event
was recorded. Mitigation: template versioning — if `prompt_template_id`
includes a version or hash, the reconstruction is exact regardless of later
template changes.

---

## 5. Remaining Roadmap Items Outside The Open-Point List

These are not just “decisions”; they are substantive implementation streams
still left on the plan.

### 5.1 Phase 4: Kernel event stream + ROS2 sidecar

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#phase-4-kernel-event-stream--ros2-observability-sidecar-p3](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#phase-4-kernel-event-stream--ros2-observability-sidecar-p3)

**Core questions**:
- how much event attribution can stay wrapper-side vs must move into the
  native kernel event stream?
- what is the precise boundary of the ROS2 sidecar?
- how strict should the ros2_medkit one-to-one mapping be?

**Round 1 positions**:

- **Event attribution**: For Phase 3, wrapper-side attribution (EventRecorder)
  is correct and sufficient. The current implementation already projects
  intervention events, delegated-recovery events, and kernel state transitions
  into a unified stream. Moving attribution into the native kernel event stream
  should happen only when a concrete lossiness or ambiguity problem is
  demonstrated — not speculatively. The criterion: if the EventRecorder cannot
  correctly attribute an event because the kernel doesn't propagate enough
  context (e.g., caller/session ID on a kernel-internal collision event), that
  specific attribution gap justifies a kernel extension.

- **ROS2 sidecar boundary**: The sidecar publishes ManyForge events and state
  onto ROS2 topics for consumption by ros2_medkit, rosbag2, and standard ROS2
  tooling. It does NOT route commands — all control still flows through
  ManyForge REST/MCP. The sidecar is a read-only observer of ManyForge state,
  not a control surface.

- **ros2_medkit mapping strictness**: One-to-one where the semantic fits
  (execution state, diagnostics, error reporting). Explicit ManyForge
  extensions where the semantic doesn't fit (behavior-tree structure,
  blackboard state, intervention sessions, skill catalog, runtime overrides).
  The mapping should be documented in `ROS2_MEDKIT_MAPPING.md` per success
  criterion 18, with clear "extension" labels for ManyForge-specific
  capabilities.

### 5.2 Contact-rich execution stream

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#parallel-design-stream-contact-rich-execution-contracts](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#parallel-design-stream-contact-rich-execution-contracts)

**Likely future spec anchor**:
- `250-contact-policy` (planned/new)

**Core questions**:
- whether Tier 1 geometry-only contact reasoning is viable enough after the
  required benchmark spike
- how contact policy should layer across deployment, program, and runtime
- how contact-tolerant motion should be exposed without weakening safety

**Round 1 positions**:

- **Tier 1 viability**: Cannot be answered without the Q3 design spike. The
  spike is correctly defined in the plan — benchmark penetration-depth
  computation across the representative scenarios, produce a go/no-go
  recommendation. Until the spike delivers results, do not assume Tier 1
  viability. If the spike is negative, fall back to binary collision (the
  current model) with richer per-zone policy as the only improvement.

- **Contact policy layering**: Deployment baseline → program override →
  runtime override, same pattern as other policies. Runtime override allowed in
  `IDLE` and `DIRECT_CONTROL` only. This is already described in the plan and
  is consistent with how `modification_policy` works for other node-level
  properties.

- **Exposing contact-tolerant motion**: Through the node catalog and skill
  catalog, not through a mode switch. A program author selects a contact-aware
  move node (or skill) and declares its contact policy. The runtime enforces
  that policy. There is no global "contact mode" — contact tolerance is
  per-move, per-policy, per-zone.

### 5.3 Phase 6 optional integration work

**Plan reference**:
- [LLM_OPERABILITY_PLAN.md#phase-6-model-in-the-loop--external-orchestration-p4-optional](/home/tndlux/workspaces/dev_ws/src/manyforge/LLM_OPERABILITY_PLAN.md#phase-6-model-in-the-loop--external-orchestration-p4-optional)

**Core questions**:
- how much should ManyForge absorb directly vs leave in OpenClaw integration
- where the boundary sits between product feature and integration feature

**Round 1 positions**:

- **What ManyForge absorbs**: `query_model` node, deterministic recovery nodes,
  runtime-override system, intervention sessions, MCP tool surface, SSE/HTTP
  transport endpoint. These are product features regardless of whether OpenClaw
  is present.

- **What stays in OpenClaw integration**: Agent runtime, model routing, sandbox
  isolation, operator messaging/escalation, workspace knowledge
  (AGENTS.md/SOUL.md), AgentSkills authoring/publishing. These are OpenClaw
  responsibilities consumed through the MCP/REST surface.

- **Boundary rule**: If the capability is needed for ManyForge to be
  independently operable (Principle 12), it is a product feature. If it only
  matters when an external orchestration stack is present, it is an integration
  feature. The MCP SSE/HTTP endpoint is a product feature (any external agent
  needs it); the OpenClaw-specific AGENTS.md prompt engineering is an
  integration feature.

---

## 6. Discussion Guidance For The Next Round

When proposing changes, prefer this format:

1. **Open point / phase item**
2. **Proposal**
3. **Why**
4. **Reference documents**
5. **Impact on plan**
6. **Impact on specs**
7. **Implementation impact**
8. **Risks / tradeoffs**

Keep proposals concrete. If a proposal changes a contract, cite the exact plan
section and spec file that should change.

---

## 7. Suggested Order Of Discussion

To avoid mixing unrelated concerns, discuss in this order:

1. OP-4 authority model
2. OP-6 delegated recovery product shape
3. OP-5 runtime-override end-state semantics
4. OP-7 `query_model` contract
5. OP-8 transport/auth decision
6. OP-9 audit policy
7. Phase 4 ROS2 sidecar boundary
8. contact-rich execution stream

This order keeps authority and runtime semantics ahead of transport and
deployment details.

---

## 8. Round 1 Proposal Summary

| OP | Proposed resolution | Phase |
|---|---|---|
| OP-4 | Scoped authority grants (`human_approved` / `policy_granted` / `deployment_granted`) | Phase 3 core; `deployment_granted` deferred |
| OP-5 | Parameter-patch only; structural adaptation deferred to future ADR | Phase 3 (HMI work remains) |
| OP-6 | Typed recovery nodes; stop-and-request remains external fallback | Phase 3 (deterministic) + Phase 6 (model nodes) |
| OP-7 | Single JSON-schema structured-query mode | Phase 6 |
| OP-8 | HTTP/SSE MCP; internal model nodes independent of transport | Phase 5 |
| OP-9 | Tiered retention; template-reference-by-default, full-text opt-in | Phase 6 (with SEC-R3) |

**Status**: Reviewed in §9, converged in §10-11, applied to plan. `skip`
outcome was subsequently rejected outright by product owner.

---

## 9. Round 1 Review — Industrial Reliability / Safety Pass

This review responds to the Round 1 proposals above from the perspective of an
industrial deployment that values reliability, repeatability, efficiency, and
safety over flexibility.

### 9.1 OP-4 review: scoped grants are the right shape, but `deployment_granted`
### should not open `IDLE -> DIRECT_CONTROL` in the baseline product

**Position**: mostly agree, with one tightening.

Accept:
- approval should be a scoped grant, not a mode bit
- `human_approved` and `policy_granted` are good concrete grant types
- every direct-control session should carry the grant provenance

Tighten:
- do **not** allow `deployment_granted` to authorize `IDLE -> DIRECT_CONTROL`
  in the baseline product
- keep `IDLE -> DIRECT_CONTROL` human-gated unless/until the product defines a
  distinct armed/autonomous maintenance mode with separate safety analysis

Reason:
- a deployment-scoped grant is too coarse to justify direct entry from `IDLE`
  in an industrial cell
- if the robot is otherwise idle and healthy, an unsolicited autonomous switch
  into direct control is exactly the kind of behavior that operators will find
  difficult to reason about and certify
- delegated recovery already covers the safer non-human path:
  `RUNNING -> STOPPED -> DIRECT_CONTROL` with explicit pending request and
  policy grant

Additional requirement:
- audit identity must not collapse to a shared bearer token
- the session record should distinguish:
  - `principal_id` or equivalent actor identity
  - transport/session identity
  - grant type

Conclusion:
- scoped grants: **yes**
- `deployment_granted` as a reserved future concept: **yes**
- autonomous `IDLE -> DIRECT_CONTROL` via deployment grant: **not in the
  baseline industrial product**

### 9.2 OP-5 review: parameter-patch-only is the correct baseline, but bind
### overrides to the exact base program revision

**Position**: strongly agree, with one additional requirement.

Accept:
- runtime overrides should remain parameter-patch-only
- structural edits should stay out of scope unless elevated into a separate
  ADR/spec effort
- user must explicitly choose between:
  - discard overrides
  - materialize overrides
  before editing the base program

Add:
- every runtime override ledger should be bound to a concrete base program
  revision hash/fingerprint
- if the loaded program changes, stale overrides must either:
  - be invalidated, or
  - be revalidated against the new base revision before use

Reason:
- repeatability depends on being able to say exactly which base artifact an
  override was applied against
- without base-revision binding, the same override key/value pair may mean
  something different after unrelated program edits

Conclusion:
- parameter-patch-only: **yes**
- structural adaptation through overrides: **no**
- base program revision binding: **required**

### 9.3 OP-6 review: typed recovery nodes are safer than a generic
### `delegate_to_llm`, but `skip` and `program_only` need stricter rules

**Position**: agree with the direction, with two important constraints.

Accept:
- typed recovery nodes are safer than a generic high-authority recovery node
- deterministic recovery should come first
- external stop-and-request remains the correct fallback when deterministic
  recovery does not cover the situation

Tighten:
- `skip` must not be a recovery outcome at all (subsequently rejected outright
  — see §10.3)
- `program_only` should remain deterministic-only unless and until a fully
  specified internal executor exists
- `open_draft_patch` should be terminal
  - it may stop the run and emit a patch proposal
  - it should not auto-resume execution

Reason:
- any `skip` outcome can silently violate process guarantees
- `program_only` with implicit model behavior would erase the authority
  boundary between deterministic execution and external intelligent recovery
- draft generation is an authoring outcome, not an execution-success outcome

Recommended trigger taxonomy ownership:
- define a closed runtime enum for delegated-recovery trigger reasons rather
  than relying on ad hoc string mapping in multiple layers

Conclusion:
- typed recovery nodes: **yes**
- generic `delegate_to_llm` as the default recovery shape: **no**
- `skip` rejected; `program_only` needs stricter gating than proposed

### 9.4 OP-7 review: JSON-schema-only is the right baseline, but prompt and
### output contracts should be versioned and atomic

**Position**: strongly agree, with stronger repeatability constraints.

Accept:
- start with a single structured-query mode
- response must validate against JSON schema
- confidence/timeout/failure handling must be explicit

Tighten:
- use deployment-configured endpoint aliases only, not arbitrary URLs in the
  node
- prefer `prompt_template_id` + version/hash over unconstrained inline prompt
  text as the stable contract
- blackboard writes should be atomic at the namespace level:
  - either the validated result replaces the target namespace fully
  - or the node fails without partial writes

Reason:
- versioned prompt templates improve repeatability and auditability
- arbitrary endpoint URLs in the tree weaken deployment portability and trust
  control
- partial writes after parse/validation failure create ambiguous runtime state

Conclusion:
- JSON-schema-only: **yes**
- versioned prompt-template contract: **required**
- atomic namespace write semantics: **required**

### 9.5 OP-8 review: HTTP/SSE MCP is the right transport target, but shared
### bearer-token identity is not good enough for industrial audit

**Position**: mostly agree, with a stronger requirement on identity.

Accept:
- HTTP/SSE MCP should be the blessed external-agent transport
- REST remains a fallback, not the preferred external integration path
- internal model-node work does not need to wait on transport work

Tighten:
- the current single API-token model is acceptable as a development bootstrap,
  not as the long-term industrial answer
- the product needs per-principal identity, or a trusted auth proxy that
  injects per-principal identity, so audit trails can distinguish operators and
  agent runtimes

Reason:
- a shared token cannot support meaningful attribution, revocation, or least
  privilege
- transport and auth are not separate concerns in industrial deployment; the
  session identity must survive all the way into event provenance

Conclusion:
- HTTP/SSE MCP: **yes**
- REST-only as the recommended path: **no**
- shared-token identity as the final model: **no**

### 9.6 OP-9 review: audit must be structured and correlated, but full prompt
### text should not be the default even for `query_model`

**Position**: partially agree, with stricter default data minimization.

Accept:
- model-facing activity needs its own structured event family
- template versioning is mandatory
- retention is deployment-configured

Change:
- do **not** store full prompt text by default for `query_model`
- default retained record should be:
  - `prompt_template_id`
  - template version/hash
  - redacted resolved arguments or argument hashes
  - model endpoint alias and model version
  - structured response fields
  - response hash
  - outcome / latency / token counts
- full expanded prompt text should be an explicit deployment opt-in

Reason:
- industrial cells often treat process parameters, geometry, product IDs, and
  workstation context as sensitive operational data
- storing full prompt text by default broadens the leak surface without being
  strictly required for most incident reconstruction

Additional distinction:
- tool-call logs for delegated recovery are more important than free-form agent
  conversation logs
- by default, store tool calls, arguments/results with redaction, and session
  correlation
- do not store hidden reasoning/scratchpad by default

Conclusion:
- structured model event family: **yes**
- full prompt text by default: **no**
- template/version/hash plus redacted arguments by default: **yes**

### 9.7 Phase 4 review: wrapper-side enrichment is fine, but native execution
### facts should remain kernel-authored

**Position**: partial agreement with the Round 1 sidecar/event-attribution
proposal.

Agree:
- the ROS2 sidecar should remain read-only and non-authoritative
- ros2_medkit mapping should be one-to-one where semantics fit, explicit
  extension where they do not

Tighten:
- wrapper-side attribution should enrich events, not become the source of truth
  for native execution facts
- motion execution facts that affect safety or sequencing should remain
  kernel-authored:
  - state transitions
  - stop reason
  - collision/contact events
  - execution start/finish/failure timing

Reason:
- once event timing or ordering depends on wrapper reconstruction rather than
  the native source, repeatability and post-mortem trust weaken

Conclusion:
- wrapper attribution as enrichment: **yes**
- wrapper attribution as the long-term source of truth for native execution:
  **no**

### 9.8 Contact-rich execution review: keep the bar high and forbid “contact
### mode” shortcuts

**Position**: agree with the conservative direction.

Accept:
- no assumption of Tier 1 viability before the required benchmark spike
- no global “contact mode”; contact tolerance should remain per-move, per-zone,
  per-policy
- layering should match deployment baseline -> program override -> runtime
  override

Tighten:
- runtime contact-policy mutation should stay forbidden while `RUNNING`
- geometry-only contact support should not be marketed as production-ready
  until the spike and acceptance criteria are passed

Reason:
- contact tolerance is exactly the area where optimistic assumptions become
  hardware risk

Conclusion:
- the plan’s current conservative shape is correct

### 9.9 Recommended convergence direction

If the next discussion round wants to narrow the decision space quickly, the
best industrially conservative convergence set is:

1. OP-4: accept scoped grants, but keep `IDLE -> DIRECT_CONTROL` human-gated in
   the baseline product
2. OP-5: lock runtime overrides to parameter patches only, and bind them to a
   base program revision hash
3. OP-6: prefer typed deterministic recovery nodes; keep generic
   `delegate_to_llm` out of the baseline runtime path
4. OP-7: adopt JSON-schema-only `query_model` with versioned prompt templates
   and atomic namespace writes
5. OP-8: bless HTTP/SSE MCP, but require per-principal identity beyond a shared
   API token
6. OP-9: use structured model events with template/version/hash by default, and
   make full prompt text opt-in

---

## 10. Round 2 — Convergence Response

This section responds to the Round 1 review (§9) and aims to converge each OP.
Where the review tightens a proposal in a way that strengthens industrial
safety/reliability, that tightening is accepted. Where a point needs further
refinement, the refinement is proposed here.

### 10.1 OP-4: Accept all tightenings — CONVERGED

The review's three adjustments are all correct:

1. **`deployment_granted` must not open `IDLE -> DIRECT_CONTROL` in the
   baseline product.** Accepted without reservation. An idle, healthy robot
   entering autonomous direct control via a deployment-wide policy is exactly
   the kind of implicit authority escalation that undermines operator trust and
   certification. `deployment_granted` remains a reserved concept, but its
   scope in any future implementation is limited to `STOPPED ->
   DIRECT_CONTROL` (delegated recovery servicing), never `IDLE ->
   DIRECT_CONTROL`.

2. **Audit identity must not collapse to a shared bearer token.** Accepted.
   The session record must carry a `principal_id` (or equivalent actor
   identity) distinct from the transport session. This means:
   - `principal_id`: identifies the human operator or named agent runtime
   - `transport_session_id`: identifies the SSE/HTTP connection
   - `grant_type`: one of `human_approved` | `policy_granted`

   For Phase 3 (local/container development), the bearer token can serve as a
   placeholder `principal_id`. But the schema must carry the field from day
   one so that industrial deployments can populate it via auth proxy or
   per-principal token issuance without a schema migration.

3. **`IDLE -> DIRECT_CONTROL` remains human-gated unless the product defines a
   distinct armed/autonomous maintenance mode.** Accepted. This is the correct
   default for industrial cells. If a future product version needs autonomous
   IDLE-entry (e.g., scheduled maintenance routines), it must come with its
   own safety analysis and a distinct system mode, not a deployment flag.

**Converged position**: Scoped grants with `human_approved` and
`policy_granted` as the active grant types. `deployment_granted` reserved but
explicitly excluded from `IDLE -> DIRECT_CONTROL`. `principal_id` required in
session record from day one.

### 10.2 OP-5: Accept base-revision binding — CONVERGED

The review's addition is exactly right:

**Runtime override ledger must be bound to a base program revision
hash/fingerprint.** Accepted. Concretely:

- `RuntimeOverrideRecord` gains a `base_program_hash` field computed from the
  program artifact content at the time the override was applied.
- When a program is reloaded or edited, any existing overrides whose
  `base_program_hash` does not match the new program are flagged as **stale**.
- Stale overrides cannot be activated. They must be either:
  - **revalidated**: the system re-runs override validation against the new
    program and, if valid, updates the hash, or
  - **discarded**: the operator or agent explicitly removes them.
- The materialized-tree preview must refuse to render if any selected overrides
  are stale.

This is a small implementation change (hash computation + staleness check in
the override ledger) with a large reliability payoff: it makes it impossible
to accidentally run a program with overrides that were authored against a
different version.

**Converged position**: Parameter-patch only. Override ledger bound to base
program revision hash. Stale overrides invalidated on program change.

### 10.3 OP-6: Accept all tightenings — CONVERGED

All three constraints are correct and strengthen the proposal:

1. **`skip` is rejected as a recovery outcome.** The review flagged it as
   dangerous; the product owner subsequently rejected it outright. A failed
   node either gets retried, overridden, patched, or the run stops. There is
   no `allow_skip` flag, no per-node opt-in, no gated version. Any future
   reconsideration requires an explicit ADR with safety analysis.

2. **`program_only` stays deterministic-only.** Accepted. This was the intent
   of the Round 1 proposal but not stated firmly enough. To be explicit:
   `program_only` delegated recovery uses only the deterministic in-tree
   recovery nodes (`retry_node`, `fallback_override_node`,
   `recovery_skill_node`). It does not invoke model calls. If model-assisted
   recovery is needed, the policy must be `llm_allowed` or `human_only`, not
   `program_only`.

3. **`open_draft_patch` is terminal.** Accepted. After generating a patch
   proposal, the system transitions to `STOPPED`. The operator reviews the
   patch via HMI or API. Resuming execution requires an explicit
   `Reset` + `Run` cycle, not an auto-resume.

4. **Closed enum for trigger reasons.** Accepted. The trigger taxonomy becomes
   a runtime-enforced enum, not ad hoc strings:

   ```python
   class RecoveryTrigger(str, Enum):
       PLANNING_FAILED = "planning_failed"
       VALIDATION_FAILED = "validation_failed"
       EXECUTE_FAILED = "execute_failed"
       EXECUTE_TIMEOUT = "execute_timeout"
       SCENE_MISMATCH = "scene_mismatch"
       PRECONDITION_FAILED = "precondition_failed"
       POSTCONDITION_FAILED = "postcondition_failed"
   ```

   The enum is extensible via spec amendment, not by arbitrary caller strings.

**Converged position**: Typed recovery nodes. `skip` rejected outright (no
`allow_skip`, no gated version). `program_only` is deterministic-only.
`open_draft_patch` is terminal. Trigger reasons are a closed enum.

### 10.4 OP-7: Accept all tightenings — CONVERGED

All three are improvements:

1. **Deployment-configured endpoint aliases only.** Accepted. The node
   parameter is `model_endpoint: string` which is resolved against a
   deployment-level endpoint registry (e.g., `deployment.model_endpoints`
   map). The tree never contains a raw URL. This ensures:
   - deployment portability (same program, different model infra),
   - trust control (only allowlisted endpoints can be called), and
   - auditability (the endpoint alias is stable across environments).

2. **`prompt_template_id` + version/hash instead of inline prompt text.**
   Accepted. The node parameter changes from `prompt_template` (inline text)
   to:

   ```yaml
   prompt_template_id: "pick_target_perception_v2"
   ```

   The template content is resolved from a deployment-level template registry
   at tick time. The registry entry includes a content hash. The template
   registry may be file-based initially (templates stored alongside
   deployment artifacts) and is not a separate service.

   This means prompt changes are deployment-level configuration changes, not
   program edits — which is the correct separation of concerns. A program
   says *what* to ask; the deployment says *how* to ask it.

3. **Atomic namespace writes.** Accepted. The `query_model` node either writes
   the full validated response to `{output_key_prefix}/...` or fails without
   writing anything. No partial namespace population. Implementation: build
   the full write set, validate all keys, then apply as a batch.

**Converged position**: JSON-schema-only. Deployment-configured endpoint
aliases. Versioned prompt template references (not inline text). Atomic
blackboard namespace writes.

### 10.5 OP-8: Accept per-principal identity requirement — CONVERGED

The review is correct that a shared bearer token is insufficient for industrial
audit. Accept with a phased approach:

- **Phase 3 (development)**: Single bearer token as `principal_id` placeholder.
  Schema carries `principal_id` field from day one.
- **Phase 5 (MCP transport)**: The SSE/HTTP endpoint accepts per-principal
  bearer tokens. The product provides a token-to-principal mapping config. A
  deployment may alternatively front ManyForge with an auth proxy that injects
  `X-ManyForge-Principal` headers.
- **Industrial deployment**: Per-principal identity is a deployment
  requirement, not optional. The product validates that `principal_id` is
  non-empty and non-default before allowing intervention sessions in
  production mode.

The key insight from the review: transport and auth are not separable in
industrial deployment. Session identity must flow from the transport layer into
event provenance without gaps. This means the MCP transport implementation
must propagate the authenticated principal into every tool invocation context,
not just validate the token at connection time.

**Converged position**: HTTP/SSE MCP. Per-principal identity required for
industrial deployment. Schema carries `principal_id` from Phase 3. Single
shared token acceptable only for development.

### 10.6 OP-9: Accept data minimization default — CONVERGED

The review's position is stronger than my Round 1 proposal and I accept the
change:

**Full prompt text should NOT be the default even for `query_model`.** The
reviewer correctly identifies that in industrial cells, prompt context often
contains sensitive operational data (process parameters, geometry, product IDs,
workstation identifiers). Storing full expanded prompts by default widens the
data leak surface unnecessarily.

Revised default retained record for all model-facing surfaces:

- `prompt_template_id` + template content hash
- resolved argument keys (blackboard key names, not values) + argument value
  hashes
- `model_endpoint` alias + model version (if available from response headers)
- `response_structured_fields` (the parsed, validated output — redacted per
  deployment config)
- `response_hash` (of the raw response, for integrity verification)
- `outcome` (success | failure | timeout | parse_error | confidence_below_threshold)
- `latency_ms`, `token_counts` (if available)
- `session_id` or `run_id` correlation

Full expanded prompt text: **deployment opt-in only**, via
`audit_policy.model_calls.full_text = true`.

Additional distinction accepted from the review:
- **Tool-call logs** (structured, correlated with session): stored by default
  for delegated recovery and supervisory agent sessions.
- **Agent internal reasoning/scratchpad**: NOT stored by default. Opt-in only.

The reconstruction path for investigators without full text:
1. Look up the `prompt_template_id` and its content hash in the template
   registry.
2. Look up the argument key names in the event.
3. Look up the blackboard state at that `timestamp_ns` from the event stream.
4. Re-expand the template to reconstruct the prompt.

This is more work than reading a stored prompt, but it keeps the default data
footprint minimal. If a deployment needs faster reconstruction, they opt in to
full text storage.

**Converged position**: Template ID + version/hash + redacted argument keys by
default. Full prompt text opt-in only. Tool-call logs stored by default for
delegated recovery. Agent scratchpad not stored by default.

### 10.7 Phase 4: Accept kernel-authored execution facts — no dispute

The review's position is correct: the wrapper (EventRecorder) should enrich
events with session/intervention context, but native execution facts (state
transitions, stop reasons, collision events, timing) must remain
kernel-authored.

The distinction is clear:
- **Kernel-authored** (source of truth): execution state machine transitions,
  stop/pause/resume events and reasons, collision/contact detections,
  trajectory start/finish timing, execution error details.
- **Wrapper-enriched** (context overlay): `session_id`, `session_kind`,
  `authority_source`, `run_id`, intervention correlation, blackboard
  promotion events, override events.

If a future need arises where the wrapper must reconstruct or infer a
kernel-level fact (e.g., inferring stop reason from observation timing),
that is the signal to extend the kernel event contract — not to let the
wrapper become an alternative source of truth.

**Converged position**: Wrapper enriches, kernel authors execution facts. No
dispute.

### 10.8 Contact-rich execution: Accept all constraints — no dispute

The review's two additional constraints are both correct:

1. **Runtime contact-policy mutation forbidden while `RUNNING`.** This is
   already the intent — contact policy runtime overrides follow the same
   `IDLE` and `DIRECT_CONTROL` mutation rules as all other runtime overrides.
   But the review is right to make it explicit: mutating contact policy during
   active motion is a safety violation, not just a policy violation.

2. **Geometry-only contact should not be marketed as production-ready before
   the spike passes.** Agreed. Until the Q3 benchmark spike delivers a
   positive go/no-go, Tier 1 geometry-only contact is a design target, not a
   product capability.

**Converged position**: No dispute. The plan's conservative shape is correct.

---

## 11. Round 2 Convergence Summary

All six OPs and all roadmap items have reached convergence after two rounds.

| OP | Converged position | Phase | Key tightening from review |
|---|---|---|---|
| OP-4 | Scoped grants; `IDLE -> DIRECT_CONTROL` stays human-gated; `principal_id` in session record | Phase 3 | No autonomous IDLE entry; per-principal audit identity |
| OP-5 | Parameter-patch only; override ledger bound to base program revision hash | Phase 3 (HMI remains) | Base-revision binding required |
| OP-6 | Typed recovery nodes; `skip` rejected; `program_only` deterministic-only; closed trigger enum | Phase 3 (det.) + Phase 6 (model) | `skip` rejected outright; `program_only` deterministic-only; `open_draft_patch` terminal |
| OP-7 | JSON-schema-only; deployment endpoint aliases; versioned prompt template refs; atomic bb writes | Phase 6 | No inline prompts; no arbitrary URLs; atomic writes |
| OP-8 | HTTP/SSE MCP; per-principal identity required for industrial; schema carries `principal_id` from Phase 3 | Phase 5 | Shared token insufficient for industrial audit |
| OP-9 | Template ID + hash + redacted args by default; full text opt-in; tool-call logs stored; scratchpad not stored | Phase 6 (with SEC-R3) | No full prompt text by default, even for `query_model` |

**Roadmap items**:

| Item | Converged position |
|---|---|
| Phase 4 sidecar | Wrapper enriches; kernel authors execution facts |
| Contact-rich | Conservative; no Tier 1 assumption before spike; no runtime mutation while RUNNING |
| Phase 6 boundary | ManyForge absorbs product features; OpenClaw owns agent runtime/sandbox/messaging |

**Next step**: Apply converged positions to `LLM_OPERABILITY_PLAN.md` and the
relevant spec documents (460, 420, 440, 450). Mark the OPs as resolved in the
plan with references back to this discussion.
