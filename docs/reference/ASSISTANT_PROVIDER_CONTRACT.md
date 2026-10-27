# Assistant Provider Contract

What ManyForge expects from the model server behind the AI tab, and how the
pieces connect. The Composer-to-bridge wire is specified in
[`ASSISTANT_PROVIDER_CONTRACT_V1.md`](ASSISTANT_PROVIDER_CONTRACT_V1.md).
Setup steps: [`../operations/ASSISTANT_E2E_COOKBOOK.md`](../operations/ASSISTANT_E2E_COOKBOOK.md).

## Topology

| Part | Role |
|---|---|
| Model server (yours) | OpenAI-compatible HTTP server with tool calling. ManyForge never starts it. |
| `ai_model` Deployment component | Names the server: `base_url`, `model`, optional `api_key_env` (a `MANYFORGE_AI_*` name). One or more per Deployment; `assistant_model_components` maps profile aliases to them (optional with exactly one). |
| Assistant profile (`assistant_profile_id`) | Selects the mode (`composer-assistant`), its tools and allowed nodes, and the request limits. |
| `manyforge_assistant_bridge` (`127.0.0.1:8100`) | Runs the bounded model loop; always started by the launcher. |
| Composer | Serves the AI tab and the Draft tool callback (`/api/assistant/bridge/tools/{tool}`). |
| Headless | Publishes the active assistant mode projection, including the model endpoint. |

## Model endpoint

For every request the bridge reads the active mode projection. Its
`model_endpoint` carries:

| Field | Source |
|---|---|
| `ai_model_component_id` | The `ai_model` component that serves the mode's model alias |
| `base_url` | Component `base_url` (the `/v1` root) |
| `model_id` | Component `model` |
| `api_key_env` | Component `api_key_env` (string or `null`) |
| `request_contract` | Assistant profile (timeout, output tokens, tool-call family) |

The bridge then sends `POST {base_url}/chat/completions` with:

- `model` = `model_id` (the served model name must match exactly);
- `messages`, `tools` (exact owner-issued JSON schemas), `tool_choice: "auto"`,
  `temperature: 0`, `max_tokens` from the request contract;
- `Authorization: Bearer <value of $api_key_env>` when `api_key_env` is set.
  An unset variable fails the request with `ai_model_token_unavailable`
  before anything is sent.

The response must be a standard non-streaming chat completion with one choice
and OpenAI `tool_calls`. Any tool outside the active mode rejects the whole
response (`response_rejected`).

With the default `endpoint` stream setting (warning, auto), Prepare and Enable never refuse because of the model server. At runtime each
`ai_model` component has one supervised stream, `endpoint` (probe every 2 s,
budget 10 s, severity `warning`, recovery `auto`; override with a
`policies.component_streams` row).

## Mode and tools

There is one mode, `composer-assistant`:

| Effect | Tools |
|---|---|
| Read | `catalog_read`, `deployment_capabilities_read`, `inspect_isaac_scene`, `program_read`, `scene_inspect`, `skills_read` |
| Draft edit | `scene_draft_{add_object, update_object, upsert_objects, remove_objects}`, `tree_draft_{insert_node, update_node_params, delete_node, move_node, replace_subtree, change_node_kind, wrap_node, rename_node}`, `program_draft_{upsert,remove}_parameters`, `blackboard_draft_{upsert,remove}_{namespaces,keys}` |

- Draft edits go through Composer's Draft engine under the same revision
  fences as manual edits. The user Applies, or Discards/Reverts, the Draft.
  There is no proposal or accept step.
- `tree_draft_insert_node` accepts only the profile's node allowlist (motion,
  gripper, scene, logic, composites, timer, simulation nodes). Pick/place
  planning nodes are not in it.
- The model never gets the MCP operator adapter, and it cannot save, Apply,
  run a Program or command a robot.

## Failures

Failures are typed `{code, message}` errors on the request status. See the
cookbook's failure-code table.
