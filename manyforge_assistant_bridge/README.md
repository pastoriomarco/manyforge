# manyforge_assistant_bridge

The bridge behind the Composer AI tab. It runs the bounded model loop between
Composer, the active headless assistant mode projection, and the model server
named by the Deployment's `ai_model` component.

The bridge:

- re-reads and verifies the active mode projection before every model turn;
- takes the model endpoint from that projection (`model_endpoint.base_url`,
  `model_id`, `api_key_env`) and calls `POST {base_url}/chat/completions`,
  sending `Authorization: Bearer $<api_key_env>` when `api_key_env` is set
  (unset variable: `ai_model_token_unavailable`, nothing sent);
- exposes only the exact owner-issued tool schemas of mode
  `composer-assistant`;
- runs read tools against headless and Draft-edit tools through the
  revision-fenced Composer callback, so edits land in the Program Draft;
- owns bounded request records, conversation history, cancellation,
  stuck-loop handling and a redacted rotating audit log; and
- never saves, Applies, mutates runtime state or commands a robot.

When the active Deployment has no AI model component, requests fail with
`assistant_ai_model_absent` (no assistant profile:
`assistant_mode_projection_unavailable`).

Run `python -m manyforge_assistant_bridge.bridge`. The launchers start it
automatically and pass `--assistant-bridge-url http://127.0.0.1:8100` to
Composer. Environment:

| Variable | Default |
|---|---|
| `MANYFORGE_HEADLESS_API_URL` | `http://127.0.0.1:9010` |
| `MANYFORGE_COMPOSER_URL` | `http://127.0.0.1:9000` |
| `MANYFORGE_ASSISTANT_BRIDGE_HOST` | `127.0.0.1` |
| `MANYFORGE_ASSISTANT_BRIDGE_PORT` | `8100` |
| `MANYFORGE_ASSISTANT_AUDIT_LOG` | `$XDG_STATE_HOME/manyforge/assistant-bridge-audit.jsonl` (10 MiB per file; active plus 3 backups, up to 4 files / 40 MiB total by default) |

The model URL, model name and token variable come only from the Deployment;
there is no environment override.

`protocol.py` exports the strict wire and projection verification helpers.
`headless.py` is the closed headless read client. `supervisor.py` holds a pure
availability reducer.

See [`docs/operations/ASSISTANT_E2E_COOKBOOK.md`](../docs/operations/ASSISTANT_E2E_COOKBOOK.md)
and [`docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`](../docs/reference/ASSISTANT_PROVIDER_CONTRACT.md).
