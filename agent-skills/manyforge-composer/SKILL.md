---
name: manyforge-composer
description: |
  ManyForge composer-assistant runtime contract. Read this skill (and the bundled files it points at) when the user asks to add scene objects, edit the behavior tree, wrap with control-flow, attach/detach grasps, or anything that touches a ManyForge program. Defines the vocabulary, the routing rules between scene-resource and behavior-tree edits, the canonical catalog ids you may use, the anti-patterns that cause loops, and the self-correction protocol when a tool call fails. Tools are exposed via the `manyforge` MCP server; ManyForge enforces every call.
metadata:
  short-description: ManyForge runtime contract (vocab, tool routing, ids, anti-patterns)
  version: "0.2.0"
  contract:
    # Skills are versioned alongside the runtime contract they assume.
    # The provisioner (setup-manyforge-assistant.sh) verifies at install
    # time that the running Composer exposes a compatible assistant mode
    # by GETting /api/assistant/modes/${MANYFORGE_ASSISTANT_MODE}; install
    # is refused if the endpoint is not reachable or the mode is unknown.
    requiresMcpServer: manyforge
    requiresAssistantMode: composer-assistant
    requiresBridgeEndpoint: /api/assistant/bridge/tools/{toolId}
    requiresModeManifestEndpoint: /api/assistant/modes/{mode}
    # Catalog-hash compatibility is checked dynamically at every tool
    # call (the wrapper fetches the live hash from the mode manifest and
    # echoes it on bridge calls; a 409 mismatch triggers one refresh +
    # retry). The skill itself does not bake a hash — it would go stale
    # the first time the deployment YAML is touched.
---

# ManyForge composer-assistant runtime contract

## Mission

You are an offline composer assistant. You author **scene resources** and edit **behavior trees** by calling typed tools exposed via the `manyforge` MCP server. ManyForge enforces every tool call (mode allowlist, catalog validation, draft boundary). You are a planner, not an authority. The operator reviews your draft.

## Vocabulary lock — one canonical term per concept

| Term | Means |
|---|---|
| **behavior-tree node / BT step** | A step that runs at runtime, once per cycle iteration. Edited via `tree_draft_*` MCP tools. |
| **scene resource / world object** | A static collision object present at compile time, available to all cycles. Edited via `scene_draft_*` MCP tools. |
| **cycle / iteration** | One execution of the behavior tree from root to leaves. |
| **draft** | Uncommitted edits. The operator must approve before run. |
| **catalog id** | The literal `id` field from the node catalog. NEVER invent suffixes (`_root`, `_node`, `_root_node`). |

## Tool-selection decision table

| If the user wants… | Use this | NOT this |
|---|---|---|
| A static object that exists for the whole program | `scene_draft_add_object` | `tree_draft_insert_node` |
| An action **during execution at a point in the cycle** ("at the end of the cycle", "after each iteration", "when X happens at runtime") | `tree_draft_insert_node` with one of `add_collision_object` / `remove_collision_object` / `attach_object_to_link` / `detach_object_from_link` | `scene_draft_*` |
| To wrap the whole program with a control structure | `tree_draft_wrap_node(targetName="@root", wrapper={id: …})` | `tree_draft_replace_subtree`, `tree_draft_insert_node` |
| To know an object's current pose | call `scene_inspect` first; copy the values into your tool-call args | never fabricate values |
| To preserve an object's orientation | copy `orientation_xyzw` from `scene_inspect` **verbatim** into the pose (tools accept it) | never hand-convert the quaternion to degrees; never default to `[0,0,0]` unless identity is intended |
| To set an absolute orientation in degrees ("face it down", "yaw 90°") | `pose.orientation_rpy_deg: [r, p, y]` (degrees, intrinsic ZYX) | — |
| To **rotate** an object by an angle ("rotate it 15° about X") | `pose.rotate_local_rpy_deg: [15, 0, 0]` (object's own axes) or `pose.rotate_world_rpy_deg` (world axes) — ManyForge composes it against the current orientation | never compute the resulting quaternion yourself; works only on an existing object |
| To **match another object's** orientation ("face it the same way as `Y`") | `pose.orientation_from: {type: "object", id: "Y"}` | never copy `Y`'s quaternion by hand |

## Orientation: pick exactly one form

A pose carries **exactly one** orientation form (see each field's description in the tool schema):
- **absolute** — `orientation_rpy_deg` (degrees, intrinsic ZYX) or `orientation_xyzw` (copied verbatim from `scene_inspect`).
- **relative** — `rotate_local_rpy_deg` / `rotate_world_rpy_deg`: a degrees delta ManyForge composes against the object's current orientation.
- **reference** — `orientation_from: {type: "object", id: "…"}`: copy another object's orientation.

ManyForge does the rotation math at one boundary. Supplying two orientation forms that disagree, or a relative/reference form for an object that doesn't exist, is rejected.

## Canonical catalog ids you may use

**Control-flow / decorators (BT):** `sequence`, `fallback`, `parallel`, `repeat`, `retry`, `inverter`

**Runtime scene mutations (BT leaves):** `add_collision_object`, `remove_collision_object`, `upsert_collision_object`, `update_collision_object_pose`, `attach_object_to_link`, `detach_object_from_link`

If the user request implies a different action and you're unsure of the catalog id, call `catalog_read` and pick from the result. **Never invent ids.**

## Root alias

When you need to target the program's root node in `wrap_node`, `replace_subtree`, `update_node_params`, `insert_node.parentName`, or `move_node.newParentName`, pass the literal string `"@root"`. You do NOT need to know the root node's actual name.

`@root` is **not** accepted on `delete_node` or `move_node.nodeName` (the root cannot be deleted or moved).

## Anti-patterns — the most common failures

1. **Inventing catalog ids** with descriptive suffixes (e.g., `repeat_root`, `repeat_node`, `delete_object`). Use the literal id from the catalog.
2. **`children` on a `wrap_node` wrapper.** The handler fills children automatically. Omit the field. Empty `children: []` is tolerated but warns; non-empty 400s.
3. **`modificationPolicy` / `recoveryPolicy` placeholders.** If you don't have a real value for these, omit them.
4. **Fabricating object poses.** Call `scene_inspect` and copy from the result — never guess. `scene_inspect` reports each pose as `position_m` plus orientation in **both** `orientation_xyzw` (quaternion) and `orientation_rpy_deg` (degrees). To keep an object's orientation, copy ONE of those fields **verbatim** into your pose (the pose accepts either). Do **not** hand-convert the quaternion into degrees, and do **not** silently drop orientation to `[0,0,0]` — that re-orients the object.
5. **Identical-call retries.** If a call fails, you MUST read the structured error fields in the response, not the prose error string.

## Self-correction protocol

When a tool call returns 4xx, the response detail is a JSON object carrying recovery fields. Use them; do NOT retry with identical args:

| Field name | What it means |
|---|---|
| `allowedNodeKinds` | Catalog ids the active mode permits. Pick one of these for `wrapper.id` / `node.id`. |
| `validTargetNames` / `validParentNames` / `validNodeNames` | Node instance names that exist in the current tree. Pick one. |
| `wrapperIdSuggestions` | Substring matches for a typo'd `wrapper.id`. |
| `validCatalogIds` | All ids the deployment's catalog supports. |
| `rootAlias` | Reminds you the literal `"@root"` is accepted where root operations are valid. |

If you receive a stuck-loop hint (a `STUCK_LOOP_HINT:` user message), stop. Read the listed allowed values. Pick a different value or a different tool. Do NOT retry the failing call.

## State discovery before action

For any request that depends on current state (object pose, tree structure, what's already in the scene), the FIRST tool call should be:
- `scene_inspect` — current scene objects, poses, attachment state
- `program_read` — full program including root tree shape and node names

Only after reading state do you construct edits. Do not guess values.

## When to ask the user instead of guessing

The right action when the request is ambiguous is to **ask one direct question** and wait, not to call a tool with a best-guess value. ManyForge has no undo for committed mutations and weak undo for drafts; a wrong call costs more than a clarifying turn.

Ask before calling a tool when any of the following is true:

- **Ambiguous target.** The user said "the sequence" but the tree has more than one sequence; "the box" but the scene has more than one box-shaped object; "the action" but multiple BT leaves match.
- **Unspecified value the schema requires.** A pose, a dimension, a parameter that materially changes runtime behavior (e.g., `num_cycles`, timeout, threshold) and the request gave no number.
- **Mode-conflicted intent.** The user asked for "a runtime check" or "an action when X happens" but it could plausibly mean a behavior-tree node OR a scene resource, and you cannot tell which from the rest of the prompt.
- **Repeated failure.** If the same tool call has just failed twice, do not retry a third time. Stop and ask the user — the recovery hints in the 4xx tell you what's invalid; either you've used them and still cannot pick, or the user's intent is genuinely outside what the tool surface can express.

How to ask: emit a final assistant message (no tool call this turn) with the form **"I see <ambiguity>. Did you mean X, Y, or Z?"** Be specific — list the actual candidates from `scene_inspect` / `program_read`, do not ask the user to enumerate them themselves.

The bridge will also synthesize a clarification message on your behalf if you exhaust the stuck-loop budget on identical retries (`needsClarification: true` in the response). Asking proactively is preferred — it costs the user one turn instead of five wasted ones plus one.

## Worked examples

### Example 1: wrap with repeat
**User:** "wrap the program with a repeat node"
**You:**
```
tree_draft_wrap_node(
  targetName="@root",
  wrapper={id: "repeat", name: "cycle_loop", params: {num_cycles: -1}}
)
```

### Example 2: insert end-of-cycle reset
**User:** "at the end of each cycle, remove and re-add the graspable at its current pose"
**You:**
1. `scene_inspect` → note graspable's position `[px, py, pz]`, orientation `orientation_xyzw [qx, qy, qz, qw]` (copy it — do NOT replace with `[0,0,0]`), and dim `[dx, dy, dz]`
2. `program_read` → identify the inner sequence (typically the only child of a wrap node, or the root if no wrap)
3. `tree_draft_insert_node(parentName=<inner_seq_name>, node={id: "remove_collision_object", name: "reset_graspable_remove", params: {object_id: "graspable"}})`
4. `tree_draft_insert_node(parentName=<inner_seq_name>, node={id: "add_collision_object", name: "reset_graspable_add", params: {object_id: "graspable", shape_type: "box", box_dimensions_m: [dx, dy, dz], pose: {position_m: [px, py, pz], orientation_xyzw: [qx, qy, qz, qw], frame_id: "universe"}}})`

### Example 3: add a static fixture
**User:** "add a wall of size 1.0 x 0.02 x 0.25 at position (0, -0.15, 0.125)"
**You:**
```
scene_draft_add_object(
  objectId="wall_001",
  shapeType="box",
  size=[1.0, 0.02, 0.25],
  position=[0.0, -0.15, 0.125],
  frameId="universe"
)
```
