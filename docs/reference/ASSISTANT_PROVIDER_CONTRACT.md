> **Historical reference only.** This contract describes the last committed full Composer/assistant interaction tree at `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1` (tree `bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f`). It is not a current API contract or qualification claim. The strict current wire is documented in `ASSISTANT_PROVIDER_CONTRACT_V1.md`; this file remains the frozen v0 behavioral reference. The current `scripts/regen-public-contracts.py` owns only `REST_API.md` and deliberately does not regenerate this file.

<!--
  FROZEN HISTORICAL GENERATED SNAPSHOT — DO NOT REGENERATE.
  The metadata below records the former generator at the captured revision.
  The current REST-only generator does not own or update this file.
  Last regenerated: 2026-07-21 22:04:29 UTC
  Former generator source: scripts/regen-public-contracts.py:gen_assistant_provider_contract
-->

# Assistant Provider Contract
Composer dispatches assistant requests to a *provider* HTTP service which speaks the `manyforge.assistant.provider_request.v0` envelope. Composer's lane registry is the owner of canonical provider IDs, aliases, routing, isolation, and default bridge ports.

## Registered engine-backed lanes

| Canonical ID | Aliases | Port | Routing | Declared runtime | Declared isolation |
|---|---|---:|---|---|---|
| `direct` | `nemoclaw` | `8100` | `direct-vllm` | `nemoclaw` | `sandbox` |
| `hermes` | — | `8300` | `hermes-sessions` | `nemoclaw` | `sandbox` |
| `openclaw` | — | `8200` | `openclaw-gateway` | `nemoclaw` | `sandbox` |

The in-tree `manyforge_assistant_bridge` implements the direct lane. NemoClaw-Thor supplies the OpenClaw and Hermes adapters. All registered lanes use the same provider envelope and remain subject to Composer's mode/catalog/authority checks. The runtime and isolation values are Composer catalog declarations, not live process-placement attestation: the standard in-tree launcher starts the direct bridge as a host peer, while a platform deployment may sandbox the same bridge. Verify the selected topology with the lane bring-up runbook and live process/health checks.

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
  "providerId": "direct" | "openclaw" | "hermes" | "nemoclaw" | "...",
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
