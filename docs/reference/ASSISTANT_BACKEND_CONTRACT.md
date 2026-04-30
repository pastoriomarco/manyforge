# Assistant Backend Contract

This document captures the current Composer assistant backend contract. It is a
reference for the deterministic tool-driven scene-authoring path and the future
NemoClaw/OpenClaw-backed provider.

## Contract Boundary

The assistant is advisory by default. It may inspect available context, call
registered tools, and return proposals, but it must not directly mutate the
loaded program, runtime scene, files, or simulator state.

Authoritative state remains in the ManyForge backend. User-visible program
changes must pass through Composer's explicit review/apply/discard flow. This
keeps assistant output auditable and prevents model output from becoming an
implicit source of truth.

## Provider Modes

`mock`

Default scaffold mode. It returns a deterministic mock answer and does not call
external services.

`deterministic-scene-authoring`

Local non-LLM provider used to validate the provider contract before live model
integration. It consumes an Isaac collider probe report supplied in chat context
and generates scene-resource proposals. When requested, it can also generate
tree-node proposals for add/remove/attach/detach object operations.

`nemoclaw` / `openclaw`

External HTTP provider adapter for a NemoClaw/OpenClaw-hosted model/tool loop.
ManyForge sends the user message, server-side context, allowed tools, and hard
constraints. The provider must return a response message, tool-call records,
warnings, and structured proposals. ManyForge rejects provider responses that
claim mutation or violate the normalized proposal/tool-call contract.

When the loaded deployment declares `assistant_modes`, provider-backed requests
use the v0.1 semantic schema on the stable
`manyforge.assistant.provider_request.v0` wire family. Composer must include
`assistantMode`, and the provider only receives the skills, tools, and primitive
nodes allowed by that mode.

Model serving profiles, memory budgets, and Thor/Orin deployment tradeoffs are
owned outside this contract by NemoClaw-Thor:

```text
/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor/MANYFORGE-ASSISTANT-DEPLOYMENT-PLAN.md
```

This contract intentionally stays model-agnostic. Any live provider must pass
through the same proposal and validation boundaries regardless of which model
profile serves it.

Providers can be selected with:

```bash
python -m manyforge_composer --assistant-provider deterministic-scene-authoring
```

or:

```bash
MANYFORGE_ASSISTANT_PROVIDER=deterministic-scene-authoring \
python -m manyforge_composer
```

Live NemoClaw/OpenClaw provider configuration:

```bash
python -m manyforge_composer \
  --assistant-provider nemoclaw \
  --assistant-endpoint http://127.0.0.1:8100/v1/manyforge/assistant
```

Equivalent environment variables:

```bash
MANYFORGE_ASSISTANT_PROVIDER=nemoclaw
MANYFORGE_ASSISTANT_ENDPOINT_URL=http://127.0.0.1:8100/v1/manyforge/assistant
MANYFORGE_ASSISTANT_TIMEOUT_S=30
MANYFORGE_ASSISTANT_API_TOKEN=
```

## Live Provider Request Contract

ManyForge sends this JSON shape to the configured live provider:

```json
{
  "version": "manyforge.assistant.provider_request.v0",
  "schemaVersion": "0.1.0",
  "requestId": "mf-generated-request-id",
  "providerId": "nemoclaw",
  "conversationId": "stable-conversation-id",
  "assistantMode": "query",
  "message": "user request",
  "requestedTools": ["program.read"],
  "context": {},
  "runtime": {
    "programLoaded": false,
    "cycleState": "idle"
  },
  "tools": [
    {
      "id": "program.read",
      "effect": "read_only",
      "description": "Read the loaded program."
    }
  ],
  "skills": [],
  "nodes": [],
  "catalog": {
    "skills": [],
    "tools": ["program.read"],
    "nodes": []
  },
  "constraints": {
    "mutatesState": false,
    "requiresReview": true,
    "proposalStatus": "draft",
    "allowedToolCallStatuses": ["proposed", "skipped", "completed", "failed"]
  }
}
```

`schemaVersion` and `assistantMode` are mandatory when the request is governed
by deployment-declared assistant modes. Legacy deployments without
`assistant_modes` may still send the older v0 request shape without those
fields while the v0.1 bridge is being rolled in.

The provider response must be a JSON object:

```json
{
  "message": "assistant response",
  "toolCalls": [],
  "proposals": [],
  "warnings": [],
  "mutated": false,
  "requiresReview": true
}
```

ManyForge forces returned proposals to `status: draft` and
`reviewRequired: true` before they reach Composer.

For assistant-mode-aware requests, provider `toolCalls[].name` values must
reference tools in the active mode catalog. Out-of-catalog tool-call references
are rejected before they reach Composer.

## Bridge Multi-Turn and Callback Contract

The ManyForge bridge may run a bounded multi-turn loop. It executes only
`effect: read_only` tools in-loop, and it executes them only through Composer's
read-only callback surface:

`POST /api/assistant/bridge/tools/{tool_id}`

Request:

```json
{
  "requestId": "mf-generated-request-id",
  "conversationId": "stable-conversation-id",
  "assistantMode": "query",
  "catalogHash": "sha256-of-mode-catalog",
  "arguments": {}
}
```

Response:

```json
{
  "toolId": "program.read",
  "success": true,
  "result": {},
  "error": null,
  "mutated": false,
  "timestampNs": 123456789
}
```

Composer revalidates the active deployment mode, recomputes the catalog hash,
checks that `{tool_id}` is in the active mode catalog, and rejects any callback
for a tool whose deployment `tool_catalog` effect is not `read_only`. The
callback endpoint never mutates Composer, runtime, files, or simulator state.

Initial read-only callback tools:

- `program.read`
- `program.validate`
- `status.read`
- `scene.inspect`
- `inspect_isaac_scene`

The bridge stops the loop when the model returns no tool calls, when it returns
`proposal` or `runtime_mutating` descriptors, when `BRIDGE_MAX_TURNS` or
`BRIDGE_MAX_WALL_TIME_S` is reached, or when the request is cancelled. Proposal
and runtime-mutating descriptors are returned as reviewable drafts; the bridge
does not execute them.

Bridge cancellation:

`POST /v1/manyforge/assistant/{requestId}/cancel`

The bridge records the cancellation request and checks it before model turns and
before read-only callbacks. A cancelled provider request returns an error
envelope with `error.code: "cancelled"`, no proposals, and `mutated: false`.

## HTTP Surface

`POST /api/assistant/chat`

Runs the configured provider. `mode: "mock"` forces the mock provider. `mode:
"provider"` uses the configured provider and fails if no provider is available
or if the requested `providerId` does not match. If the loaded deployment
declares `assistant_modes`, `mode: "provider"` also requires `assistantMode`.

`GET /api/assistant/providers`

Lists available provider modes and the currently configured provider.

`GET /api/assistant/tools`

Lists backend tools available to assistant workflows.

`POST /api/assistant/tools/inspect-isaac-scene`

Calls the Isaac scene inspection tool. This is read-only and may require the ROS
scene-probe service to be reachable.

`POST /api/assistant/tools/propose-scene-objects`

Converts an Isaac collider probe report into ManyForge scene-resource
proposals.

`POST /api/assistant/tools/propose-scene-object-nodes`

Converts scene resources into draft behavior-tree object-operation nodes. This
currently proposes nodes only; insertion point selection and tree mutation are
deferred.

`POST /api/assistant/proposals/stage`

Stores a provider/tool proposal in the backend as an explicit review artifact.

`GET /api/assistant/proposals`

Lists staged or discarded proposal records, optionally filtered by lifecycle
status or conversation id.

`POST /api/assistant/proposals/{proposal_id}/discard`

Marks a staged proposal as discarded.

`POST /api/assistant/proposals/{proposal_id}/materialize`

Validates a staged proposal and returns a Composer session patch. The endpoint
does not save files and does not bypass the existing Composer session
apply/discard mechanism.

## Proposal Lifecycle

`draft`

Provider or tool output that has not yet been accepted into backend review
state.

`staged`

Proposal stored by the backend for explicit review.

`discarded`

Proposal rejected by the user or workflow.

`materialized`

Validated session patch generated from a staged proposal. At this stage the
proposal is still not a saved program change; Composer's normal session
apply/discard controls remain authoritative.

Current materialization support is intentionally narrow. `scene_resource_add`
can produce a validated `sessionPatch.scene` entry. `tree_node_add` can be
staged and reviewed, but materialization is blocked until the UI/API has an
explicit insertion-point contract.

## Tool Semantics

`inspect_isaac_scene`

Read-only simulator inspection. It should return the raw scene/collider report
needed for downstream selection and approximation decisions. The assistant may
summarize the result, but the user remains responsible for deciding which
objects matter.

`propose_scene_objects`

Deterministic conversion from collider report to ManyForge scene resources. It
must preserve source paths and approximation metadata so decisions remain
traceable.

`propose_scene_object_nodes`

Deterministic creation of object-operation node drafts from reviewed scene
resources. It must not guess a behavior-tree insertion point.

## Test Contract

Live Isaac Sim and live ROS are not required for deterministic backend tests.
Use fixture-backed reports and the deterministic provider to test the full
inspect-to-propose contract without depending on simulator availability.

The live HTTP provider boundary can be tested without inference by running the
fake provider fixture:

```bash
./scripts/fake-assistant-provider.py --host 127.0.0.1 --port 8100
```

Then configure the live adapter against:

```bash
MANYFORGE_ASSISTANT_PROVIDER=nemoclaw
MANYFORGE_ASSISTANT_ENDPOINT_URL=http://127.0.0.1:8100/v1/manyforge/assistant
```

The fixture returns deterministic review-required scene-resource proposals and
also supports forced mutation, malformed response, timeout, and HTTP-error
cases for adapter contract tests.

The same endpoint can be checked directly with the v0.1 assistant-mode smoke
harness:

```bash
python scripts/smoke-assistant-provider-bridge.py \
  --endpoint http://127.0.0.1:8100/v1/manyforge/assistant \
  --assistant-mode composer-assistant \
  --requested-tool inspect_isaac_scene \
  --requested-tool propose_scene_objects
```

When the real `manyforge_assistant_bridge` service is running, use its bridge
URL. NemoClaw-Thor owns the upstream model serving stack the bridge consumes;
the bridge itself lives in the ManyForge repository. The smoke harness
validates response shape, rejects direct mutation claims, and fails if returned
tool calls reference tools outside the declared assistant-mode catalog.

The current fixture path is:

```text
manyforge_composer/tests/fixtures/isaac_scene_authoring_soup_can.json
```

Live ROS/Isaac checks remain separate smoke tests. They validate deployment
plumbing and service compatibility, not the deterministic provider contract.

The repeatable smoke command for the full backend proposal lifecycle is:

```bash
./scripts/smoke-assistant-scene-authoring.py \
  --transport api \
  --workflow proposal-lifecycle \
  --fixture manyforge_composer/tests/fixtures/isaac_scene_authoring_soup_can.json \
  --include-subtree /World/soup_can \
  --source-root /World/soup_can \
  --object-id-prefix sim_
```

This covers scene inspection with fixture data, scene-resource proposal,
backend staging, and materialized session-patch generation. It must leave the
loaded program unchanged.

## Safety Notes

The assistant backend must stay non-mutating until the user explicitly reviews
and applies a proposed change. Runtime lockouts and program/session validation
remain backend responsibilities, not model responsibilities.

Assistant output is never sufficient evidence that a scene is safe. For
industrial use, generated scene objects and tree edits require human review,
validation, and qualification against the target robot/simulator cell.
