# Assistant Provider Contract

What ManyForge expects from the model server behind the AI tab, and how the
pieces connect. The Composer-to-bridge wire is specified in
[`ASSISTANT_PROVIDER_CONTRACT_V1.md`](ASSISTANT_PROVIDER_CONTRACT_V1.md).
Setup steps: [`../operations/ASSISTANT_E2E_COOKBOOK.md`](../operations/ASSISTANT_E2E_COOKBOOK.md).

## Topology

| Part | Role |
|---|---|
| Model server (yours) | OpenAI-compatible HTTP server with tool calling. ManyForge never starts it. |
| `ai_model` Deployment component | Names the server: `base_url`, `model`, optional `api_key_env` (a `MANYFORGE_AI_*` name). One or more per Deployment; the assistant component names the chat's, and `assistant_model_components` maps the runtime aliases to them (optional with exactly one). |
| `assistant` Deployment component | At most one per cell: `model_component_id` (the chat's `ai_model`) and `assistant_profile_id` (a profile with a chat mode). Its host starts the bridge at Enable and publishes the bridge URL as the component endpoint `assistant_request`. |
| Assistant profile | Selects the mode (`composer-assistant`), its tools, stuck-loop recovery and the request limits. |
| `manyforge_assistant_bridge` | Runs the bounded model loop on a loopback socket with an ephemeral port, from the assistant's Enable to its Disable. |
| Composer | Serves the AI tab and the Draft tool callback (`/api/assistant/bridge/tools/{tool}`); finds the bridge through the assistant component's endpoint for every call. |
| Headless | Publishes the active assistant mode projection, including the assistant and model components, the model endpoint and the nodes the assistant may author, built from the node catalog it loaded. |

## Model endpoint

For every request the bridge reads the active mode projection. Its
`model_endpoint` carries:

| Field | Source |
|---|---|
| `ai_model_component_id` | The `ai_model` component that serves the mode's model alias |
| `base_url` | Component `base_url` (the `/v1` root) |
| `model_id` | Component `model` |
| `api_key_env` | Component `api_key_env` (string or `null`) |
| `request_contract` | Assistant profile (per-call timeout, output tokens, optional `reasoning` and `sampling`, tool-call family) |

The bridge then sends `POST {base_url}/chat/completions` with:

- `model` = `model_id` (the served model name must match exactly);
- `messages`, `tools` (exact owner-issued JSON schemas), `tool_choice: "auto"`,
  `max_tokens` from the request contract;
- no sampling parameters, so the model's or server's defaults apply, unless
  the request contract sets the optional `sampling: {temperature?, top_p?,
  top_k?}`; then exactly the fields it sets are sent (`temperature` 0–2,
  `top_p` above 0 up to 1, `top_k` a positive integer; at least one field;
  the maintained profiles set none);
- `chat_template_kwargs: {enable_thinking: true|false}` when the request
  contract sets the optional `reasoning: on|off` (experimental; absent keeps
  the model's default; the maintained chat profile sets `off`);
- `Authorization: Bearer <value of $api_key_env>` when `api_key_env` is set.
  An unset variable fails the request with `ai_model_token_unavailable`
  before anything is sent.

For a constrained regeneration (below) the request instead carries
`response_format: {type: "json_schema", json_schema: {name: "tool_arguments",
strict: true, schema}}` and `tool_choice: "none"`, the same sampling
parameters, and, when the request contract sets `reasoning: on`,
`chat_template_kwargs: {enable_thinking: false}`, so that small structured
call does not think. The schema includes
`"x-guidance": {lenient: true, ...}` options for llguidance-based servers
(vLLM's guidance backend; with vLLM's `auto` backend these schemas resolve to
it). A server that refuses the schema makes only that repair call fail.

One request is bounded to 16 model turns, 64 tool calls per turn and 180 s in
total (`assistant_timeout`). The request contract may not exceed the bridge's
caps (65,536 output tokens, 3,600 s per call;
`assistant_provider_configuration_invalid`).

The response must be a standard non-streaming chat completion with one choice
and OpenAI `tool_calls`. A call to a tool outside the active mode is never
run; it gets a typed error naming the closest listed tool. Every failure
caused by a call's arguments (in any tool, reads included) is a tool result
the model can correct; only infrastructure failures (model server,
projection, Composer contract, time) end the request.

Prepare and Enable never refuse because of the model server. Each `ai_model`
component has one stream, `endpoint`: every 2 s a probe sends
`GET {base_url}/models` and records a sample only when the server answers 2xx
and lists the configured model (budget 10 s). A stale `endpoint` stream shows
only in the component's stream freshness; it is never a fault and never
latches the cell.

## Mode and tools

There is one mode, `composer-assistant`:

| Effect | Tools |
|---|---|
| Read | `catalog_read`, `deployment_capabilities_read`, `inspect_isaac_scene`, `program_read`, `scene_inspect`, `skills_read` |
| Draft edit | `scene_draft_{add_object, update_object, upsert_objects, remove_objects}`, `tree_draft_{insert_node, update_node_params, delete_node, move_node, replace_subtree, change_node_kind, wrap_node, rename_node}`, `program_draft_{upsert,remove}_{parameters,contact_policies}`, `blackboard_draft_{upsert,remove}_{namespaces,keys}` |

`inspect_isaac_scene` is offered only when the Deployment provides the
simulator capability `simulator.entity_state.set`.

A node's `contact_policy_id` names a Program contact policy
(`manyforge.contact_policy.v1`); `planning.plan_pick_sequence` needs one with
mode `contact_grasp`. The assistant declares it with
`program_draft_upsert_contact_policies`, as the Program contact policy editor
does; Composer checks it and computes its `version`.

- Draft edits go through Composer's Draft engine under the same revision
  fences as manual edits. The user Applies, or Discards/Reverts, the Draft.
  There is no proposal or accept step.
- The assistant may author a node when its catalog entry is
  `stability: stable` and the active Deployment offers every capability the
  entry requires: the nodes the GUI palette offers for this cell. There is no
  per-mode node list; the set follows the Deployment at each Apply.
- The system prompt is generated: the rules (placement, one `instance_id`
  per new node, build step by step), the cell (one line per component with
  its targets, frames, input and output signals and mount, and one line of
  the Deployment's interrupt rules, all from the request's `selectorFacts`;
  at most 12 names per list),
  an outline of the Program Draft (tree, Program parameters, blackboard
  keys, scene resources, contact policies as `id: mode` with each zone's
  body pairs and its maximum penetration or release evidence, open
  diagnostics) and the authorable nodes by
  category as `id: summary` (the first sentence of the catalog description).
  Each part names the tool that gives more: `deployment_capabilities_read`,
  `program_read` (`instance_id` reads one subtree), `catalog_read`.
- Tool schemas are top-level only. An argument holding a domain value names
  its type (`node`, `wrapper_node`, `replacement_node`, `scene_resource`,
  `scene_resource_edit`, `parameter_declaration`, `contact_policy`,
  `blackboard_key`, `blackboard_namespace`); `catalog_read` with `types` returns any type's
  shape, and with `include_parameters` an entry's parameters (description,
  required, type) and every `schema_types` definition they use, in the
  catalog's own notation (`required_when` / `allowed_when`).
- The bridge checks shapes and types only (known fields, value types, ids,
  JSON), against the full type and the node's catalog schema. Completeness
  (missing children or required parameters) is a Draft diagnostic, as in the
  GUI; Apply refuses an incomplete Program. A failure is
  `assistant_tool_arguments_invalid` listing every error of the closest
  form, each with its field `path` and the `expected` shape, for example
  `node.parameters.position: expected coordinate_position {kind: angular_rad|linear_m|named_preset, ...}`.
- The model sees concise tool results: `{ok, changed, diagnostics_new,
  diagnostics_other}` for an edit (the Draft and reference diagnostics this
  edit caused, and the count of those already open, with up to 3 of their
  paths in `diagnostics_other_at` when short; the reference diagnostics open
  at the request's start count as already open), `{ok, errors}` for a failure, and the
  content for a read.
- A Draft edit or read that Composer refuses comes back as a callback result
  of kind `validation_error` (with `httpStatus` and `message`) whose
  `detail.code` is the typed refusal code: the Draft engine's code (for
  example `draft_arguments_invalid`, `draft_node_not_found`,
  `composer_authoring_child_capacity_exceeded`), or
  `assistant_callback_arguments_invalid` for Composer's own argument schema
  check. The model's `errors[].code` and the stuck-loop error class are that
  code.
- Stuck-loop recovery (mode setting `stuck_loop_recovery`, on by default):
  when the same tool fails with the same error class a second time, or the
  same Draft edit succeeds again with identical arguments (class
  `repeated_identical_edit`), the tool result carries a typed `recovery` object (`code`
  `assistant_repeated_tool_failure`, `toolId`, `errorClass`, `repeats`,
  `action`, `hint`, `clarificationRequired`); on the fifth repeat the
  request ends with `needsClarification: true` and `exitReason:
  needs_clarification`. With the setting off, neither happens.
- History compaction (mode setting `compact_history`, off by default;
  experimental): the conversation history keeps
  each completed request as the user message and one assistant message with
  the final answer and a generated `Draft changes:` list (what each committed
  edit added, updated or removed). Intermediate tool calls, their results and
  failed calls are left out. The running request always keeps its full
  exchange.
- Constrained regeneration (mode setting `constrained_regeneration`, off by
  default, on in the maintained `composer-assistant` profile; experimental): a call that fails
  argument validation gets one extra model call for the same tool. It carries
  the conversation, the failed call's errors and a request for corrected
  arguments only, with `response_format` `json_schema` set to the full JSON
  Schema of that call (typed arguments expanded; a tree node resolved by its
  `node_id` to its own parameter schema; a node without a known `node_id` and
  further children constrained only to the node keys, accepted `node_id`s and
  plain `parameters`; a container's child count left to validation). The
  reply is validated and executed like any call, at most once per failed
  call; its tool result and audit record carry `regenerated: true`. When the regeneration fails (a truncated or non-JSON
  reply, or any upstream failure of that call such as a refused grammar or a
  timeout), the original call and its error stand and the tool result carries
  `regenerationFailed` with the typed reason.
- The model never gets the MCP operator adapter, and it cannot save, Apply,
  run a Program or command a robot.

## Failures

Failures are typed `{code, message}` errors on the request status. See the
cookbook's failure-code table.
