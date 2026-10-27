# Assistant Cookbook

The Composer AI tab lets a chat model edit the Program Draft through a fixed
set of tools. This page covers setup, the checks to run, and common failures.

```text
AI tab -> Composer /api/assistant/requests -> manyforge_assistant_bridge (:8100)
       -> your OpenAI-compatible model server ({base_url}/chat/completions)
model tool calls -> bridge -> Composer /api/assistant/bridge/tools/{tool}
       -> Program Draft (same Draft and fences as manual edits)
```

## 1. Run a model server

ManyForge never starts or manages the model server. Run any OpenAI-compatible
server that supports tool calling (`tools`, `tool_choice: "auto"`), reachable
from the ManyForge host.

- The served model name must equal the component's `model`.
- `base_url` is the `/v1` root: ManyForge calls `{base_url}/models` and
  `{base_url}/chat/completions`.

llama.cpp (GGUF):

```bash
llama-server -m /models/gemma-4-12b-it-Q4_K_M.gguf --jinja \
  --alias gemma4-12b-it-gguf --host 127.0.0.1 --port 8000 -c 32768
```

`--jinja` enables the chat template's tool-call format; without it tool
calls are not parsed.

vLLM:

```bash
vllm serve <model path or HF id> --served-model-name gemma4-12b-it-gguf \
  --enable-auto-tool-choice --tool-call-parser <parser for the model family> \
  --host 127.0.0.1 --port 8000
```

Check:

```bash
curl -fsS http://127.0.0.1:8000/v1/models   # must list the model name
```

## 2. Add the AI model component to the Deployment

The Deployment needs an assistant profile (`assistant_profile_id`, for example
`assistant_scene_authoring`) and at least one component of kind `ai_model`:

```json
{
  "component_id": "assistant_model",
  "component_profile_id": "ai_model_v0",
  "configuration": {
    "runtime_kind": "ai_model",
    "runtime_configuration": {
      "base_url": "http://127.0.0.1:8000/v1",
      "model": "gemma4-12b-it-gguf",
      "api_key_env": null
    }
  },
  "target_settings": {}
}
```

You can also add it in the Deployment editor: **AI models > Add AI model**
(fields Component id, Base URL, Model, Token environment variable).

- `base_url`: absolute `http(s)` URL of the `/v1` root.
- `model`: the served model name.
- `api_key_env`: `null` for a server without authentication, or the **name**
  of an environment variable that holds the token. The name must start with
  `MANYFORGE_AI_` (for example `"MANYFORGE_AI_CHAT_TOKEN"`); any other name
  is refused with `ai_model_api_key_env_invalid`. Run
  `export MANYFORGE_AI_<NAME>=<token>` before starting the workbench; the
  launcher forwards every `MANYFORGE_AI_*` variable into the Workbench
  container. ManyForge reads the variable at call time and sends
  `Authorization: Bearer <token>`. The token itself is never stored in the
  Deployment, the bundle, logs or audit. If the variable is unset, requests
  fail with `ai_model_token_unavailable` and no request is sent.

### Several AI models

An assistant profile names one or more model aliases (for example
`assistant_model` for the chat and `selection_model` for a Program's model
query). With exactly one `ai_model` component, it serves every alias. With
several, choose the component for each alias in the Deployment editor
(**AI models > Assistant profile**), which writes the Deployment member
`assistant_model_components`:

```json
"assistant_model_components": {
  "assistant_model": "chat_model",
  "selection_model": "selection_model"
}
```

An alias without an entry uses the only `ai_model` component. Compile
refusals: no `ai_model` component gives `assistant_ai_model_component_missing`;
an alias left unmapped with several components, or an entry that names an
unknown alias or a component that is not an `ai_model`, gives
`assistant_model_component_unresolved`.

## 3. Start and check

The launcher always starts the bridge on `127.0.0.1:8100` and passes
`--assistant-bridge-url` to Composer. Nothing else needs configuring.

With the default `endpoint` stream setting (warning, auto), Prepare and Enable never refuse because of the model server. While the
Deployment runs, each endpoint is supervised as the component stream
`endpoint` of the `ai_model` component: it is probed every 2 s, and its
default budget is 10 s, severity `warning` and recovery `auto`. An unreachable
server, or one that does not list the model, raises a warning that clears by
itself when the server returns. Override severity, recovery or budget with a
`policies.component_streams` row:

```json
{
  "controllable_component_id": "assistant_model",
  "stream_id": "endpoint",
  "budget_ns": 30000000000,
  "severity": "warning",
  "recovery": "auto"
}
```

Quick checks:

```bash
curl -fsS http://127.0.0.1:3000/api/health          # assistant_bridge: configured
curl -fsS http://127.0.0.1:3000/api/assistant/modes # ai_model_component_id set
```

## 4. Use the AI tab

- The AI tab is enabled when the bridge is configured and the active
  Deployment has an AI model component and an assistant profile. Otherwise
  it shows "No AI model component in this Deployment".
- There is one mode, `composer-assistant`. The model can read the node
  catalog, skills, Deployment capabilities, Program and scene, and edit the
  scene objects, tree, parameters and blackboard of the Draft.
- Edits land in the Program Draft immediately, through the same Draft engine
  and revision fences as manual edits. There is no separate accept step:
  review the Draft, then Apply it, or Discard/Revert it, as for manual edits.
- The chat may insert the motion, gripper, scene, logic, composite, timer and
  simulation nodes of its allowlist. It cannot insert pick/place planning
  nodes (`planning.plan_pick_sequence`, `planning.plan_place_sequence`); add
  those by hand.
- The model never receives the MCP operator adapter, and it cannot save,
  Apply, run a Program or move a robot.
- Cancel stops an in-flight request. New conversation clears the history.

## Recipes

Run each on a Mock Scenario with an assistant profile, for example
`examples/ur10e_assistant_scene_authoring.scenario`.

| Recipe | Prompt | Pass signal |
|---|---|---|
| Read | "What objects are in the scene?" | Answer names the scene objects; no Draft change |
| Scene edit + Discard | "Add a 10 cm box named `crate` at x=0.5, y=0, z=0.05." | Box appears in the Draft; Discard removes it; Saved/Applied unchanged |
| Tree edit + Apply | "Insert a move to joint target `home` at the start of the tree." | Node appears in the Draft; Apply makes it the active Program |
| Refused node | "Insert a plan_pick_sequence node." | The model reports it cannot; Draft unchanged |
| Cancel | Send a long request, press Cancel | Status `cancelled`; no further Draft change |
| Model down | Stop the model server | `endpoint` warning in the component streams; a chat request fails with `upstream_unavailable`; warning clears after restart |

## Failure codes

| Code | Meaning |
|---|---|
| `assistant_mode_projection_unavailable` | The active Deployment has no assistant profile |
| `assistant_ai_model_absent` (chat) | No `ai_model` component serves the chat alias in the active bundle |
| `ai_model_token_unavailable` | `api_key_env` names a variable that was not exported before starting the workbench |
| `ai_model_api_key_env_invalid` (compile) | `api_key_env` does not start with `MANYFORGE_AI_` |
| `assistant_model_component_unresolved` (compile) | Several `ai_model` components and an alias without a mapping, or a mapping to an unknown alias or component |
| `upstream_unavailable` / `upstream_timeout` / `upstream_http_error` | The model server is down, slow, or refused the request (check URL, token, served name) |
| `upstream_response_invalid` / `response_rejected` | The model returned malformed output or called a tool outside the mode; try a model with reliable tool calling |
| `assistant_projection_stale` / `assistant_catalog_mismatch` | The active Deployment changed during the request; retry |

The bridge writes a bounded audit log (`MANYFORGE_ASSISTANT_AUDIT_LOG`, default
`$XDG_STATE_HOME/manyforge/assistant-bridge-audit.jsonl`, 10 MB x 3 files). It
holds identities, hashes and outcomes, never prompt text, responses or
argument values.
