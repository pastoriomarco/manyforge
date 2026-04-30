# manyforge_assistant_bridge — assistant-provider HTTP bridge (v0.1)

Implements the assistant-provider HTTP contract against a local
OpenAI-compatible LLM endpoint (typically vLLM on the deployment host).
Receives ManyForge envelopes, validates them per the bounded-autonomy
spec, runs a bounded multi-turn loop against the upstream LLM, executes
only `effect: read_only` tools through ManyForge callbacks, and returns
ManyForge-shaped responses.

This is the local enforcement point for the bounded-autonomy invariant
defined in `manyforge_specs/docs/spec/480-assistant-modes-and-bounded-autonomy.md`.
Architectural design: `manyforge_specs/docs/spec/485-assistant-bridge-architecture.md`.
Wire contract: `../docs/reference/ASSISTANT_BACKEND_CONTRACT.md`.

## What this does

- `POST /v1/manyforge/assistant` — accepts the ManyForge envelope,
  validates per spec 480 §6.3 (fail-closed on missing `assistantMode`,
  malformed catalog, etc.), filters `tools[]` to the embedded catalog,
  converts ManyForge tool descriptors to OpenAI shape, maps ManyForge
  ids such as `program.read` to model-safe function names, dispatches
  to the upstream LLM, executes in-catalog `effect: read_only` calls via
  Composer callbacks, hard-rejects any out-of-catalog tool calls, and
  returns a ManyForge-shaped envelope with `mutated: false`,
  `requiresReview: true`.
- `POST /v1/manyforge/assistant/{requestId}/cancel` — marks a running
  or not-yet-started request as cancelled. The bridge checks this before
  each model turn and before each read-only callback; cancelled requests
  return an error envelope with `error.code: "cancelled"`.
- `GET /healthz` — liveness + upstream URL + resolved model identifier.

## Tool effects

- `read_only` — executed in-loop through
  `POST <manyforge>/api/assistant/bridge/tools/{toolId}`. Callback
  results are fed back to the model as tool results.
- `proposal` — never executed by the bridge. The bridge stops the loop
  and returns draft proposals for ManyForge review.
- `runtime_mutating` — never executed by the bridge. These are runtime
  action descriptors for ManyForge policy/recovery paths.

## Setup

```bash
cd manyforge/manyforge_assistant_bridge
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Running

Default config talks to `http://127.0.0.1:8000/v1` and listens on
`127.0.0.1:8100`. The model identifier is auto-discovered from the
upstream `/v1/models` endpoint; set `BRIDGE_MODEL` to pin explicitly.

```bash
.venv/bin/python -m manyforge_assistant_bridge.bridge
```

Environment overrides:

| Variable | Default | What |
|---|---|---|
| `BRIDGE_HOST` | `127.0.0.1` | bind address |
| `BRIDGE_PORT` | `8100` | bind port |
| `BRIDGE_UPSTREAM_BASE_URL` | `http://127.0.0.1:8000/v1` | OpenAI-compatible LLM endpoint |
| `BRIDGE_MANYFORGE_BASE_URL` | `http://127.0.0.1:9000` | ManyForge Composer API base URL for read-only callbacks |
| `BRIDGE_MODEL` | _auto-discovered_ | model identifier sent in chat-completion requests; auto-discovery picks the first model from `/v1/models` if unset |
| `BRIDGE_AUDIT_LOG` | `manyforge_assistant_bridge/audit.log` | JSONL audit trail |
| `BRIDGE_REQUEST_TIMEOUT_S` | `60` | per-upstream-call timeout cap |
| `BRIDGE_CALLBACK_TIMEOUT_S` | `10` | per-read-only-callback timeout |
| `BRIDGE_MAX_TURNS` | `4` | maximum model turns per request |
| `BRIDGE_MAX_WALL_TIME_S` | `60` | maximum wall time per bridge request |

## Wiring ManyForge to the bridge

Set `MANYFORGE_ASSISTANT_ENDPOINT_URL` on the ManyForge composer side:

```
MANYFORGE_ASSISTANT_ENDPOINT_URL=http://127.0.0.1:8100/v1/manyforge/assistant
```

The provider in `manyforge_composer/backend/assistant_provider.py`
reads this when the deployment artifact's `assistant.provider` is set
to `nemoclaw` or `openclaw` (per `manyforge_specs` 430 §3.5D).

## Health probe

```bash
curl -s http://127.0.0.1:8100/healthz | python3 -m json.tool
```

Expected:

```json
{
  "status": "ok",
  "version": "manyforge.assistant.provider_request.v0",
  "schemaVersion": "0.1.0",
  "upstreamBaseUrl": "http://127.0.0.1:8000/v1",
  "manyforgeBaseUrl": "http://127.0.0.1:9000",
  "model": "<resolved-model-id>",
  "modelSource": "explicit" | "auto" | "unresolved",
  "maxTurns": 4,
  "maxWallTimeS": 60.0
}
```

## Audit log

JSONL, append-only. One record per request, including:

- `requestId`, timestamp, `assistantMode`,
- deterministic `catalogHash` over the resolved `{skills, tools, nodes}` set,
- `model` actually called,
- `responseShape` summary (tool-call names, proposal count, warnings),
- `outOfMode` rejections (when applicable, with argument hashes),
- `durationMs`, `turnCount`, `exitReason` (`ok` / `proposal_returned` /
  `envelope_rejected` / `response_rejected` / `model_unresolved` /
  `upstream_http_error` / `timeout` / `upstream_call_error` /
  `max_turns_exceeded` / `cancelled`).

## Trust model (v0.1)

ManyForge composer and the bridge run as peer processes on the same
operator-controlled host. The bridge trusts the catalog ManyForge
sends; there is no inter-process auth or catalog signing. Multi-host
or untrusted-peer hardening (signed catalogs, mutual TLS, relocating
composition into the bridge with a declared deployment-artifact source)
is tracked in the bridge architecture spec as future work.
