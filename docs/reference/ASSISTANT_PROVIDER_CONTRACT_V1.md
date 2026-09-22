# Assistant Provider Contract v1

This is the current strict wire for the bounded direct/`nemoclaw` assistant
bridge. Normative authority and mode semantics live in ManyForge specs 480 and
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

Bridge service:

| Method | Route | Meaning |
|---|---|---|
| `GET` | `/health` | Process/configuration liveness, not model or runtime readiness |
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
- forwarded proof input: the complete `modeProjection` and exact
  `resolvedCatalog` (`catalogHash`, `grantPolicy`, `nodes`, `skills`, `tools`);
- Composer context: `composerDraft` with Draft identity/revision, the
  composite `expectedWorkspaceEpoch`/`expectedWorkspaceRevision` of the shared
  authoring workspace, the current Kernel `expectedAdmissionGeneration`,
  optional Program snapshot, and the node-environment/source-model projection
  digests.

Composer captures that backend admission generation and that composite
workspace identity when it accepts the assistant request. Every callback
rechecks them through the normal authoring owner, and a mutating callback
commits under the same composite fence an ordinary edit uses. Browser snapshot
fields are deliberately absent: the GUI is a disposable presentation client,
so its disconnect or snapshot expiry cannot become assistant authority or
interrupt accepted backend work.
Each Composer callback returns its exact settled `nextAdmissionGeneration` and
its exact `nextWorkspaceRevision`. The bridge chains both backend-issued values
for a later callback in the same assistant request; read-only or rejected
callbacks leave the generation unchanged and never decrease the workspace
revision. A committed edit advances the workspace revision, a validated no-op
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
`manyforge.assistant.provider_response.v1`. It echoes attribution and mode,
records the narrowed `catalogHash`, text, reviewable `proposals`, completed
`toolCalls`, warnings, Draft provenance, clarification flag, exit reason, and
bounded turn count. Proposal/runtime descriptors are never executed in-loop.

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
- Any out-of-catalog model call rejects the entire response.
- Known callback arguments are validated against the exact schema hash before
  execution. Composer independently re-verifies the active projection,
  narrowing, schema hash, Draft ID, and expected Draft revision.
- Each Composer callback uses a deterministic idempotency identity derived
  from the provider request owner/identity, model turn, upstream call identity,
  and call position. Composer replays an exact durable outcome before
  consulting mutable Draft/admission context; changed intent under the same
  identity conflicts.
- Draft, node, skill, and tool-result content is untrusted model data, never an
  instruction source. Only the bridge-owned system rules and exact retained
  instrument contract grant model actions.
- Repeated identical validation failures receive one bounded correction hint
  and then terminate as a clarification instead of looping indefinitely.
- Conversation history is memory-, count-, TTL-, mode-, and catalog-bounded;
  it is never runtime or Draft state.

## Bounds and audit

HTTP bodies/responses, model text, turn count, wall time, tool-call count,
request records, session history, and audit retention are explicitly bounded.
The rotating JSONL audit records identities, exact tool/schema/catalog hashes,
argument hashes, outcomes, and timing. It does not retain prompt text,
response text, or argument values.

## Historical reference

The pre-relocation v0/v0.1 Composer-owned provider contract remains preserved
in `ASSISTANT_PROVIDER_CONTRACT.md` and at commit
`08222188293df8e12f90dc72e204c21f22f92ea1`. It is continuity evidence, not a
loader, compatibility alias, or runnable fallback.
