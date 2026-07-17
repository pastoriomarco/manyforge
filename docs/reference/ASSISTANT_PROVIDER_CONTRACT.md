<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd /workspace
    python3 scripts/regen-public-contracts.py
  Last regenerated: 2026-07-16 23:53:24 UTC
  Generator source: scripts/regen-public-contracts.py:gen_assistant_provider_contract
-->

# Assistant Provider Contract
Composer dispatches assistant requests to a *provider* HTTP service which speaks the `manyforge.assistant.provider_request.v0` envelope. Two reference providers ship with manyforge:

- `nemoclaw` lane: `manyforge_assistant_bridge` on port 8100 —   runs the agent loop in-process; calls the model directly.   Same repo as this doc.
- `openclaw` lane: `openclaw_assistant_bridge` on port 8200 —   dispatches into a NemoClaw sandbox running OpenClaw. Lives in   the platform repo (NemoClaw-Thor) under `manyforge/`.

Both speak the same wire envelope; this doc covers the wire format the provider must accept.

## Bridge HTTP routes (machine-introspected)

| Method | Path | Summary |
|---|---|---|
| `GET` | `/healthz` | Healthz |
| `POST` | `/v1/manyforge/assistant` | Assistant |
| `POST` | `/v1/manyforge/assistant/{request_id}/cancel` | Cancel Request |
| `GET` | `/v1/sessions/stats` | Session Stats |
| `POST` | `/v1/sessions/{conversation_id}/clear` | Clear Session |

## Envelope wire format

The request body sent by Composer to the provider on `POST /v1/manyforge/assistant`:

```json
{
  "version": "manyforge.assistant.provider_request.v0",
  "schemaVersion": "0.1.0",
  "requestId": "<uuid>",
  "conversationId": "<uuid>",
  "principal": "<operator id>",
  "assistantMode": "composer-assistant" | "query" | ...,
  "message": "<user prompt>",
  "tools": [ {"id": "...", "description": "...", "parameters": {...}}, ... ],
  "constraints": { "mutatesState": bool, "requiresReview": bool, "proposalStatus": "draft", ... },
  "context": { "catalogHash": "<sha256>", "programRevision": "...", ... },
  "programSnapshot": { ... },
  "sceneSnapshot": { ... },
  "nodeCatalog": [ ... ],
  "skillCatalog": [ ... ]
}
```

## Catalog-hash gate

Every envelope carries a `context.catalogHash` computed by the shared protocol helper. Any provider implementation must validate the hash before executing tool calls; mismatch → HTTP 409 with `code: catalog_mismatch`. Hash function lives in:

  `manyforge_assistant_bridge/protocol.py:compute_catalog_hash`

Both the Composer manifest emitter and the bridge consumer compute the hash from the same canonical input shape, so byte-identical output is required across the wire.

## Provider response

On success, the provider returns:

```json
{
  "requestId": "<echo>",
  "conversationId": "<echo>",
  "providerId": "nemoclaw" | "openclaw" | "...",
  "message": "<final assistant text>",
  "proposals": [ ... ],          // draft mutations the operator must accept
  "toolCalls": [ ... ],          // executed callbacks (audit trail)
  "warnings": [ ... ],
  "draftMutated": bool,
  "openclaw": { ... }            // optional provider-specific telemetry
}
```

Errors return non-200 with:

```json
{ "requestId": "<echo>", "code": "<short_id>", "detail": "<human-readable>" }
```

## Code source

- `manyforge_assistant_bridge/bridge.py` — direct lane provider (this repo).
- `manyforge_assistant_bridge/protocol.py` — shared hashing helpers.
- `manyforge_composer/backend/routes_assistant.py` — Composer-side   envelope assembly + tool-callback endpoints (`/api/assistant/chat`,   `/api/assistant/bridge/tools/<tool_id>`).
