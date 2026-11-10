# Assistant Provider Contract v1

This is the current strict wire between Composer and the in-tree assistant
bridge. The model endpoint side is described in
[`ASSISTANT_PROVIDER_CONTRACT.md`](ASSISTANT_PROVIDER_CONTRACT.md). Normative authority and mode semantics live in ManyForge specs 480 and
485; the closed runtime models in `manyforge_assistant_bridge/contracts.py` are
the executable field authority.

## Ownership boundary

Composer originates attributed operator intent and appends its current
revision-fenced in-memory Draft context. Headless publishes the exact active
retained assistant mode and instrument descriptors. The bridge independently
re-reads and verifies that projection, applies the same narrowing, mediates the
model loop, and owns recent request status used for outcome reconciliation.

The bridge can call owner-neutral headless reads and Composer Draft callbacks.
It cannot save a Program, Apply a deployment, mutate runtime state, admit an
operation, or command a robot. Provider responses always carry
`mutated: false` and `requiresReview: true`; `draftMutated: true` means only the
Composer in-memory Draft changed.

## Routes

The bridge runs as the process of the Deployment's `assistant` component and
serves on a loopback socket its host binds on an ephemeral port. Composer
resolves the bridge URL for every call from that component's `assistant_request`
endpoint in `GET /api/components`, and refuses before calling it when the
Deployment has no assistant component (`assistant_component_absent`) or it is
not enabled (`assistant_component_not_enabled`); a submit is also refused while
a Program run exists (`assistant_program_active`) or the assistant's AI model
is not enabled (`assistant_ai_model_not_enabled`).

Bridge service:

| Method | Route | Meaning |
|---|---|---|
| `GET` | `/health` | `manyforge.assistant.bridge_health.v2` `{status: "ready", activity: "busy" \| "idle"}`; `busy` while any request runs. Liveness only, not model or runtime readiness |
| `POST` | `/v1/manyforge/assistant` | Validate and accept one idempotent request (`202`) |
| `GET` | `/v1/manyforge/assistant/{requestId}` | Read the bridge-owned current/terminal status |
| `POST` | `/v1/manyforge/assistant/{requestId}/cancel` | Cancel an in-flight request by ID |

Composer exposes the corresponding operator routes under
`/api/assistant/requests` and proxies owner-issued mode discovery at
`GET /api/assistant/modes`. `/api/assistant/bridge/tools/{toolId}` is the
bridge-to-Composer Draft callback boundary, not a general runtime tool API.

## Request

`schemaVersion` is exactly `manyforge.assistant.provider_request.v1`. The
closed request contains:

- attribution: `requestId`, `requestOwnerEpoch`, `sourceLabel`, nullable
  `correlationId`, and `conversationId`;
- operator intent: `assistantMode`, `message`, sorted unique
  `requestedTools`, and narrowing-only `fieldPolicy`;
- forwarded proof input: the complete `modeProjection`
  (`manyforge.assistant_mode_projection.v2`, which names the
  `assistant_component_id` and the cell states the mode `runs_in`) and exact
  `resolvedCatalog` (`catalogHash`, `grantPolicy`, `nodes`, `skills`, `tools`);
- Composer context: `composerDraft` with Draft identity/revision, the
  composite `expectedWorkspaceEpoch`/`expectedWorkspaceRevision` of the shared
  authoring workspace, the current Kernel `expectedAdmissionGeneration`,
  optional Program snapshot, and the node-environment/source-model projection
  digests. With a loaded Program, Composer runs one reference validation per
  request and adds its result: `selectorFacts` (the qualified candidate's
  `components`, `targets`, `signals`, `frames`, `bodies` and `rules`, or null)
  and `referenceDiagnostics` (its typed reference diagnostics). A refused
  validation is one `referenceDiagnostics` row carrying the refusal code, for
  example `activation_preparer_unavailable`, and null `selectorFacts`. Both
  fields are optional.

Composer captures that backend admission generation and that composite
workspace identity when it accepts the assistant request. Every callback
rechecks the workspace identity and the current admission through the normal
authoring owner; the admission generation is carried as claim evidence and is
not compared. A mutating callback
commits under the same composite fence an ordinary edit uses. Browser snapshot
fields are deliberately absent: the GUI is a disposable presentation client,
so its disconnect or snapshot expiry cannot become assistant authority or
interrupt accepted backend work.
Each Composer callback returns its exact settled `nextAdmissionGeneration` and
its exact `nextWorkspaceRevision`. The bridge chains both backend-issued values
for a later callback in the same assistant request; read-only or rejected
callbacks leave the generation unchanged and never decrease the workspace
revision. The result of a committed edit also carries `referenceDiagnostics`:
the reference validation of the installed Draft (the same validator as the
GUI), with a refused validation reported as one typed row. A committed edit advances the workspace revision, a validated no-op
callback does not, so the bridge requires the exact next revision rather than
accepting a gap or a backwards value.

Forwarded projection/catalog content is not trusted by itself. Before every
model turn, the bridge independently reads the active mode and node environment
from headless, verifies their identities/freshness, reapplies request and field
narrowing, and requires byte-semantic equality with the forwarded contract.

An omitted or empty `requestedTools` means no further narrowing. Current
`fieldPolicy` either is `{}` or carries the sorted non-empty
`allowedOrientationFields` subset defined by the Program pose contract. It can
remove fields, never add them.

## Status and response

Every submit/status/cancel result is
`manyforge.assistant.provider_status.v1` and echoes the complete request
attribution plus one state: `running`, `completed`, `failed`, or `cancelled`.
Only `completed` carries a response; only `failed` carries a closed
`{code,message}` error.

A completed response is exactly
`manyforge.assistant.provider_response.v1`. It echoes attribution and
`assistantMode`, and records the narrowed `catalogHash`, the model's `message`
text, completed `toolCalls`, `warnings`, `requiresReview`, `mutated`,
`draftMutated`, `needsClarification`, `exitReason`, and the bounded
`turnCount`.

The same `requestId` and identical content is idempotent while its bounded
record is retained. Reusing an ID with different content is a conflict. Clients
must reconcile an uncertain submit through the status route before offering an
explicit new-ID retry; there is no silent retry.

## Model and callback rules

- Only canonical underscore tool IDs and exact owner-issued schemas reach the
  model.
- The current built-in bridge uses `tool_choice: "auto"`; it does not guess a
  tool from prompt keywords. Forced selection requires an explicitly narrowed
  single tool and separately qualified endpoint/model/parser support.
- An out-of-catalog model call is never run; it gets a typed tool error
  naming the closest listed tool.
- Known callback arguments are validated against the exact schema hash before
  execution; tree-tool nodes are also validated against their own catalog
  schema from the projection's `nodes` rows, and a failure names the field
  path and its expected shape. Composer independently re-verifies the active projection,
  narrowing, schema hash, Draft ID, and expected Draft revision.
- Each Composer callback uses a deterministic idempotency identity derived
  from the provider request owner/identity, model turn, upstream call identity,
  and call position. Composer replays an exact durable outcome before
  consulting mutable Draft/admission context; changed intent under the same
  identity conflicts.
- Draft, node, skill, and tool-result content is untrusted model data, never an
  instruction source. Only the bridge-owned system rules and exact retained
  instrument contract grant model actions.
- With the mode's `stuck_loop_recovery` on (the default), the second failure
  of the same tool with the same error class adds a typed `recovery` object to
  the tool result, and the fifth ends the request as a clarification. With it
  off, failures carry no hint and the request runs to its turn limit.
- Conversation history is memory-, count-, TTL-, mode-, and catalog-bounded;
  it is never runtime or Draft state, and it ends with the bridge process
  (Disable cancels running requests, audited `cancelled`). With the mode's experimental
  `compact_history` on (default off), a completed request is kept only as its
  user message and the final answer with a generated `Draft changes:` list.
- With the mode's experimental `constrained_regeneration` on (default off), a
  call that fails argument validation is regenerated once by a model call
  constrained (`response_format` `json_schema`) to that call's full schema;
  the result and audit record carry `regenerated: true`.

## Bounds and audit

HTTP bodies/responses, model text, turn count, wall time, tool-call count,
request records, session history, and audit retention are explicitly bounded.
The rotating JSONL audit records identities (including the
`assistantComponentId`), exact tool/schema/catalog hashes,
argument hashes, outcomes, and timing. It does not retain prompt text,
response text, or argument values.
