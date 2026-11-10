# Assistant Cookbook

The Composer AI tab lets a chat model edit the Program Draft through a fixed
set of tools. This page covers setup, the checks to run, and common failures.

```text
AI tab -> Composer /api/assistant/requests -> assistant component's bridge (loopback)
       -> your OpenAI-compatible model server ({base_url}/chat/completions)
model tool calls -> bridge (checks arguments) -> Composer /api/assistant/bridge/tools/{tool}
       -> Program Draft (same Draft and fences as manual edits)
operator -> reviews the Draft -> Apply (or Discard)
```

The model server is yours (§1). The Deployment names it with an `ai_model`
component, and an AI assistant component runs the bridge with the model loop
(§2, §3). You review and Apply (§4).

## 1. Run a model server

ManyForge never starts or manages the model server. Run any OpenAI-compatible
server that supports tool calling (`tools`, `tool_choice: "auto"`), reachable
from the ManyForge host. Nothing in ManyForge is tied to a particular model:
the Deployment names the server and the model.

- The served model name must equal the component's `model`.
- `base_url` is the `/v1` root: ManyForge calls `{base_url}/models` and
  `{base_url}/chat/completions`.
- Requests are non-streaming chat completions. They carry no sampling
  parameters unless the profile sets `request_contract.sampling`, so the
  model's or server's defaults apply.
- With constrained regeneration on (the default `composer-assistant` profile),
  a repair call also uses `response_format: {type: "json_schema"}`
  ([§5](#5-what-the-assistant-does-for-you)). A server without structured
  output still works; that repair call just falls back.
- The workbench uses host networking, so `127.0.0.1` in `base_url` is the
  host running ManyForge.

llama.cpp (GGUF):

```bash
llama-server -m /models/gemma-4-12b-it-Q4_K_M.gguf --jinja \
  --alias gemma4-12b-it-gguf --host 127.0.0.1 --port 8000 -c 32768
```

`--jinja` enables the chat template's tool-call format; without it tool
calls are not parsed.

vLLM:

```bash
vllm serve <model path or HF id> --served-model-name <served name> \
  --enable-auto-tool-choice --tool-call-parser <parser for the model family> \
  --host 127.0.0.1 --port 8000
```

Use the tool-call parser (and, for a reasoning model, the reasoning parser)
that the model family documents.

### Example: vLLM on a Jetson AGX Thor

An example only; pick the model and its parsers for your own hardware. It runs
the official `vllm/vllm-openai` container (v0.33) with a Qwen3-family NVFP4
checkpoint:

```bash
docker run --rm --name model-server --runtime nvidia --network host --ipc=host \
  -v <hf cache>:/root/.cache/huggingface \
  -v <flashinfer cache>:/root/.cache/flashinfer \
  vllm/vllm-openai:<v0.33 tag or digest> \
  --model <checkpoint> --served-model-name <served name> \
  --host 127.0.0.1 --port 8000 \
  --gpu-memory-utilization 0.85 --max-num-seqs 4 \
  --kv-cache-dtype bfloat16 --enable-prefix-caching \
  --reasoning-parser qwen3 \
  --enable-auto-tool-choice --tool-call-parser qwen3_coder \
  --disable-flashinfer-autotune
```

- `--disable-flashinfer-autotune`: FlashInfer's autotune can stall a Jetson
  for a long time at startup. Keep the FlashInfer cache mounted so compiled
  kernels persist across restarts.
- A 27B-class model takes about 5–8 minutes to become ready; `/v1/models`
  answers only then.
- Speculative decoding (a draft model or an MTP head) changes only speed,
  not the output distribution.
- Lower `--gpu-memory-utilization` when other GPU work (perception, Isaac Sim)
  shares the device.

Check:

```bash
curl -fsS http://127.0.0.1:8000/v1/models   # must list the model name
```

## 2. Add the AI model and assistant components to the Deployment

The Deployment needs at least one component of kind `ai_model` and one AI
assistant component (kind `assistant`, one per cell) that names it. The
example Scenarios name `gemma4-12b-it-gguf`; set `model` to whatever your
server serves:

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

The assistant component chooses the chat's AI model and its assistant
profile ([Assistant profile](#assistant-profile)); the profile must have a
chat mode:

```json
{
  "component_id": "assistant",
  "component_profile_id": "manyforge.assistant",
  "configuration": {
    "runtime_kind": "assistant",
    "runtime_configuration": {
      "model_component_id": "assistant_model",
      "assistant_profile_id": "assistant_scene_authoring"
    }
  },
  "target_settings": {}
}
```

In Composer:

1. **Components > Custom I/O device or AI model > Add AI model.** Fill in
   Component id (default `assistant_model`), Base URL, Model and, optionally,
   Token environment variable, then **Create AI model**. This adds the
   component to the Deployment Draft only.
2. **Components > Add component > AI assistant.** Pick the entry and **Add to
   Draft**. On its **AI assistant** card choose the **AI model** (preset when
   there is only one) and the **Assistant profile** (profiles with a chat
   mode; default `assistant_scene_authoring`).
3. Edit the AI model fields later in its **AI models** section; with several
   AI models, choose one per runtime alias under **AI model aliases**
   ([Several AI models](#several-ai-models)).
4. **Apply** the Deployment, then Enable the components. Nothing changes for
   the assistant until Apply: until then the AI tab keeps using the Applied
   model and warns "The Deployment Draft names … ; Apply it to use it."

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

The chat alias `assistant_model` is always the AI assistant component's
`model_component_id`; it cannot be mapped in `assistant_model_components`.
The Deployment's own `assistant_profile_id` names the runtime model aliases
of a Program's model query (for example `selection_model` for
`query_model`). With exactly one `ai_model` component, it serves every such
alias. With several, choose the component for each alias in Composer
(**Deployment > Cell settings > AI model aliases**), which writes the
Deployment member `assistant_model_components`:

```json
"assistant_model_components": {
  "selection_model": "selection_model"
}
```

An alias without an entry uses the only `ai_model` component. Compile
refusals: no `ai_model` component gives `assistant_ai_model_component_missing`;
an alias left unmapped with several components, or an entry that names an
unknown alias or a component that is not an `ai_model`, gives
`assistant_model_component_unresolved` (also for an assistant component whose
`model_component_id` names no `ai_model`).

## 3. Start and check

The bridge is the AI assistant component's process; no launcher starts it.
The component follows the usual lifecycle:

- **Prepare** (or Recover) starts the component's host, which binds a
  loopback socket on a free port.
- **Enable** starts the bridge on that socket
  (`python -m manyforge_assistant_bridge.bridge --fd <N>`), waits until its
  `/health` answers and publishes its URL as the component endpoint
  `assistant_request`. Composer reads the URL from there for every call.
- **Disable** stops the bridge: running requests end `cancelled` and the
  conversation history is dropped. After a restart (about 1–2 s, on a new
  port) the old request ids answer `assistant_request_not_found`.
- While a request runs, the component's activity is `busy` (it may lag by up
  to 1 s). Its `bridge` stream is fresh while `/health` answers (budget 5 s;
  stale is a warning).
- If the host or the bridge exits, the component faults
  (`fault_process_exited`); like any component fault it latches the cell and
  blocks Execute. Recover, then Enable.

Enabling the assistant does not depend on the AI model being enabled; the AI
tab and Composer check the model separately. The bridge reads the model
endpoint from the Applied Deployment for every request, so a new model URL or
token variable takes effect after Apply. Its other settings are environment
variables it inherits from the execution host:

| Variable | Default |
|---|---|
| `MANYFORGE_HEADLESS_API_URL` | `http://127.0.0.1:9010` (the launchers pass the Headless URL) |
| `MANYFORGE_COMPOSER_URL` | `http://127.0.0.1:9000` (the launchers pass the Composer port) |
| `MANYFORGE_ASSISTANT_AUDIT_LOG` | Set by the host: `controller-launch-artifacts/<component_id>/assistant-audit.jsonl` under the execution run root |

### Assistant profile

The assistant component's `assistant_profile_id` selects the chat profile from
`manyforge_deployment/resources/maintained/maintained_assistant_profiles.v2.json`;
the example Scenarios use `assistant_scene_authoring`. Only a profile with a
chat mode is accepted. The Deployment's own `assistant_profile_id` selects a
profile only for the prompt templates and model aliases of runtime model
queries ([Several AI models](#several-ai-models)). The profile holds no
endpoint (that is the `ai_model` component); it sets the request limits and
the chat mode's behaviour:

| Field | Meaning | `assistant_scene_authoring` |
|---|---|---|
| `request_contract.default_timeout_s` | Time limit of one model call (`upstream_timeout`); at most 3600 | 180 |
| `request_contract.max_output_tokens` | `max_tokens` of each model call; at most 65536 | 8192 |
| `request_contract.reasoning` | Optional, experimental: `on`/`off` sends `chat_template_kwargs: {enable_thinking: true/false}`; absent sends nothing, so the server's default applies. A constrained-regeneration call always sends `false` when `on` | `off` |
| `request_contract.sampling` | Optional: `{temperature?, top_p?, top_k?}` (`temperature` 0–2, `top_p` above 0 up to 1, `top_k` a positive integer; at least one field) sends exactly the fields set; absent sends no sampling parameters, so the model's or server's defaults apply | not set |
| `stuck_loop_recovery` | Hints after repeated failures and a clarification stop ([§5](#5-what-the-assistant-does-for-you)); default on | on |
| `constrained_regeneration` | Experimental: one schema-constrained repair of an invalid edit ([§5](#5-what-the-assistant-does-for-you)); default off | on |
| `compact_history` | Experimental: keep each finished exchange as request, answer and a list of Draft changes; default off | off |
| `catalog.tools` | The tools the mode offers | 26 tools ([contract](../reference/ASSISTANT_PROVIDER_CONTRACT.md#mode-and-tools)) |

Independently of the profile, one request is limited to 16 model turns and
180 s in total (`assistant_timeout`). A limit above the bridge's caps fails
with `assistant_provider_configuration_invalid`. The profiles ship with
ManyForge; changing one is a source change
([Identities and pins](../reference/IDENTITY_AND_PINNING.md)).

Program model requests that attach an image require a connected Composer
3D viewer. Both Program runner launch paths provide the Composer
`/api/scene/snapshot/render` URL automatically. Open the viewer before running
a vision Scenario and keep it connected so the request can obtain a
generation-matched frame. If Composer has no generation-matched viewer frame,
capture fails with `model_snapshot_observer_unavailable`; if the Composer
snapshot endpoint is unreachable, it fails with
`model_snapshot_image_source_unavailable`.

Prepare and Enable never refuse because of the model server. While the
Deployment runs, each endpoint is supervised as the component stream
`endpoint` of the `ai_model` component: it is probed every 2 s, and its
default budget is 10 s. An unreachable server, or one that does not list the
model, makes the stream stale; it never faults the component and turns fresh
again when the server returns. Override the budget with a
`policies.component_streams` row:

```json
{
  "controllable_component_id": "assistant_model",
  "stream_id": "endpoint",
  "budget_ns": 30000000000
}
```

Quick checks:

```bash
curl -fsS http://127.0.0.1:3000/api/assistant/modes # assistant_component_id, ai_model_component_id set
curl -fsS http://127.0.0.1:3000/api/components      # the assistant is enabled, with an assistant_request endpoint
```

## 4. Use the AI tab

- The AI tab is enabled while the active Deployment's AI assistant component
  and its AI model are enabled and no Program run exists (a paused or held
  run counts). Otherwise its tooltip says why, the first that applies: "Add
  an Assistant component"; "A Program run exists: the assistant runs only
  while no run exists"; "Assistant faulted: Recover it"; "Assistant is
  prepared: Enable it" (or "not prepared"); "Assistant is offline"; "AI model
  `<id>` is …". When the AI model's `endpoint` stream is stale, a yellow
  notice says "AI model `<id>`: endpoint not answering"; sending still works.
- Chat and Program runs never overlap. A chat request is refused while a run
  exists (`assistant_program_active`). While a request from the AI tab runs,
  Execute (also Run, Resume and Retry) is disabled with "An assistant request
  is running: wait or cancel it", and Composer refuses it with
  `execution_assistant_turn_in_progress`. Nothing is cancelled automatically.
  Composer is the only gate: clients that call Headless or the Program runner
  directly must follow the same rule.
- The panel docks at the bottom; the 3D view stays visible. Closing it keeps
  the conversation.
- There is one mode, `composer-assistant` (shown as **Read/Write**). The model
  can read the node catalog, Deployment capabilities, the Program and the
  scene, and edit the scene objects, tree, Program parameters and blackboard
  of the Draft.
- The model may insert any node the palette offers for this cell: every
  stable catalog node whose capabilities the Deployment provides. The set
  follows the Applied Deployment.
- Type a request; Enter sends, Shift+Enter adds a line. While it runs, the
  header shows Working and the elapsed time, and **Cancel** stops it. If the
  outcome is unclear, **Recheck** reads the request status again. **Retry**
  sends the last message again as a new request.
- Each reply shows a summary (tool calls, whether the Draft changed) and a
  list of the Draft changes in plain words. The side column shows the model,
  endpoint and timeout, the mode, the orientation forms the model may use
  (RPY, Delta, Local, World, Quat, Ref), and the tool calls of the last reply
  with their arguments and results.
- A reply marked **needs your answer** means the assistant stopped to ask
  ([§5](#5-what-the-assistant-does-for-you)); answer in the next message.
- **New conversation** clears the context the model sees.

### Review and Apply

- Edits land in the Program Draft immediately, through the same Draft engine
  and revision fences as manual edits. A banner says so: "The assistant edited
  the Program Draft. Apply to use the changes, or Discard the Draft to undo
  them."
- Review the Draft as for any edit: the tree, the scene, and the Draft
  diagnostics. The assistant may leave a composite incomplete while building
  step by step; it shows as a diagnostic, and Apply refuses an incomplete
  Program.
- Apply makes the Draft the active Program. Discard/Revert undoes the
  assistant's edits. There is no separate accept step.
- The model never receives the MCP operator adapter, and it cannot save,
  Apply, run a Program or move a robot.

## 5. What the assistant does for you

These run inside one request and need no action:

- **Field-level corrections.** An invalid tool call is not run. The model gets
  the field path and the expected shape (for example
  `node.parameters.position: expected coordinate_position {kind: ...}`) and
  corrects it in the next turn. The failed call shows as `failed` in the Tool
  calls list; the request continues.
- **Constrained regeneration** (profile `constrained_regeneration`,
  experimental). When a Draft edit has invalid arguments, the assistant asks
  the model once more for that one call, with the call's full JSON Schema as
  a structured-output constraint. A repaired call is checked and run like any
  other and shows `regenerated: true` in its result. If the repair fails (the
  server refuses the schema, times out, or the reply is cut off or not JSON),
  the original error stands and the result shows `regenerationFailed` with
  the reason; the request goes on as usual.
- **Stuck-loop recovery** (profile `stuck_loop_recovery`). When the same tool
  fails with the same kind of error a second time, or the same edit is
  repeated with identical arguments, the model gets a hint to change approach.
  At the fifth repeat the request stops and the assistant asks you for
  clarification, listing what it tried. Rephrase or simplify the request.
- **Turn limit.** After 16 model turns without an answer the request stops
  with "The assistant reached its bounded turn limit before completing." and
  needs your answer.

## 6. Run the Mock smoke scripts

Use a Mock Scenario with an applied Deployment that has an enabled AI
assistant component and a loaded Program Draft, such as `examples/mock_ur10e_assistant_scene_authoring.scenario`. Keep the
Composer and ManyForge services on the same host where the smoke command runs.
For the local setup above, configure the chat component's `base_url` as
`http://127.0.0.1:8000/v1` and set its `model` to the exact served name.

If the component uses a token, export its `MANYFORGE_AI_*` variable in the host
shell before starting the workbench. The launcher forwards these variables by
name; do not put the token on a command line. The smoke scripts read the active
chat alias projection and check that `{base_url}/models` lists the component's
`model` before sending an assistant request.

First run the read-only provider and tool-callback check:

```bash
python3 scripts/smoke-assistant-provider-bridge.py
```

It restricts the model to `program_read` and verifies that the Composer Draft
snapshots before and after the request match. A changed snapshot can reflect a
concurrent Draft edit, so run this check against an isolated Mock Draft. The
complete check adds one uniquely named box through
`scene_draft_add_object`:

```bash
python3 scripts/smoke-assistant-e2e.py --confirm-draft-only
# Equivalent entry point:
scripts/demo-assistant-known-good.sh --confirm-draft-only
```

The scene-only check is also available:

```bash
python3 scripts/smoke-assistant-scene-authoring.py --confirm-draft-only
```

All scripts accept `--composer-url` (default `http://127.0.0.1:3000`, the
Composer frontend proxy; the internal Composer API defaults to port 9020),
`--expected-component-id` to check which AI model serves the chat, and
`--timeout-seconds` (1–900, default 180). For example, if the assistant
component's `model_component_id` is `chat_model`, pass
`--expected-component-id chat_model`.

The full check leaves its new object in the Program Draft. It verifies that
Saved and Applied identities stay the same; it never Saves, Applies, Discards,
runs a Program, or sends a robot command. Review the Draft in Composer and
choose Apply or Discard/Revert yourself. Run it against an isolated Mock Draft,
not while another operator is editing the same Draft. If a request times out or
its result is unclear, inspect request status and the Draft before rerunning;
the scripts submit each request once and do not retry an uncertain outcome.

## Recipes

Run each on a Mock Scenario with an AI assistant component, for example
`examples/mock_ur10e_assistant_scene_authoring.scenario`.

| Recipe | Prompt | Pass signal |
|---|---|---|
| Read | "What objects are in the scene?" | Answer names the scene objects; no Draft change |
| Scene edit + Discard | "Add a 10 cm box named `crate` at x=0.5, y=0, z=0.05." | Box appears in the Draft; Discard removes it; Saved/Applied unchanged |
| Tree edit + Apply | "Insert a move to joint target `home` at the start of the tree." | Node appears in the Draft; Apply makes it the active Program |
| Clarification | "Delete it." (with several candidates) | The assistant asks which one; Draft unchanged |
| Cancel | Send a long request, press Cancel | Status `cancelled`; no further Draft change |
| Disable mid-request | Send a long request, Disable the assistant | The reply fails with a typed reason; the audit records `cancelled`; Enable starts a new, empty conversation |
| Model down | Stop the model server | the `endpoint` stream shows stale on the component; a chat request fails with `upstream_unavailable`; the stream turns fresh after restart |

## Failure codes

A request that fails shows a red reply with a typed code. Codes that end a
request:

| Code | Meaning | What to do |
|---|---|---|
| `assistant_component_absent` | The active Deployment has no AI assistant component | Add one and Apply ([§2](#2-add-the-ai-model-and-assistant-components-to-the-deployment)) |
| `assistant_component_not_enabled` | The assistant is not enabled (or faulted), so no bridge serves | Enable it; Recover it first if it faulted |
| `assistant_program_active` | A Program run exists (also paused or held); the chat runs only while none does | Stop or finish the run |
| `assistant_ai_model_not_enabled` | The assistant's AI model component is not enabled | Enable it |
| `assistant_mode_projection_unavailable` | No Applied Deployment, or its assistant projection is not available | Apply the Deployment and retry |
| `assistant_ai_model_absent` | No `ai_model` component serves the chat alias in the active Deployment | Add one and Apply ([§2](#2-add-the-ai-model-and-assistant-components-to-the-deployment)) |
| `ai_model_token_unavailable` | `api_key_env` names a variable that was not exported before starting the workbench; nothing was sent | Export it, restart the workbench |
| `upstream_unavailable` | The model server cannot be reached | Start it; check `base_url` with `curl {base_url}/models` |
| `upstream_http_error` | The server answered with an HTTP error; the message gives the status (404: wrong model name; 401/403: token; 400: request refused) | See [Troubleshooting](TROUBLESHOOTING.md#ai-assistant) |
| `upstream_timeout` | One model call took longer than `default_timeout_s` | Check the server load; retry when the model is ready |
| `assistant_timeout` | The whole request took longer than 180 s | Ask for less at once, or retry |
| `upstream_response_invalid` / `upstream_response_oversized` | The reply is not one standard chat completion, or exceeds its size bound | Check the server's tool-call parser; try a model with reliable tool calling |
| `assistant_projection_stale` / `assistant_catalog_mismatch` | The active Deployment changed during the request | Retry |
| `assistant_projection_unavailable` | The Applied Deployment is not fully available yet | Wait until Apply completes, then retry |
| `assistant_provider_configuration_invalid` | The profile's limits exceed the bridge's caps | Use a maintained profile |
| `assistant_callback_timeout` | Composer did not answer a Draft edit in time; whether it took effect is unknown | Check the Draft before retrying |
| `assistant_provider_unavailable` / `assistant_provider_timeout` | Composer cannot reach the bridge | Check the assistant component's state and its `bridge` stream (`GET /api/components`) |
| `assistant_request_not_found` | The bridge restarted (Disable/Enable or Recover); its old requests are gone | Send the request again |
| `assistant_request_capacity_exhausted` | Too many requests are running | Wait or cancel one |

Shown inside a reply's Tool calls, not as a failure (the model corrects
itself): `assistant_tool_arguments_invalid` (with field path and expected
shape), `assistant_callback_arguments_invalid`,
`assistant_owner_response_oversized` (read less at once),
`assistant_node_not_found`, an unknown tool name, `regenerationFailed`, and the
`assistant_repeated_tool_failure` recovery hint.

Deployment compile refusals (shown at Apply):

| Code | Meaning |
|---|---|
| `ai_model_api_key_env_invalid` | `api_key_env` does not start with `MANYFORGE_AI_` |
| `ai_model_configuration_invalid` | `base_url` is not an absolute `http(s)` URL, or `model` is empty |
| `assistant_ai_model_component_missing` | The assistant profile needs an `ai_model` component |
| `assistant_model_component_unresolved` | Several `ai_model` components and an alias without a mapping, a mapping to an unknown alias or component, or an assistant component whose `model_component_id` names no `ai_model` |
| `assistant_component_duplicate` | More than one AI assistant component; a cell has at most one |
| `assistant_profile_has_no_chat_mode` | The assistant component's profile has no chat mode |

Execute refusal: `execution_assistant_turn_in_progress` (a chat request from
Composer is running; wait for it or cancel it).

The bridge writes a bounded audit log (`MANYFORGE_ASSISTANT_AUDIT_LOG`, 10 MiB per
file, the active file plus 3 backups;
`controller-launch-artifacts/<component_id>/assistant-audit.jsonl` under the
execution run root, see
[Observability](OBSERVABILITY.md#assistant-bridge-audit)). It
holds identities, hashes and outcomes, never prompt text, responses or
argument values.
