# LLM Operability Plan for ManyForge

**Goal**: Make ManyForge fully operable and observable by LLM agents so that an
LLM can build, start, program, monitor, debug, and adapt a robot cell within
explicit approval, authority, audit, and safety boundaries suitable for
industrial deployment.

ManyForge must remain independently operable through its own REST/MCP surfaces.
External orchestration stacks such as NemoClaw/OpenClaw are optional consumers
of those surfaces, not prerequisites for core operability.

**Date**: 2026-04-11

## Open Points

- Semantic object selection demo currently runs with image input disabled by
  default because `gemma-4-E4B-it` behaved more reliably on the same selection
  task in direct text-only tests than in the current multimodal path.
- The image path remains an open investigation item. Compare at least:
  - current base64 `data:` URL transport
  - HTTP-served temporary snapshot URLs
  - local-media-path server configuration in vLLM when/if enabled
- Do not re-enable image-based selection in the demo until one of those paths
  shows stable behavior and traceability that is better than the text-only
  baseline.
- The new response-tool path for `query_model` is currently verified with
  `Qwen3.5-9B-Claude-Distilled-NVFP4`. Tool-call compatibility across other
  allowed local models remains an explicit open point.
- Do not assume tool-call compatibility from the provider name alone. Models
  such as `gemma-4-E4B-it` may require a different chat template, parser, or
  fallback mode than Qwen-family models. ManyForge should maintain a
  deterministic compatibility matrix or per-endpoint capability flags rather
  than asking an LLM which models are safe to use.
- Composer should expose deployment endpoint aliases, configured model IDs, and
  deterministic `/v1/models` discovery directly in the UI. Assistant model
  selection must remain separate from per-node model selection.

---

## Recent Findings and Resolutions

### 2026-04-13 to 2026-04-14: Semantic Selection + Composer Model UX

- **Problem**: The semantic selection path was too brittle when the model was
  asked to emit a large structured JSON response directly.
  **Resolution**: `query_model` now supports a deployment-defined
  `response_tool`, and the semantic selection demo uses a deterministic
  `select_scene_object` response tool instead of relying on free-form JSON
  formatting.

- **Problem**: Prompt placeholders written as `{{ variable }}` with whitespace
  were not being rendered, which made the model behave as if the selection
  request had not been provided.
  **Resolution**: prompt rendering now supports whitespace-tolerant placeholder
  substitution in `query_model`.

- **Problem**: The multimodal text+image selection path was less reliable than
  the text-only path for the current local models.
  **Resolution**: the semantic selection demo currently runs text-only by
  default; the image path remains an explicit open investigation item rather
  than a silently degraded default.

- **Problem**: The old output schema was strict in the wrong place; the model
  often selected the right object but failed on non-critical diagnostic field
  formatting.
  **Resolution**: the execution-critical contract is now enforced through the
  response tool arguments, while optional diagnostics remain looser.

- **Problem**: Tool-call success is model-specific. Qwen-family local models
  worked with the current tool-call setup, but that does not imply Gemma-family
  compatibility.
  **Resolution**: this is now tracked explicitly as a compatibility/open-point
  item. ManyForge should maintain deterministic per-endpoint capability
  knowledge instead of assuming compatibility from provider type alone.

- **Problem**: Composer’s `query_model` node UI was misleading. The node
  referenced a deployment alias, but the deployment metadata was not surfaced,
  so the field looked ignored when the demo script rewrote the actual model ID
  at launch.
  **Resolution**: Composer now exposes deployment `model_endpoints` and
  `prompt_templates`, adds deterministic `/v1/models` discovery, and shows the
  resolved provider/base URL/configured model for the selected alias.

- **Problem**: The semantic demo alias name `gemma4_vlm` became misleading once
  the launch script started swapping model IDs at runtime.
  **Resolution**: the semantic demo now uses a role-based alias
  `selection_model`, which matches what the node actually means.

- **Problem**: The `template_variables` parameter in Composer rendered as
  `[object Object]`, making it effectively unusable.
  **Resolution**: `template_variables` now has a typed row editor with explicit
  variable-name and blackboard-path fields instead of a raw string fallback.

- **Problem**: The `model_endpoint_alias` and `prompt_template_id` fields in
  Composer looked interactive even when only one valid deployment option
  existed.
  **Resolution**: when only one valid deployment alias/template exists, the UI
  now renders resolved read-only info rather than a meaningless single-option
  dropdown. If multiple aliases/templates exist, the UI still renders a real
  selector.

- **Problem**: Deployment-side model endpoint configuration could not be edited
  coherently in Composer, so aliases were effectively fixed by the demo script.
  **Resolution**: the Deployment panel now exposes model endpoint aliases and
  their configurable fields (`provider`, `modelId`, `baseUrl`, `apiKeyEnv`,
  `defaultTimeoutS`, `maxTokens`) directly in the Composer UI.

---

## 1. Current State Inventory

### 1.1 What an LLM Can Already Do

| Capability | Interface | Readiness |
|-----------|-----------|-----------|
| Build the container | `docker compose build` / `Dockerfile` multi-stage (includes FastAPI/uvicorn/httpx test/runtime deps) | Ready |
| Start the application | `python -m manyforge_composer --catalog-path ... --hmi-port 8081` | Ready |
| Load a program + deployment | `POST /api/program/load`, `POST /api/deployment/load` | Ready |
| Read the behavior tree | `GET /api/program/tree` (JSON TreeNode) | Ready |
| Write/modify the behavior tree | `PUT /api/program/tree` | Ready |
| Query available node types | `GET /api/catalog` (full schema, params, constraints) | Ready |
| Validate a candidate tree before applying/running | `POST /api/program/validate` | Ready |
| Run / stop / reset execution | `POST /api/cycle/run\|stop\|reset` | Ready |
| Adjust velocity | `PUT /api/cycle/velocity-scale` | Ready |
| Monitor execution state | `ws://host:9000/ws/state` at 10Hz, `GET /api/execution/digest` | Ready |
| Query full scene state in one call | `GET /api/scene/state` | Ready |
| Mutate runtime scene | `POST /api/scene/edit` | Ready |
| Compare backend scene truth vs HMI truth | `GET /api/scene/diagnostics` | Ready |
| Query kernel snapshot via REST | `GET /api/kernel/snapshot` | Ready |
| Query kernel metrics via REST | `GET /api/kernel/metrics` | Ready |
| Review execution history after a run | `GET /api/execution/history` | Ready |
| Inspect py_trees blackboard state | `GET /api/execution/blackboard` | Ready |
| Export durable debug bundle | `GET /api/debug/export` | Ready |
| Discover deployment/backend capabilities | `GET /api/capabilities` | Ready |
| Query validated high-level skills | `GET /api/skills` | Ready |
| Use local MCP tool surface | stdio MCP server (`manyforge_*` tools) | Ready |
| Execute temporary intervention/recovery sessions without rewriting the saved artifact | `/api/direct/*` + MCP intervention tool | Ready (externally serviced / policy-scoped) |
| Inspect and materialize runtime overrides | `/api/program/runtime-overrides*` + MCP parity | Ready |
| Query individual object state | `kernel.get_object_state(id)`, `kernel.get_object_pose(id, frame)` | Ready (Python only, no REST) |
| Query execution snapshot | `kernel.get_execution_snapshot()` | Ready (Python only, no REST) |
| Debug scene rendering | `GET /api/debug/scene-trace` | Ready |
| Get a simulated scene snapshot | `GET /api/scene/snapshot` | Ready |
| Save modified program | `POST /api/program/save` | Ready |
| Browse filesystem for files | `GET /api/files/browse` | Ready |

### 1.2 What an LLM Cannot Do Yet

| Capability | Gap | Severity |
|-----------|-----|----------|
| Stream full native kernel events in real time | WS now carries recorder-derived execution events, but there is still no native C++ event subscription path for low-level events such as collision detections | **Medium** |
| Auto-execute bounded recovery from inside the saved program without external servicing | No typed deterministic recovery nodes or `program_only` internal recovery executor yet | **High** |
| Use contact-aware execution policies | No contact-policy model or API; collision handling is still binary | **High** |
| Perceive the real-world scene | No camera integration in ManyForge itself | **Low** (camera_vllm_bridge exists separately) |
| Serve MCP over an external-agent-friendly transport | MCP exists in-process/stdin only; no HTTP/SSE transport for sandboxed remote agents | **Medium** |
| Use multi-model `query_model` safely across deployment/model families | Initial `query_model` support exists, but per-endpoint capability flags, formal response-tool compatibility rules, and assistant-vs-node model separation are still incomplete | **Medium** |
| Offer a design-time Composer chat assistant | No assistant endpoint or UI panel for tree proposals/interactive debugging | **Low** |
| Provide closed, typed delegated-recovery contracts across all recovery paths | Pending recovery is enforced today, but typed recovery nodes, closed trigger taxonomy, and per-node recovery policy are not yet implemented | **High** |

---

## 2. Architecture for LLM Operability

### 2.1 Design Principles

1. **REST-first**: Every piece of state an LLM needs must be queryable via a
   single HTTP GET. WebSocket is for push notifications, not primary data access.
2. **Composite endpoints over granular ones**: An LLM has limited tool-call budget.
   One `/api/scene/state` returning everything is better than 10 calls to
   individual object queries.
3. **Human-readable JSON**: Field names should be self-documenting. Include units
   in field names (`_m`, `_rad`, `_mps`, `_ns`).
4. **Polling-friendly**: LLM agents poll; they don't maintain persistent WS
   connections. Endpoints must return the latest state, not require subscription.
5. **Validation before execution**: An LLM should be able to test its tree edits
   without risking runtime crashes.
6. **Traceability**: Every execution must produce a queryable history so the LLM
   can learn from what happened.
7. **Correlation model**: Every event, snapshot, and API response must carry
   consistent correlation IDs (`timestamp_ns`, `run_id`, `execution_id`,
   `move_id`, `node_instance`, `object_id`, `source`) so an LLM can trace
   causality across multiple surfaces without manual join logic.
8. **Multi-truth distinction**: REST APIs must explicitly distinguish backend
   runtime truth (collision scene), HMI/render truth (Viser visualization), and
   optional perception truth (camera/VLM). Never collapse them into one ambiguous
   snapshot — an LLM debugging a discrepancy needs to compare planes, not guess
   which one a field came from.
9. **ROS2 compatibility when present**: If ManyForge is deployed alongside
   ROS2, the observability/control sidecar should map 1:1 to ros2_medkit
   capabilities wherever semantically applicable, while keeping ManyForge-
   specific features (behavior-tree authoring, blackboard visibility, scene/HMI
   diagnostics, artifact workflow) as explicit extensions rather than forcing
   them into an awkward medkit-shaped model.
10. **Execution mode distinction**: Free-space execution, contact-tolerant or
   compliant execution, and intervention execution must be distinguishable in
   APIs, traces, and event history. Never collapse them into one ambiguous
   runtime state.
11. **Authority provenance**: Saved-program execution, delegated recovery, and
    manual direct control must remain distinguishable. An LLM must be able to
    tell what ran, why it ran, who or what was allowed to trigger it, and
    whether the result changed runtime state only or the canonical artifact.
12. **Optional orchestration layers**: External agent runtimes such as
    NemoClaw/OpenClaw may consume ManyForge surfaces, but the core product must
    not depend on them for correctness or basic operability.
13. **Enforceable delegation scope**: If a saved-program node delegates
    temporary authority to an LLM, the allowed action set must be enforced by
    backend/tool authorization, not only by prompt text or operator
    convention.
14. **Model-call auditability**: Any prompt/response pair that can affect robot
    behavior must be traceable with durable correlation metadata, plus an
    explicit redaction/retention policy for sensitive process data.
15. **Node outcome contracts**: Any node type that may trigger recovery,
    delegated intervention, or model-assisted adaptation must define explicit
    success criteria, failure criteria/classes, and the evidence used to
    verify the outcome. LLM logic must not invent success conditions at
    runtime.
16. **Trace all effective LLM actions**: Any externally effective action taken
    by an LLM or agent runtime must be part of the durable observable history:
    model calls, tool invocations, intervention sessions, recovery requests,
    runtime overrides, draft patches, approval/grant use, and post-action
    verification results.

### 2.2 Target Architecture

```
LLM Agent
  |
  |-- REST API (synchronous, polling)
  |     |-- /api/scene/state          [NEW] Full world snapshot (backend truth)
  |     |-- /api/scene/edit           [NEW] Runtime scene mutation
  |     |-- /api/scene/diagnostics    [NEW] Backend vs HMI comparison
  |     |-- /api/kernel/snapshot      [NEW] Execution state + joints
  |     |-- /api/kernel/metrics       [NEW] Performance counters
  |     |-- /api/capabilities         [NEW] Backend capability + contact-tier summary
  |     |-- /api/execution/history    [NEW] Event log for past run
  |     |-- /api/execution/digest     [NEW] High-level summary
  |     |-- /api/execution/blackboard [NEW] Py_trees blackboard state
  |     |-- /api/program/validate     [NEW] Dry-run tree validation
  |     |-- /api/skills               [NEW] Skill catalog with contracts/examples
  |     |-- /api/direct/*             [NEW] Intervention session control
  |     |-- /api/contact-policy       [NEW, after contact-rich execution stream]
  |     |-- /api/scene/snapshot       [NEW] HMI render-to-image (simulation only)
  |     |-- /api/assistant/chat       [NEW, optional design-time assistant]
  |     |-- /api/debug/export         [NEW] Durable trace bundle for post-mortem
  |     |-- /api/program/tree         [EXISTS]
  |     |-- /api/catalog              [EXISTS]
  |     |-- /api/cycle/*              [EXISTS]
  |     |-- /api/debug/scene-trace    [EXISTS]
  |
  |-- MCP / Tool Surface
  |     |-- stdio MCP                 [EXISTS]
  |     |-- /api/mcp                  [NEW, optional HTTP message endpoint]
  |     |-- /api/mcp/sse              [NEW, optional SSE transport]
  |
  |-- WebSocket (optional, for real-time monitoring)
  |     |-- ws://host:9000/ws/state   [EXISTS, extend with kernel events]
  |
  |-- Vision (optional, for real-hardware or visual reasoning)
        |-- camera_vllm_bridge        [EXISTS, external ROS2 package]
        |-- Viser render-to-image     [NEW, for simulated scene]
  |
  |-- ROS2 sidecar (optional when ROS2 is present)
        |-- ros2_medkit-compatible entity/services/topics [TARGET]
        |-- ROS2_MEDKIT_MAPPING.md                          [NEW]
        |-- painless deployment profile                    [NEW]
```

---

## 3. Implementation: New REST Endpoints

### 3.1 `GET /api/scene/state` (P0)

Returns the complete spatial state of the world in a single call.

```json
{
  "robot": {
    "robot_id": "ur5",
    "group_id": "arm",
    "base_frame": "base_link",
    "tcp_frame": "tool0",
    "joint_positions": {
      "shoulder_pan_joint": 0.0,
      "shoulder_lift_joint": -1.57,
      "elbow_joint": 1.57,
      "wrist_1_joint": -1.57,
      "wrist_2_joint": -1.57,
      "wrist_3_joint": 0.0
    },
    "tcp_pose_in_base": {
      "position_m": [0.3, -0.3, 0.25],
      "orientation_xyzw": [0.707, -0.707, 0.0, 0.0]
    }
  },
  "objects": [
    {
      "object_id": "graspable",
      "shape": {"type": "box", "box_dimensions_m": [0.1, 0.01, 0.01]},
      "pose_in_universe": {
        "position_m": [0.3, -0.3, 0.15],
        "orientation_xyzw": [0.0, 0.0, -0.383, 0.924]
      },
      "attached": false,
      "attached_to": null
    },
    {
      "object_id": "ground",
      "shape": {"type": "box", "box_dimensions_m": [1.0, 1.0, 0.2]},
      "pose_in_universe": {
        "position_m": [0.0, 0.0, -0.11],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0]
      },
      "attached": false,
      "attached_to": null
    }
  ],
  "timestamp_ns": 1744200000000000000
}
```

**Implementation**: New route in `routes_debug.py` or new `routes_scene.py`.
Wire through `StateBridge` to query `cycle_manager.get_execution_snapshot()` for
joints, `kernel.get_link_pose("tool0", "base_link")` for TCP, and consume a
backend-authoritative runtime object snapshot for scene objects.

**Key challenge**: The backend doesn't currently maintain an enumerable list of
all scene objects at runtime. The program YAML defines the initial set, but
runtime additions (via `add_collision_object` tree nodes) aren't tracked in a
queryable registry. The kernel's C++ side has this state but it's not exposed.

**Fix**: Add a backend-authoritative scene object snapshot surface to
`KernelClient`, plus `get_joint_names()`:

- `get_scene_objects() -> list[SceneObjectSnapshot]`
- `get_scene_object_ids() -> list[str]` (convenience for diagnostics/UI diffs)
- `get_joint_names() -> list[str]`

Important: IDs alone are **not** sufficient for reliable `GET /api/scene/state`.
The scene route must not reconstruct shapes from the program YAML because that
misses runtime-added objects and can diverge from the actual backend scene. The
native implementation must source shape, attachment/frame, and pose from the
backend-owned runtime scene state.

### 3.2 `GET /api/kernel/snapshot` (P0)

Returns the current execution kernel state.

```json
{
  "state": "executing",
  "execution_id": 42,
  "active_move_id": "pick_approach",
  "progress": 0.65,
  "joint_positions_rad": [0.0, -1.57, 1.57, -1.57, -1.57, 0.0],
  "error": null,
  "last_transition_ns": 1744200000000000000,
  "timestamp_ns": 1744200000100000000
}
```

**Implementation**: Add route calling `cycle_manager.get_execution_snapshot()`
and `cycle_manager.get_status()`, merge into single response.

### 3.3 `GET /api/kernel/metrics` (P0)

Returns performance and diagnostic counters.

```json
{
  "cache_hits": 3,
  "cache_misses": 7,
  "cache_invalidations": 2,
  "plans_succeeded": 7,
  "plans_failed": 0,
  "validations_passed": 7,
  "validations_failed": 0,
  "executions_completed": 5,
  "executions_failed": 0,
  "stops_issued": 0,
  "collision_detections": 0,
  "snapshots_published": 1250
}
```

**Implementation**: `NativeKernelClient.get_metrics()` already exists.
Add a REST route. For `MockKernelClient`, return zeroes or accumulated counts.

### 3.4 `POST /api/program/validate` (P1)

Dry-run validation of a tree without loading it.

```json
// Request
{"tree": {"id": "sequence", "name": "root", "children": [...]}}

// Response (success)
{"valid": true, "warnings": []}

// Response (failure)
{"valid": false, "errors": [
  {"path": "root.children[2]", "message": "Unknown catalog entry: 'nonexistent_node'"},
  {"path": "root.children[0].params.velocity_scale", "message": "Must be > 0.0, got -1.0"}
]}
```

**Implementation**: The `catalog.validate_tree()` path already exists inside
`Program.from_dict()`. Extract it into a standalone function, wire to a route.

### 3.5 `GET /api/execution/history` (P1)

Query the event log from the most recent (or current) execution run.

```json
{
  "execution_id": 42,
  "events": [
    {"timestamp_ns": ..., "type": "plan_started", "move_id": "pick_approach", "details": {}},
    {"timestamp_ns": ..., "type": "plan_completed", "move_id": "pick_approach", "details": {"duration_ms": 234, "waypoints": 47}},
    {"timestamp_ns": ..., "type": "execute_started", "move_id": "pick_approach", "details": {}},
    {"timestamp_ns": ..., "type": "execute_completed", "move_id": "pick_approach", "details": {"duration_ms": 3200}}
  ],
  "node_status_transitions": [
    {"timestamp_ns": ..., "node": "move_to_pick_approach", "from": "RUNNING", "to": "SUCCESS"}
  ]
}
```

**Implementation**: Add an `EventRecorder` (bounded deque) subscribed to kernel
events and py_trees tick statuses. The recorder lives in `StateBridge` or
`CycleManager`. New route serves it.

### 3.6 `GET /api/execution/digest` (P1)

High-level summary designed for LLM polling (call every 2-5 seconds).

```json
{
  "state": "executing",
  "current_node": "move_to_pick_approach",
  "current_node_status": "RUNNING",
  "active_move_id": "pick_approach",
  "progress": 0.65,
  "elapsed_s": 12.3,
  "velocity_scale": 0.5,
  "completed_nodes": ["setup_scene"],
  "remaining_nodes": ["move_to_pick", "close_gripper", "attach_graspable", ...],
  "object_summary": [
    {"id": "graspable", "attached": false, "position_m": [0.3, -0.3, 0.15]}
  ],
  "error": null,
  "warnings": []
}
```

**Implementation**: Aggregation of existing data: `cycle_manager.state`,
`get_node_statuses()`, `get_execution_snapshot()`, object queries. Compute
completed/remaining from tree structure + node statuses.

### 3.7 `GET /api/scene/snapshot` (P2)

Render the Viser 3D viewport as an image.

```
GET /api/scene/snapshot?width=640&height=480&format=jpeg
Content-Type: image/jpeg

<binary JPEG data>
```

**Implementation**: Viser's `ViserServer` supports `client.get_render(height, width)`
which returns an RGB numpy array. Add a route that:
1. Picks the first connected Viser client (or a headless one)
2. Calls `get_render()`
3. Encodes to JPEG/PNG via Pillow
4. Returns as binary response

**Caveat**: Requires at least one Viser client connection. For headless operation,
may need a virtual client or Viser's offscreen rendering (if supported).

**Note**: This renders the HMI/simulation view, NOT a real-world camera image.
For physical-world perception, use `camera_vllm_bridge` (Section 5.2).

### 3.8 `POST /api/scene/edit` (P0)

Runtime scene mutation — add, remove, or update objects without modifying the
behavior tree.

```json
// Request
{
  "operation": "add",
  "object_id": "new_obstacle",
  "shape_type": "box",
  "box_dimensions_m": [0.2, 0.2, 0.5],
  "pose_position_m": [0.5, 0.0, 0.25],
  "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
  "frame_id": "universe"
}

// Response
{"success": true, "error": null}
```

Supported operations: `add`, `remove`, `update_pose`, `attach`, `detach`.

**Execution state policy (v0 — external mutations only)**:
- **IDLE**: All operations allowed
- **RUNNING**: All external operations rejected with 409 Conflict. Authored
  in-tree scene mutations (behavior tree nodes like `add_collision_object`,
  `attach_object_to_link`) continue to execute within the kernel's own
  concurrency model. External REST edits are a separate concern.
- **STOPPED**: All operations allowed (kernel is quiescent)

Note: This distinction between external (REST API) and internal (tree node)
mutations is fundamental. Future versions may relax the RUNNING restriction
with a dedicated synchronization channel for perception-driven updates.
Clarification: this `STOPPED` allowance applies to **scene object mutations**
only. It does not imply that other runtime-owned resources, such as
contact-policy overrides, are mutable in `STOPPED`.

**Implementation**: Thin wrapper over existing `KernelClient.scene_edit()` which
accepts `SceneEditRequest`. Add state-gate logic checking `cycle_manager.state`.

### 3.9 `GET /api/scene/diagnostics` (P1)

Compare backend scene truth vs HMI render truth. Returns per-object mismatch
summary.

```json
{
  "objects": [
    {
      "object_id": "graspable",
      "backend": {"exists": true, "attached": false, "frame_id": "universe",
                  "pose_m": [0.3, -0.3, 0.15]},
      "hmi": {"rendered": true, "scene_name": "/scene/graspable",
              "render_state": "synced"},
      "match": true
    },
    {
      "object_id": "obstacle_2",
      "backend": {"exists": true, "attached": false, "frame_id": "universe",
                  "pose_m": [0.5, 0.0, 0.25]},
      "hmi": {"rendered": false, "scene_name": null, "render_state": "missing"},
      "match": false,
      "mismatch_reason": "Object exists in backend but not rendered in HMI"
    }
  ],
  "summary": {"total": 2, "matched": 1, "mismatched": 1},
  "timestamp_ns": 1744200000000000000
}
```

**Implementation**: Wire `ViserHmiServer.get_object_render_diagnostics()` for
each object returned by `get_scene_object_ids()`. Compare backend state with
HMI render state.

### 3.10 `GET /api/debug/export` (P1)

Single-call durable trace bundle for LLM post-mortem debugging.

```json
{
  "exported_at_ns": 1744200000000000000,
  "run_id": "run-2026-04-09-001",
  "program": {"name": "pick_and_place_demo", "source_path": "/path/to/program.yaml"},
  "deployment": {"name": "ur5_roboplan_local", "source_path": "/path/to/deployment.yaml"},
  "kernel_snapshot": { ... },
  "kernel_metrics": { ... },
  "execution_digest": { ... },
  "execution_history": [ ... ],
  "scene_state": { ... },
  "scene_diagnostics": { ... },
  "scene_trace": [ ... ],
  "blackboard": { ... },
  "node_statuses": { ... }
}
```

**Implementation**: Calls each of the above endpoints internally and merges
into a single JSON response. All sub-objects carry the same `run_id` and
`execution_id` for correlation.

### 3.11 `GET /api/execution/blackboard` (P1)

Expose py_trees blackboard key-value state for debugging.

```json
{
  "keys": {
    "gripper_closed": {
      "value_json": true,
      "type": "bool"
    },
    "tool0_pose": {
      "value_json": {"position_m": [0.3, -0.3, 0.2], "orientation_xyzw": [0.707, -0.707, 0, 0]},
      "type": "dict"
    },
    "pinocchio_model": {
      "value_json": null,
      "value_repr": "<pinocchio.Model object at 0x7f...>",
      "type": "pinocchio.Model",
      "truncated": true
    }
  },
  "timestamp_ns": 1744200000000000000
}
```

**Note**: This is a **JSON-safe filtered snapshot**, not a raw Python dump.
Each key includes `value_json` (when serializable), `value_repr` (safe string
fallback), `type`, and optionally `truncated`. Non-serializable Python objects
(numpy arrays, Pinocchio models, custom dataclasses) get `value_repr` only.

**Implementation**: `py_trees.blackboard.Blackboard` provides read access to
all keys. Add a method on `CycleManager` to snapshot the blackboard with
JSON-safe serialization (try `json.dumps`, fall back to `repr()`), route
through `StateBridge`.

### 3.12 `GET /api/capabilities` (P3)

Return the capabilities currently available to the loaded deployment so an LLM
can validate skills and contact-aware actions before composing them.

```json
{
  "planning": ["planning.motion.joint_goal", "planning.motion.pose_goal"],
  "execution": [
    "execution.trajectory.execute",
    "execution.contact.geometric"
  ],
  "sensing": ["sensing.joint_state"],
  "contact_tier": "geometry_only",
  "timestamp_ns": 1744200000000000000
}
```

**Implementation**: Populate from the loaded deployment artifact in Phase 3.
If dynamic backend self-reporting becomes available later, it can be layered on
without changing the route contract. A dedicated route keeps skill
compatibility checks cheap and explicit for agents.

### 3.13 `GET /api/skills` (P3)

Expose the skill catalog as machine-readable data so smaller models can compose
programs from validated building blocks instead of wiring raw nodes directly.

```json
{
  "skills": [
    {
      "id": "pick_object",
      "version": "0.1.0",
      "compatible_profiles": ["v0.0"],
      "requires": ["object exists in scene", "gripper open"],
      "provides": ["object attached to tcp"],
      "modifies": ["scene object attachment", "trajectory cache"],
      "requires_capabilities": ["planning.motion.pose_goal"],
      "example": {"object_id": "graspable", "approach_height_m": 0.05}
    }
  ],
  "timestamp_ns": 1744200000000000000
}
```

**Implementation**: Load separate YAML skill artifacts at startup and serve
them through a dedicated route. Each skill references catalog node IDs rather
than redefining primitive semantics.

### 3.14 `POST /api/direct/execute` / `GET /api/direct/status` / `POST /api/direct/abort` / `POST /api/direct/heartbeat` (P3)

Execute a temporary intervention subtree without modifying the saved program.
This is the preferred first implementation of direct command mode; raw
imperative direct commands are deferred.

```json
// Request
{
  "tree": {
    "id": "place_object",
    "name": "recover_stuck_part",
    "params": {"object_id": "stuck_part"}
  },
  "leaseTimeoutMs": 60000,
  "resumeTarget": "resume"
}
```

**Behavior**:
- Intervention can start from `IDLE` or `STOPPED`, but `IDLE -> DIRECT_CONTROL`
  requires explicit human approval by default unless a saved-program node or
  policy has explicitly delegated that authority.
- Each intervention gets a `session_id`, `session_kind`
  (`manual_intervention` | `delegated_recovery`), `authority_source`
  (`human` | `llm` | `program_node` | `policy`), concrete `principal_id`,
  grant provenance (`human_approved` | `policy_granted`), `parent_run_id`
  (when interrupting an active run), `entry_state`, `resume_target`,
  `trigger_reason`, `trigger_node_instance`, `actual_outcome`, `events[]`, and
  a lease/watchdog.
- Intervention subtrees have full blackboard read access, scoped write access
  under `_intervention/<session_id>/...`, and explicit promotion on
  completion.
- Scene edits during intervention are allowed only within that intervention
  session and must be fully audited.
- Lease expiry transitions the system to `STOPPED`; it must never auto-resume
  execution.
- Direct control and saved-program execution must remain separate in every
  history/export surface.
- Runtime overrides discovered or produced during direct control must remain
  distinguishable from persisted program edits.

**Implementation**: Extend `CycleManager` with `STOPPED` and `DIRECT_CONTROL`
state handling, session tracking, authority provenance, and wrapper-level event
attribution. The first implementation validates intervention trees against the
same catalog/skill/profile/capability rules as the saved-program path. A
follow-on authority/adaptation stream must add delegated-recovery contracts,
direct-control approval gating, node-level editability policy enforcement, and
runtime-override tracking. If wrapper-side attribution proves lossy, a later
kernel extension will propagate session context natively.

### 3.15 `GET /api/contact-policy` / `PUT /api/contact-policy` (Phase 3+)

Reserved for the contact-rich execution stream. These endpoints expose baseline
and named contact policies once the kernel supports zone-aware contact
execution. Runtime overrides are allowed only in `IDLE` and `DIRECT_CONTROL`.

### 3.16 `POST /api/assistant/chat` (Phase 6, optional)

Optional design-time assistant surface for the Composer UI. The assistant
receives user messages, current program/deployment/capability context, and
returns explanatory text plus optional structured actions such as tree
proposals or validation requests.

**Rules**:
- design-time only; the endpoint proposes edits but does not silently apply
  them,
- any returned tree proposal must still go through normal validation and
  artifact workflow,
- the assistant may call live state readers, but any runtime-affecting action
  still goes through the normal `/api/program/*` or `/api/cycle/*` contracts.

### 3.17 `GET /api/mcp/sse` + `POST /api/mcp` (Phase 5+, optional)

Optional HTTP/SSE transport for the existing MCP tool surface. This is needed
when ManyForge tools must be consumed from sandboxed external runtimes such as
NemoClaw/OpenClaw without launching ManyForge inside the sandbox itself.

If Phase 6 external orchestration is in scope, this transport stops being a
nice-to-have and becomes a practical prerequisite for production integration.
REST-only fallback remains acceptable for prototyping, but HTTP/SSE MCP should
be treated as the target transport for stable external-agent operation.

**Rules**:
- the tool contract remains the same as the stdio MCP surface,
- authentication/authorization must match the HTTP backend security model,
- bounded payload and polling/SSE semantics remain preferred over opaque,
  long-lived bidirectional sockets.

---

## 4. Implementation: Extending Existing Interfaces

### 4.1 WebSocket: Add Kernel Events (P2)

Extend the `ws://host:9000/ws/state` broadcast to optionally include kernel events.

```json
{
  "state": "running",
  "velocityScale": 0.5,
  "nodeStatuses": {...},
  "error": null,
  "programLoaded": true,
  "timestamp": 1744200000.0,
  "kernelEvents": [
    {"type": "plan_completed", "move_id": "pick_approach", "timestamp_ns": ..., "details": {"duration_ms": 234}}
  ]
}
```

The `kernelEvents` array contains events that occurred since the last broadcast
(100ms window). Empty array when idle. This avoids a separate WS endpoint while
keeping backward compatibility (new field, existing clients ignore it).

### 4.2 KernelClient: Scene Object Snapshot + Enumeration (P0, prerequisite)

Add to the `KernelClient` abstract interface:

```python
@abstractmethod
def get_scene_objects(self) -> list[SceneObjectSnapshot]:
    """Return backend-authoritative scene objects with shape, frame, and world pose."""

@abstractmethod
def get_scene_object_ids(self) -> list[str]:
    """Return IDs of all collision objects currently in the scene."""

@abstractmethod
def get_joint_names(self) -> list[str]:
    """Return ordered joint names for the active robot group."""
```

These are adapter/HMI observability surfaces. They MUST be backed by the
backend-owned runtime scene state, not by a parallel Python/HMI cache.
`MockKernelClient` tracks them in its internal dict; the native implementation
must expose them through the pybind layer sourced from the RoboPlan planning
context's runtime object registry.

### 4.3 ros2_medkit Compatibility Mapping (P3)

If ManyForge is deployed with ROS2, Phase 4 must also produce a compatibility
artifact that maps ManyForge surfaces to ros2_medkit capabilities on a
one-to-one basis wherever applicable.

**Required deliverable**:
- `ROS2_MEDKIT_MAPPING.md`

The mapping document should classify each relevant capability as:
- **native match**: ManyForge already exposes the same concept cleanly
- **adapted match**: a thin sidecar translation is needed
- **ManyForge extension**: not represented by ros2_medkit; retained as a
  ManyForge-specific API or tool
- **not applicable**: ros2_medkit feature not relevant to ManyForge

Minimum mapping areas:
- entity hierarchy and component health
- lifecycle state and lifecycle transitions
- event/history streaming
- SSE subscriptions (ros2_medkit exposes ROS2 events via SSE; evaluate as
  an LLM-friendly alternative to ManyForge's WebSocket broadcast, since
  LLM agents work better with polling/SSE than persistent connections)
- bulk data transfer (ros2_medkit supports large diagnostics payloads;
  map to ManyForge's debug/export bundle and JSONL trace sink)
- durable recording / replay surfaces
- fault reporting / diagnostics
- contact events and contact-policy diagnostics
- intervention sessions and recovery/debug workflows
- RBAC / operator roles where applicable
- service/action/control surfaces exposed through the ROS2 sidecar

---

## 5. Vision Integration Strategy

### 5.1 Simulated Scene (Viser Render-to-Image)

For simulated environments, the 3D Viser viewport is the "camera".
`GET /api/scene/snapshot` (Section 3.7) provides this.

An LLM agent can:
1. Poll `/api/scene/state` for structured spatial data
2. Poll `/api/scene/snapshot` for a rendered image when visual confirmation needed
3. Send the image to a VLM (Qwen3.5-2B via local vLLM) for natural language scene description

### 5.2 Real Hardware (camera_vllm_bridge)

`camera_vllm_bridge` is a ROS2 package that:
- Subscribes to `/camera/color/image_raw` (RealSense, Isaac Sim, etc.)
- Sends frames to a local vLLM instance via OpenAI-compatible API
- Exposes `/analyze_image` service and `/analyze_image_action` action

**Integration options** (ManyForge is ROS-free):

**Option A: HTTP bridge** (recommended for v0)
- Add a thin FastAPI wrapper around `camera_vllm_bridge` functionality
- Endpoint: `GET /api/vision/analyze?prompt=<text>`
- Captures latest frame, sends to VLM, returns analysis text
- Can run as a sidecar container with access to the same vLLM instance

**Option B: Pure Python port** (for later)
- Extract the core logic (frame capture + VLM query) from `camera_vllm_bridge`
- No ROS2 dependency; camera source is V4L2 or Viser render
- Integrate directly into ManyForge composer as an optional module

### 5.3 Dual-Model Vision Architecture

For an LLM agent supervising a robot cell:

```
Supervisory LLM (122B, slow, smart)
  |
  |-- Polls /api/scene/state (structured, fast)
  |-- Polls /api/execution/digest (structured, fast)
  |-- Occasionally requests /api/vision/analyze (routed to 2B VLM)
  |
  |   Vision VLM (2B, fast, specialized)
  |     |-- Processes camera frames on demand
  |     |-- Returns natural language scene description
  |     |-- Flags anomalies: "object has fallen", "robot is near collision"
  |
  |-- Decides: continue, stop, modify tree, alert human
```

The 2B VLM acts as the "eyes" — cheap to run, fast enough for near-real-time
visual feedback. The supervisory LLM reasons about the structured + visual data
combined.

---

## 6. Skill Catalog + LLM Composition Guide

### 6.1 Skill Artifacts (`skills/*.yaml`)

Skills are separate YAML artifacts that expand to validated subtrees composed of
portable catalog primitives. They are not a replacement for the node catalog;
they are a higher-level layer optimized for reliability and smaller models.

Each skill definition should include:
- `skill_id`, `version`, `compatible_profiles`
- `body` (catalog node references and parameter wiring)
- `requires` (scene/robot preconditions)
- `provides` (postconditions)
- `modifies` (blackboard, scene, cache, attachments, etc.)
- `requires_capabilities`
- `example` (canonical, executable example tied to a fixture or test)

Skill rules:
- skills may reference other skills, but nesting must be acyclic and limited to
  max depth 2 initially
- composition constraints are hard for safety/structure and soft for style
- `GET /api/skills` is the discovery surface; `GET /api/catalog` cross-references
  skills via a `compound_skills` section

### 6.2 File: `LLM_GUIDE.md` (to be created)

A machine-readable guide explaining how to compose behavior trees and when to
prefer skills over raw node composition. Covers common patterns the catalog
alone doesn't teach:

**Pattern: Pick an object**
```yaml
sequence:
  - move_manipulator_action (relative_move to object + z offset)  # approach
  - move_manipulator_action (relative_move to object)              # move down
  - set_key_bool_value (gripper_closed = true)                     # close gripper
  - attach_object_to_link (object_id, tool0)                       # attach to TCP
  - move_manipulator_action (relative_move from current + z)       # retreat up
  - reset_trajectories                                             # invalidate cache
```

**Pattern: Place an object**
```yaml
sequence:
  - move_manipulator_action (pose_goal to drop approach)
  - move_manipulator_action (pose_goal to drop)
  - set_key_bool_value (gripper_closed = false)
  - detach_object_from_link (object_id, tool0)
  - move_manipulator_action (relative_move retreat up)
  - reset_trajectories
```

**Pattern: Conditional retry with fallback**
```yaml
fallback:
  - sequence:
      - check_key_bool_value (sensor_ready = true)
      - move_manipulator_action (...)
  - retry (num_attempts=3):
      - sequence:
          - timer (duration_s=1.0)
          - move_manipulator_action (alternative approach)
```

**Rules**:
- Always `reset_trajectories` after any scene change (attach/detach/add/remove)
- Always approach before moving to a tight pose (collision avoidance)
- `relative_move` with `target_reference.type=object` resolves the object's
  current pose at planning time — use this for dynamic scenes
- `velocity_scale` is parameterizable with `${param:name}` substitution
- Joint goals are for known configurations (home, rest); pose goals for
  Cartesian targets; relative moves for object-relative or TCP-relative motions

**Drift prevention**: The guide must be anchored in executable sources. Each
pattern should reference the specific example file or integration test that
validates it. When an example is updated, a CI check should verify the guide
still matches. If generating the guide from annotated examples is feasible,
prefer that over hand-maintaining prose.

### 6.3 Catalog Annotations (future)

Add `usage_patterns` field to catalog entries linking related nodes:
```yaml
- id: attach_object_to_link
  usage_patterns:
    typical_sequence: [move_to_object, close_gripper, attach_object_to_link, retreat, reset_trajectories]
    prerequisites: [object must exist in scene, robot must be near object]
    post_conditions: [object follows TCP, old trajectories invalid]
```

---

## 7. Implementation Roadmap

### Phase 1: Core Observability + Scene Mutation (P0)

**Scope**: Scene state API, scene mutation API, kernel snapshot/metrics, object
enumeration.
**Files touched**:
- `manyforge_behavior/kernel_client.py` (add `get_scene_objects`, `get_scene_object_ids`, `get_joint_names`)
- `manyforge_roboplan/include/manyforge_roboplan/roboplan_planning_context.hpp` (runtime object snapshot methods)
- `manyforge_roboplan/src/roboplan_planning_context.cpp` (authoritative object registry exposure)
- `manyforge_roboplan/python/bindings.cpp` (pybind export for scene/joint snapshot methods)
- `manyforge_roboplan/python/native_kernel_client.py` (implement above)
- `manyforge_composer/backend/models.py` (typed request/response contracts)
- `manyforge_composer/backend/routes_scene.py` (new: `/api/scene/state`, `/api/scene/edit`)
- `manyforge_composer/backend/routes_kernel.py` (new: `/api/kernel/snapshot`, `/api/kernel/metrics`)
- `manyforge_composer/tests/test_observability_routes.py` (container-backed composer API tests)
- `manyforge_roboplan/test/test_native_e2e.py` (native binding/runtime assertions)

**Estimated agent work**: 2 agents (1 Python backend, 1 C++ bindings)

**Validation requirement**: Phase 1 is not done until the following pass in the
same Docker/dev image that ships the backend surface:

- `docker compose run --rm dev bash -lc 'cd /workspace && pytest manyforge_composer/tests/test_observability_routes.py -q'`
- `docker compose run --rm dev bash -lc 'cd /workspace && ./scripts/test-all.sh roboplan'`
- `docker compose run --rm dev bash -lc 'cd /workspace && pytest manyforge_composer/tests manyforge_behavior/tests tests/test_pick_and_place.py tests/test_integration.py -q'`

This is mandatory because host environments may not include FastAPI/httpx or a
compatible native RoboPlan toolchain, while the industrial deployment surface is
defined by the containerized environment.

**Scene-contract rule**: backend scene-edit APIs remain strict and
backend-authoritative. `add` of an existing object and `remove` /
`update_pose` of a missing object are domain failures at the backend/API
boundary. The orchestration layer must therefore not depend on duplicate
application of static scene resources. Program `scene:` resources are applied by
the runner/CycleManager before execution, and tree-authored scene nodes are for
runtime edits only.

### Phase 2: Validation + History + Diagnostics (P1)

**Status**: Implemented on the current branch and container-validated. The
remaining work in this phase is documentation hygiene and keeping the new
surfaces coherent as later phases land.

**Scope**: Tree validation endpoint, execution event recorder, digest endpoint,
blackboard API, scene diagnostics, trace export bundle, local JSONL trace sink.
**Files touched**:
- `manyforge_behavior/catalog.py` (extract `validate_tree()` standalone)
- `manyforge_composer/backend/routes_program.py` (add `/api/program/validate`)
- `manyforge_composer/backend/event_recorder.py` (new: in-memory ring buffer + JSONL file sink)
- `manyforge_composer/backend/routes_execution.py` (new: `/api/execution/history`, `/api/execution/digest`, `/api/execution/blackboard`)
- `manyforge_composer/backend/routes_scene.py` (add `/api/scene/diagnostics`)
- `manyforge_composer/backend/routes_debug.py` (add `/api/debug/export`)
- `manyforge_composer/backend/ws_state.py` (extend broadcast with kernel events)

**JSONL trace sink**: The event recorder writes events to both the in-memory
ring buffer (for live queries) and a `{run_id}.events.jsonl` file (for durable
storage). `GET /api/debug/export` reads from the JSONL sink when available,
falling back to the in-memory buffer. This provides lightweight persistence
without requiring ROS2/rosbag.

**Estimated agent work**: 2-3 agents (1 backend routes, 1 event recorder + digest, 1 diagnostics + export)

### Phase 3: Vision + Skill Catalog + Intervention Mode (P2)

**Status**: Complete. Capabilities, skill catalog, composition guide,
intervention session support, scene snapshot, MCP server, and security
hardening have all landed.

Clarification: this status applies to the original Phase-3 intervention scope.
The later authority/adaptation expansion defined in Section 11 remains follow-on
work; it is not covered by the original “complete” claim.

**Scope**: Viser render-to-image, camera_vllm_bridge integration,
capabilities discovery, skill catalog, composition guide, and intervention
session support via temporary subtree execution.
**Files touched**:
- `manyforge_hmi/server.py` (add render-to-image method)
- `manyforge_composer/backend/routes_vision.py` (new: `/api/scene/snapshot`, `/api/vision/analyze`)
- `manyforge_composer/backend/routes_skills.py` (new: `/api/skills`, `/api/capabilities`)
- `manyforge_composer/backend/routes_direct.py` (new: intervention session routes)
- `manyforge_composer/backend/event_recorder.py` (extend with intervention-session segmentation)
- `manyforge_behavior/skills/` (new: skill YAML artifacts)
- `manyforge_behavior/skill_loader.py` (new: load/validate skills)
- `manyforge_behavior/runner.py` (skill expansion support)
- `manyforge_behavior/cycle_manager.py` (STOPPED, DIRECT_CONTROL, session tracking)
- `LLM_GUIDE.md` (new: composition patterns and skill usage guide)

**Implementation notes**:
- `GET /api/capabilities` reports planning/execution/sensing/contact-tier data
  from the loaded deployment artifact; dynamic backend self-reporting can be
  added later without changing the public route.
- Intervention mode uses wrapper/event-recorder attribution first.
- If session attribution is not sufficiently robust or lossless, a planned
  follow-on kernel extension will propagate caller/session context into kernel
  events.
- Raw imperative direct commands are deferred; temporary subtrees are the first
  supported intervention surface.

**Estimated agent work**: 2-3 agents + manual documentation

### Phase 4: Kernel Event Stream + ROS2 Observability Sidecar (P3)

**Scope**: Bridge C++ kernel events to Python, include in WS broadcast.
Optionally, publish events to a ROS2 sidecar for rosbag recording, ros2_medkit
diagnostics, and camera_vllm_bridge integration. If ROS2 is used at all, this
phase must also deliver a 1:1 ros2_medkit compatibility mapping for applicable
features and a low-friction deployment path for the sidecar.

**Files touched (core)**:
- `manyforge_bindings/` (expose event subscription to Python)
- `manyforge_behavior/cycle_manager.py` (subscribe to kernel events)
- `manyforge_composer/backend/ws_state.py` (include events in broadcast)
- optional kernel DTO/event metadata propagation for native intervention session
  attribution if wrapper-side attribution proves insufficient

**Files touched (optional ROS2 sidecar)**:
- `manyforge_ros2_bridge/` (new package: ZMQ subscriber → ROS2 topic publisher)
- `manyforge_ros2_bridge/msg/` (custom ROS2 message types for kernel events,
  execution snapshots, scene state)
- `manyforge_ros2_bridge/launch/` (launch file for sidecar + rosbag recorder)
- `ROS2_MEDKIT_MAPPING.md` (new: capability mapping and semantic notes)
- sidecar deployment profile / compose or launch assets (new: painless startup)
- Integration with `ros2_medkit` for SOVD entity hierarchy, fault management,
  RBAC, and SSE streams
- Integration with `camera_vllm_bridge` for vision (runs natively in the
  ROS2 sidecar environment)

**ROS2 sidecar architecture** (see `LLM_OPERABILITY_DISCUSSION.md` Section 2.8):

ManyForge stays ROS-free. The sidecar is an optional container that subscribes
to ManyForge's internal event stream (via ZMQ or shared memory) and republishes
as ROS2 topics. This enables:
- **rosbag2**: Persistent, replayable recording of all execution data
- **ros2_medkit**: REST+SSE diagnostics gateway with SOVD hierarchy and RBAC
- **Lifecycle management**: Standardized component state machine
- **camera_vllm_bridge**: Native ROS2 camera integration for visual perception
- **Isaac Sim pathway**: Future simulation backend using Isaac Sim's ROS2 bridge

**Compatibility requirement when ROS2 is present**:
- the sidecar must provide a documented one-to-one mapping to ros2_medkit
  capabilities wherever semantically applicable,
- ManyForge-specific capabilities that do not fit ros2_medkit cleanly remain
  explicit extensions and must be documented as such,
- deploying ros2_medkit alongside the sidecar should be a low-friction,
  documented path rather than an ad hoc integration exercise.

**Painless deployment target**:
- one documented startup flow for ManyForge + sidecar + ros2_medkit
- default wiring for rosbag2 recording when enabled
- natural co-location path for `camera_vllm_bridge` when vision is enabled
- health checks sufficient for an LLM or operator to determine whether the ROS2
  observability stack is up and correctly connected

**Estimated agent work**: 2-3 agents (1 C++/Python event bridge, 1 ROS2 sidecar
package, 1 ros2_medkit integration + deployment/mapping work)

### Parallel Design Stream: Contact-Rich Execution Contracts

This stream runs alongside the operability phases and is not a prerequisite for
P0-P2, but it is required for reliable bin-picking and other contact-rich tasks.

**Scope**:
- New `250-contact-policy` spec (or equivalent spec extension)
- Per-zone contact policy model with body-pair matching
- `sensing_mode` in policy schema (`geometry_only | force_torque | controller_reported`)
- Deployment baseline + program override + runtime override model
  (`IDLE` and `DIRECT_CONTROL` only for runtime override)
- Backend capability tiers for contact-rich execution
- Operability surfaces for contact policies, contact events, and capability tiers

**Kernel-level changes explicitly in scope**:
- `CollisionCheckResultDTO` extended with `penetration_depth_m` and optional
  `estimated_force_n`
- `CollisionPolicy` rewritten as zone-aware with per-body-pair responses
- `collision_check_tick()` extended to use per-move contact policy
- `ExecuteRequestDTO` extended with optional `contact_policy_id`
- `pre_execute_check()` made zone-aware
- backend contract change: `IPlanningContext::collision_check()` returns
  penetration depth

**Backend capability tiers**:
- Tier 1: geometry-only contact reasoning
- Tier 2: force-backed contact execution
- Tier 3: controller-reported contact execution

**Q3 design spike (required before full implementation)**:
Measure whether the backend can compute penetration depth for relevant body
pairs at the intended monitoring cadence and scene complexity envelope.

Representative scenarios:
- free-space baseline with no expected contact
- expected finger/object contact during grasp
- unexpected arm/object or arm/environment contact
- attached-object contact case
- denser bin-like scene with multiple nearby collision bodies

Required deliverable:
- a short benchmark/design note capturing measured latency, variance, scene
  complexity envelope, result quality observations, and a go/no-go
  recommendation for Tier 1 `geometry_only` contact reasoning

Normative follow-through:
- the spike result must be folded back into `250-contact-policy` and
  `240-supported-backend-capability-profile.md` before full contact-rich
  implementation begins
- if the result is negative or materially constrained, the implementation plan
  must be narrowed accordingly rather than assuming Tier 1 viability

Acceptance criteria:
1. Can penetration depth be computed for relevant body pairs at the configured
   collision monitoring cadence?
2. Under what scene complexity envelope?
3. What is the latency budget and variance?
4. Is result quality sufficient for Tier 1 geometry-only contact reasoning?

### Phase 5: LLM-Native Operability Facade (P3)

**Scope**: MCP server or equivalent SDK wrapping the REST API surface into a
high-level tool interface optimized for LLM agent consumption. If external
agent runtimes such as NemoClaw/OpenClaw are in scope, this phase also grows
an HTTP/SSE transport so those agents can consume the same MCP contract
without embedding ManyForge in-process.

**Schema contract**: The MCP tool schema should live in a separate
`MCP_SCHEMA.md` (or equivalent interface contract document), not in this plan.
This plan tracks the tool families and sequencing; the schema document owns the
concrete tool names, input/output payloads, and compatibility rules.

**Tools exposed**:
- `manyforge_scene_state` — composite scene query
- `manyforge_scene_edit` — runtime scene mutation
- `manyforge_program_load` — load program + deployment
- `manyforge_program_tree` — read/write behavior tree
- `manyforge_program_validate` — dry-run validation
- `manyforge_cycle_control` — run/stop/reset/velocity
- `manyforge_capabilities` — deployment/backend capability discovery
- `manyforge_skills` — skill catalog and contracts
- `manyforge_intervention` — intervention session execution/status
- `manyforge_execution_status` — digest + blackboard
- `manyforge_debug_export` — full trace bundle
- `manyforge_catalog` — node type introspection

**Design approach**: Define the MCP tool schemas during Phase 1-2 work in the
separate `MCP_SCHEMA.md` contract so the REST endpoint designs remain
MCP-compatible from the start (consistent naming, bounded payloads, no
streaming-only patterns). Implement the MCP server in Phase 5.

**External-agent transport requirement when targeted**:
- add `GET /api/mcp/sse` + `POST /api/mcp` or an equivalent HTTP transport
  carrying the same tool schema,
- keep stdio MCP as the local/in-process default,
- validate end-to-end use from a sandboxed external agent runtime rather than
  only local process tests.

**Estimated work**: 1 agent + manual schema design

### Phase 6: Model-In-The-Loop + External Orchestration (P4, optional)

**Scope**: Runtime model-query nodes, explicit delegated-recovery integration,
Composer assistant chat, and optional NemoClaw/OpenClaw orchestration on top of
the existing ManyForge REST/MCP surfaces.

OpenClaw-specific topology, sandbox, model-allocation, and operator-mode
details live in `OPENCLAW_INTEGRATION_PLAN.md`. This plan keeps only the
ManyForge product changes required to support that integration cleanly.

This phase is optional because ManyForge must remain independently operable
without OpenClaw. It becomes mandatory only if the product target includes:
- sandboxed autonomous operator loops,
- design-time Composer chat assistance,
- per-cycle VLM/LLM target selection inside the behavior tree, or
- delegated recovery via an external agent runtime.

**Files touched (ManyForge core)**:
- `manyforge_behavior/nodes/integration.py` (new: `query_model`, later `delegate_to_llm`)
- `manyforge_behavior/resources/node_catalog.yaml` (catalog entries + contracts)
- `manyforge_behavior/cycle_manager.py` (delegation/session enforcement)
- `manyforge_composer/backend/routes_assistant.py` (new: `/api/assistant/chat`)
- `manyforge_composer/backend/routes_mcp_transport.py` or equivalent
  (new: `/api/mcp`, `/api/mcp/sse`)
- `manyforge_composer/backend/event_recorder.py` (model/delegation/supervisor events)
- `manyforge_composer/frontend/src/...` (assistant panel + runtime override UX)
- deployment/docs for model endpoint configuration and audit policy

**Files touched (external integration assets)**:
- OpenClaw/NemoClaw policy/configuration assets
- sandbox-domain knowledge docs / prompts / skills
- end-to-end integration tests or scripted validation flows

**Implementation notes**:
- `query_model` is a regular tree node, not an intervention session; it writes
  structured outputs to the blackboard and must have explicit timeout,
  confidence, and parsing semantics.
- `query_model` should follow a concrete contract shape rather than a vague
  “call a model” abstraction. Minimum expected fields are:
  - deployment-configured `model_endpoint` alias, not a raw URL,
  - `prompt_template_id` referencing a versioned/hashable deployment-side
    template, not inline free-form prompt text,
  - prompt-template variables for blackboard and scene context,
  - optional scene-state / snapshot attachment with stable payload references
    or hashes in the trace,
  - JSON-schema output validation as the baseline parsing mode,
  - output-key prefix / blackboard write namespace,
  - minimum confidence threshold,
  - wall-clock timeout / cancellation behavior, and
  - atomic namespace writes (all validated outputs applied together or no
    blackboard writes at all).
- deterministic recovery should be preferred over model-assisted recovery in
  the baseline product. The first in-tree recovery shapes should therefore be
  typed, bounded recovery nodes such as retry / fallback-override /
  recovery-skill patterns rather than a generic high-authority
  `delegate_to_llm` node.
- the currently implemented delegated-recovery path is a stop-and-request
  policy hook. A program node can raise a pending recovery request, but true
  in-tree model-assisted recovery remains future work in this phase.
- recovery-capable nodes should carry explicit node-level contracts:
  success/failure criteria, verification evidence, and bounded recovery policy
  metadata describing whether LLM intervention is allowed on failure, which
  skills/actions are permitted, and the maximum persistence level of any
  resulting change.
- prompt-only tool restrictions are not sufficient; delegated scopes must be
  enforced by the ManyForge backend/tool layer.
- prompt/response logging must follow an explicit redaction and retention
  policy before industrial deployment.
- If external operator escalation is part of the product target, ManyForge must
  expose enough structured diagnostic context that an orchestration layer such
  as OpenClaw can relay it through operator messaging channels without losing
  provenance.
- EventRecorder / debug-export work in this phase should treat the following as
  first-class event families rather than ad hoc details inside generic logs:
  - `model_query_*`
  - `delegation_*`
  - `supervisor_*`

**Estimated work**: 2-4 agents + manual validation on the target deployment
stack

---

## 8. Testing Strategy

Each new endpoint gets:
1. **Unit test**: Mock kernel, verify JSON schema of response
2. **Integration test**: Real kernel (simulated mode), verify data matches expected state
3. **LLM smoke test**: Prompt an LLM with the endpoint docs and verify it can
   formulate correct API calls to accomplish a goal

For optional external-agent integration work (Phase 6), add:
4. **Sandboxed-agent end-to-end test**: An external agent runtime reaches
   ManyForge only through the intended HTTP/MCP transport, performs a bounded
   workflow, and leaves a fully correlated audit trail in ManyForge history.

### LLM Smoke Test Protocol

```
Given: pick_and_place demo loaded, idle state
Prompt LLM: "The graspable object is at [0.3, -0.3, 0.15]. Move it to [0.3, 0.3, 0.15]."
Expected: LLM queries /api/scene/state, /api/catalog, constructs a valid tree
          via PUT /api/program/tree, runs it via POST /api/cycle/run,
          monitors via GET /api/execution/digest until completion.
Pass criteria: Object ends up at target location (within tolerance).
```

---

## 9. Success Criteria

ManyForge is LLM-operable when an LLM agent can, using only HTTP APIs:

1. Start the application from a Docker image (given a compose file)
2. Load an existing program and deployment
3. Query and understand the complete scene state
4. Modify the behavior tree to change robot actions
5. Validate the tree before running
6. Execute the program and monitor progress
7. Detect when something goes wrong (failed planning, collision, timeout)
8. Query execution history to understand what happened
9. Adapt the tree based on observations and re-run
10. Optionally use vision to verify the physical scene matches expectations

Items 1-6 require Phase 1 + Phase 2.
Items 7-8 require Phase 2.
Item 9 requires Phase 1 + Phase 2 + the skill catalog / `LLM_GUIDE.md`.
Item 10 requires Phase 3.

Additional criteria:

11. Query current deployment capabilities and determine whether a skill is
    compatible before composition
12. Compose or modify a program using validated skills rather than only raw
    primitive nodes
13. Execute a temporary intervention subtree without rewriting the saved
    program, then resume, stop, or reset with full traceability

Item 11 requires `GET /api/capabilities` in Phase 3.
Item 12 requires the skill catalog in Phase 3.
Item 13 requires intervention mode in Phase 3.

Extended criteria (with ROS2 sidecar, Phase 4):

14. Record execution data persistently for post-mortem analysis (rosbag2)
15. Monitor component health through standardized diagnostics (ros2_medkit)
16. Use real camera feeds for visual scene verification (camera_vllm_bridge)
17. Replay recorded sessions to compare expected vs actual behavior
18. Produce and maintain a `ROS2_MEDKIT_MAPPING.md` showing one-to-one mapping
    to ros2_medkit capabilities where applicable, with clear ManyForge
    extensions where not applicable
19. Bring up ManyForge + ROS2 sidecar + ros2_medkit through a documented,
    low-friction deployment path

Optional integration criteria (Phase 6, when OpenClaw/model-in-the-loop support is targeted):

20. Expose the ManyForge MCP surface over an external-agent-friendly transport
    (HTTP/SSE or equivalent) without requiring ManyForge to run inside the
    external sandbox
21. Execute a cataloged `query_model` node that writes structured outputs to
    the blackboard with explicit timeout/confidence handling
22. Enforce delegated-recovery action scope at the backend/tool layer rather
    than only in prompt text
23. Provide a Composer assistant that can propose valid tree diffs without
    silently mutating the saved artifact
24. Record model prompts/responses or approved redacted equivalents with enough
    correlation metadata for industrial audit/debug use
25. Emit first-class model/delegation/supervisor event families that let an
    external operator runtime reconstruct what happened without scraping
    free-form logs
26. Support structured escalation context so an external orchestration layer
    can relay actionable diagnostics to a human operator while preserving
    ManyForge provenance
27. Record the concrete `principal_id`, grant type, session ID, and resulting
    runtime/artifact effect for every externally effective LLM action
28. Bind runtime overrides to the exact authored program revision they were
    validated against, and invalidate or revalidate them on program change
29. Require explicit node outcome contracts and bounded recovery policy for any
    node type that permits LLM intervention on failure

---

## 10. Security Audit

Full-repo security review conducted 2026-04-10. Findings are categorized by
severity (HIGH / MEDIUM / LOW / INFO) and grouped by attack surface.

### 10.1 Findings Fixed During Review

These were discovered and remediated during the Phase 3-5 security review,
with tests added.

| ID | Severity | Finding | Fix | Tests |
|----|----------|---------|-----|-------|
| SEC-F1 | HIGH | MCP `manyforge_program_load` accepted arbitrary filesystem paths | Added allowlisted-root validation with shared path-normalization helpers | `test_load_program_path_traversal_blocked`, `test_load_deployment_path_traversal_blocked` |
| SEC-F2 | HIGH | `CycleManager.set_velocity_scale` accepted NaN/inf, risking undefined robot behavior | Added `math.isnan`/`math.isinf` guard with `ValueError` | `test_nan_rejected`, `test_inf_rejected` (CycleManager + MCP) |
| SEC-F3 | MEDIUM | MCP error messages leaked internal filesystem paths and stack traces | Sanitized all MCP tool error returns to generic messages | Covered by existing error-path tests |
| SEC-F4 | LOW | `mcp` pip package was unpinned (could pull breaking or malicious version) | Pinned exact runtime dependencies in the Docker image, including `mcp==1.27.0` | N/A (build-time) |
| SEC-F5 | HIGH | CORS allowed all origins (`*`) | Restricted to configured origins via `SecurityConfig` | `test_security.py::TestCORS` |
| SEC-F6 | HIGH | No authentication on HTTP or WebSocket endpoints | Bearer token auth middleware for HTTP, query-param token for WS | `test_security.py::TestBearerAuth`, `TestWebSocketAuth` |
| SEC-F7 | HIGH | File browser routes had no path traversal protection | Added shared allowlisted-root guards in filesystem, REST artifact load/save, and MCP artifact load flows | `test_security.py::TestPathTraversal` |
| SEC-F8 | MEDIUM | WebSocket accepted unbounded message sizes | 256-byte message size limit | `test_security.py::TestWebSocketMessageLimit` |

### 10.2 Residual Findings / Deployment Notes

The critical and medium-severity findings above have been remediated in the
current implementation. The remaining items are follow-on hardening or product
deployment decisions, not blockers for local/container development.

#### LOW Severity

**SEC-R1: Library-level save helpers still trust their caller**

- **Location**: `deployment.py` (`Deployment.save(path)`), `program.py`
- **Description**: The route/MCP layers now validate artifact paths before
  writes, but the lower-level library save helpers still accept any path
  string.
- **Impact**: Defense-in-depth gap if a future caller bypasses the validated
  API layer.
- **Remediation**: Keep path validation mandatory at all external entrypoints.
  Optionally add an internal trusted-path wrapper type later if direct library
  exposure grows.

**SEC-R2: Runtime root allowlists are policy-sensitive**

- **Location**: `SecurityConfig.allowed_fs_roots`
- **Description**: A permissive allowlist is still a policy risk even though
  traversal and symlink-escape protections are enforced.
- **Impact**: Larger blast radius if operators allowlist broad parts of the
  host filesystem.
- **Remediation**: Production deployments should set the smallest practical
  allowlist and run inside a container or restricted service account.

**SEC-R3: Model endpoint trust, prompt logging, and retention are product decisions**

- **Location**: future `query_model`, delegated-recovery, supervisory-agent,
  and assistant integrations
- **Description**: Once ManyForge or an attached orchestration stack can send
  prompts/responses to model endpoints, the system starts handling potentially
  sensitive process data, scene state, and recovery context outside the core
  planner/executor path.
- **Impact**: Misconfigured endpoint trust, logging, or retention policy can
  create data-exposure and auditability failures even if robot motion remains
  safe.
- **Remediation**: Define an explicit allowlist for model endpoints, a prompt
  redaction policy, retention/access-control rules for prompt/response logs,
  and deployment guidance for local-vs-remote inference.

#### INFO (No Action Required)

| ID | Area | Finding |
|----|------|---------|
| SEC-I1 | YAML deserialization | All YAML loading uses `yaml.safe_load` — no unsafe deserialization risk |
| SEC-I2 | Event recorder | Trace directory and run IDs are internally generated — no path injection vector |
| SEC-I3 | Scene edit validation | `SceneEditApiRequest` uses Pydantic `Literal` types + `model_validator` — robust input validation |
| SEC-I4 | Frontend dependencies | Pinned via `package-lock.json` with `npm ci` — deterministic installs |
| SEC-I5 | Blackboard snapshot | Truncation applied to large values — no unbounded memory risk in API response |

### 10.3 Remediation Priority

For hardware deployment, fix in this order:

1. Keep filesystem allowlists tight and review them per deployment (`SEC-R2`)
2. Preserve validated entrypoints for all artifact load/save operations
   (`SEC-R1`)
3. Add stronger network-fronting/auth around any remote-exposed HMI/Viser
   deployment
4. Define model-endpoint trust and prompt-log retention policy before enabling
   model-in-the-loop or external-agent flows (`SEC-R3`)

---

## 11. Open Points

Design decisions and known limitations that are deferred but worth tracking.

### OP-1: Intervention blackboard promotion

Implemented on the current branch:

- intervention-scoped writes now use the runtime-safe slash-path namespace
  `_intervention/<session_id>/...`,
- explicit promotion is available through `/api/direct/promote` and the MCP
  intervention tool,
- session history now records promotion events and promoted key mappings.

**Remaining**:
- decide whether any safe automatic-promotion mechanism is ever desirable, or
  whether promotion should remain explicit-only,
- document any future bulk-promotion convenience features against the same
  safety rules.

### OP-2: Intervention blackboard scoping is resolved on the current branch

Intervention rewriting is now catalog-driven rather than hardcoded in
`cycle_manager.py`.

Implemented on the current branch:

- node entries may declare `bb_reader_params` / `bb_writer_params`,
- the runner/catalog validator checks that those metadata fields reference
  declared parameters only,
- intervention scoping consults catalog metadata dynamically, so new
  blackboard-aware nodes no longer require `CycleManager` code changes just to
  participate in scoped intervention writes.

**Remaining**:
- keep the shipped catalog metadata complete as new blackboard-aware nodes are
  added,
- expose/use the metadata consistently across any future authoring or analysis
  surfaces that reason about blackboard dependencies.

### OP-3: MCP tool response key casing inconsistency is resolved on the current branch

MCP responses are now normalized to **snake_case** even when the MCP tool
wraps a REST builder that naturally emits camelCase aliases. This removes the
need for tool-specific casing exceptions on the MCP surface.

**Remaining**:
- keep new MCP tools on the same normalization path,
- ensure any future schema examples in `MCP_SCHEMA.md` stay aligned with the
  snake_case contract.

### OP-4: Direct-control authority model is converged in principle; identity/auth implementation remains

Implemented on the current branch:

- `IDLE -> DIRECT_CONTROL` is rejected unless the request is an explicitly
  human-approved `manual_intervention`,
- intervention requests now carry `session_kind`, `authority_source`,
  `trigger_reason`, `trigger_node_instance`, `approval_mode`, and
  `artifact_origin`,
- provenance is carried through REST, MCP, history, and debug export, and
- public `delegated_recovery` remains reserved instead of being accepted
  implicitly.

**Converged direction**:
- authority is modeled as a **scoped grant**, not a mode bit
- active baseline grant types are:
  - `human_approved`
  - `policy_granted`
- `deployment_granted` is reserved for a future deployment-level policy path,
  but it does **not** authorize `IDLE -> DIRECT_CONTROL` in the baseline
  product
- `IDLE -> DIRECT_CONTROL` remains human-gated in the baseline industrial
  product
- intervention provenance must carry a concrete `principal_id` distinct from
  the transport/session identifier

**Remaining**:
- define the trust boundary for `principal_id` propagation:
  - native per-principal token mapping, and/or
  - trusted proxy/header injection
- propagate `principal_id` through REST, stdio MCP, future HTTP/SSE MCP, event
  history, and debug export
- align specs and deployment/security docs with the scoped-grant model and the
  `IDLE -> DIRECT_CONTROL` restriction

### OP-5: Runtime-override revision binding and stale-override handling — IMPLEMENTED

Implemented on the current branch:

- `modification_policy.runtime_override` is now parsed and preserved in the
  program tree model,
- runtime overrides have a first-class ledger/API surface
  (`GET/POST /api/program/runtime-overrides`, activation, removal, clear, MCP
  parity),
- runtime overrides are validated by rebuilding a full effective program
  against the same program/profile/capability contracts as authored trees,
- mutation is restricted to `IDLE` and `DIRECT_CONTROL`,
- override state is tracked as `active` vs `staged`, with `STOPPED` requiring
  reset before staged overrides can take effect, and
- explicit materialization is now available:
  - non-destructive preview via
    `GET /api/program/runtime-overrides/materialized-tree`
  - promotion into the loaded in-memory program via
    `POST /api/program/runtime-overrides/promote {target=loaded_program}`
- explicit save-to-artifact via
    `POST /api/program/runtime-overrides/promote {target=save}`
    with persistent-edit policy enforcement, and
- digest/debug export/event history now surface runtime-override state and
  override events.
- **base-program revision binding**: every `RuntimeOverrideRecord` stores a
  `base_program_hash` (SHA-256 of the normalized program tree) computed at the
  time the override was applied.
- **stale-override detection**: when the program tree changes, overrides whose
  `base_program_hash` no longer matches are flagged as stale.
- **stale enforcement**: stale overrides cannot be activated, materialized, or
  used to build an effective program. They must be explicitly revalidated
  (`POST /api/program/runtime-overrides/revalidate`, MCP
  `manyforge_runtime_override action=revalidate`) or discarded.
- **no silent merge**: loading a new program or editing the tree while
  overrides exist is rejected unless the caller passes
  `forceDiscardOverrides=true` (REST) or `force_discard_overrides=true` (MCP).
- **revalidation**: stale overrides can be re-checked against the new program;
  valid ones get their hash updated, invalid ones are removed from the ledger.

**Converged direction**: runtime overrides remain **parameter-patch only**.
Structural tree changes are **not** runtime overrides; they remain full
program edits and would require a future ADR/spec effort.

**Remaining**:
- add explicit UI/HMI controls for promote-to-loaded-program /
  save-materialized workflows and stale-override indicators
- align specs so runtime overrides are explicitly limited to parametric patching

### OP-6: Delegated recovery direction is converged; typed recovery and node-level recovery contracts remain to be implemented

Implemented on the current branch:

- node-instance `modification_policy.intervention_delegate` is now a real
  runtime policy hook rather than reserved metadata only,
- when an allowed node fails, the runtime can stop in `STOPPED` with a durable
  pending delegated-recovery request,
- that pending request carries trigger provenance, allowed authorities, allowed
  actions, and resume-target constraints,
- public `delegated_recovery` sessions are now accepted only when such a
  pending request exists and the caller authority matches it, and
- digest/history/debug/MCP surfaces now expose the pending request.

**Converged direction**:
- the baseline industrial recovery model does **not** include `skip` as a
  supported recovery outcome
- nodes that may allow LLM intervention on failure must define explicit:
  - success criteria,
  - failure criteria / failure classes, and
  - the evidence used to verify the outcome
- node-instance recovery metadata should bound the recovery envelope:
  - whether LLM intervention on failure is allowed,
  - allowed deterministic recovery actions,
  - allowed skills and any skill-specific limits,
  - persistence ceiling of the resulting change
    (`session_only`, `runtime_override`, `draft_patch`, etc.),
  - optional bounded recovery context/template for the assisting agent
- typed recovery nodes are preferred over a generic high-authority
  `delegate_to_llm` baseline:
  - `retry_node`
  - `fallback_override_node`
  - `recovery_skill_node`
- `program_only` recovery remains deterministic-only
- `open_draft_patch` is terminal: it may stop the run and emit a patch
  proposal, but it does not auto-resume execution
- recovery trigger reasons should be a closed runtime enum rather than ad hoc
  strings
- any future attempt to introduce `skip` would require an explicit ADR/spec
  change rather than a recovery-policy flag or implementation shortcut

**Remaining**:
- add `recovery_policy` (or equivalent) and node-outcome contract fields to the
  program/schema/catalog specs
- implement closed `RecoveryTrigger` enum and the typed deterministic recovery
  nodes
- add explicit UI/HMI affordances for pending delegated-recovery requests
- keep the current stop-and-request path as the safe externally serviced
  fallback
- define any future model-assisted in-tree recovery separately from the
  deterministic baseline

**Current limitation**: delegated recovery is explicit and enforceable, but the
first implementation is still externally serviced. A saved program can raise a
recovery request; it cannot yet execute typed deterministic recovery nodes or a
future model-assisted in-tree recovery path.

### OP-7: `query_model` initial implementation is landed; spec alignment and multi-model hardening remain

OpenClaw-style per-cycle perception is useful, but it introduces a new class of
runtime node that mixes prompt templating, structured output parsing, blackboard
writes, timeout handling, and optional scene-snapshot attachment.

**Converged direction**:
- `query_model` starts with a single structured-output mode:
  JSON-schema-validated output
- the node references a deployment-configured `model_endpoint` alias, not a
  raw URL
- the node references a deployment-side `prompt_template_id` with a stable
  version/hash, not inline prompt text
- timeout is wall-clock and cancellation must respect the BT/kernel stop path
- blackboard writes are atomic at the namespace level
- all effective model activity must be durably traced with enough references or
  hashes to reconstruct what the model saw without storing full prompt text by
  default
- structured output may use either direct JSON-schema validation or a
  deployment-defined deterministic `response_tool` when the model family
  supports tool calls more reliably than free-form JSON

**Remaining**:
- align the normative specs to the shipped `query_model` contract
- define prompt-template variable rules and the deployment-side template
  registry contract in the specs
- define formal `response_tool` schema rules and the compatibility policy for
  model families that differ in tool-call behavior
- add per-endpoint capability flags so deployments can declare whether an
  endpoint supports response tools, image input, JSON-object output, and any
  model-family-specific parser requirements
- define attachment hashing/reference rules for scene snapshots / scene-state
  payloads
- keep assistant model configuration separate from per-node model configuration
  in the deployment/UI contract
- extend tests across mocked and real model endpoints, including a
  compatibility matrix for the supported local model families

**Impact**: Without this hardening, `query_model` works as a runtime feature
but remains weaker than it should be as an industrial multi-model surface.

### OP-8: External-agent transport direction is converged; transport/auth implementation remains

If NemoClaw/OpenClaw becomes a supported orchestration layer, ManyForge needs a
blessed transport for the MCP/tool surface outside local stdio use.

**Converged direction**:
- HTTP/SSE MCP is the blessed external-agent transport
- REST remains a fallback for prototyping/integration, not the recommended
  external orchestration path
- internal model-node work does not depend on external transport
- per-principal identity is required for industrial deployment; a shared token
  is acceptable only as a development bootstrap

**Remaining**:
- implement `/api/mcp` + `/api/mcp/sse`
- define the auth trust model:
  - native token-to-principal mapping, and/or
  - trusted auth proxy/header injection
- add end-to-end validation from a sandboxed external runtime with
  `principal_id` correlation carried through the trace

**Impact**: The internal MCP server is useful locally today, but external
industrial orchestration remains incomplete until transport and identity are
implemented together.

### OP-9: Model-call audit direction is converged; event-schema and retention implementation remain

The plan now assumes more model-facing surfaces: runtime `query_model`, future
delegated recovery, supervisory agents, and optional Composer chat assistance.
Industrial deployment needs a consistent answer to what is logged and how.

**Converged direction**:
- full expanded prompt text is **not** stored by default
- the default retained record should include:
  - `prompt_template_id` + template version/hash
  - resolved argument key names plus argument value hashes / redacted values
  - model endpoint alias + model version where available
  - structured validated response fields (redacted per deployment policy)
  - raw response hash
  - outcome / latency / token counts
  - `run_id` / `session_id` / `principal_id` correlation
- tool-call logs for delegated recovery / supervisory agents are stored by
  default
- agent scratchpad / hidden reasoning is not stored by default

**Remaining**:
- define the model-event schemas and deployment audit-policy schema
- implement redaction and retention controls
- ensure debug export applies the same redaction policy
- define template/version retention rules so prompt reconstruction remains
  possible without full-text-by-default storage

**Impact**: This is not a motion-safety blocker, but it is a deployment and
audit blocker for real industrial use.

---

## 12. Next Execution Steps

The discussion has converged enough that the next work should be driven as an
implementation/spec sequence rather than more open-ended design debate.

### 12.1 Immediate documentation/spec alignment

Because `LLM_OPERABILITY_PLAN.md` is non-normative and the spec documents are
normative, the next implementation batch should start by aligning the specs to
the converged decisions above.

**Required spec updates**:
- [420-program-yaml-schema.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/420-program-yaml-schema.md)
  - add/clarify `recovery_policy` (or equivalent) for node-instance bounded
    recovery metadata
  - make runtime overrides explicitly parameter-patch only
  - add base-program revision binding requirement for overrides
- [440-node-catalog-format-and-versioning.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/440-node-catalog-format-and-versioning.md)
  - add node outcome-contract fields (success criteria, failure classes,
    verification evidence)
  - add `query_model` contract, or reserve it pending a dedicated spec
- [450-composer-ui-session-and-artifact-workflow.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/450-composer-ui-session-and-artifact-workflow.md)
  - define HMI behavior for active/stale overrides
  - define HMI behavior for pending delegated-recovery requests
- [460-skill-catalog-and-intervention.md](/home/tndlux/workspaces/dev_ws/src/manyforge_specs/docs/spec/460-skill-catalog-and-intervention.md)
  - scoped grants and `principal_id`
  - typed deterministic recovery nodes as the baseline recovery direction
  - model-call audit and delegated-recovery trace requirements
- new spec recommended:
  - `470-model-node-contracts.md` for `query_model` and future model-facing
    nodes, unless folded into 440/460 cleanly
- observability spec update recommended:
  - add model-event schemas and redaction/retention policy to the relevant
    observability/tracing spec when introduced

### 12.2 Next implementation batch

Proceed in this order:

1. **OP-4 + OP-8 identity/auth foundation**
   - add `principal_id` plumbing through intervention records and traces
   - define trusted identity source for future HTTP/SSE MCP

2. **OP-5 runtime-override hardening** — DONE
   - bind overrides to base program revision hash — implemented
   - invalidate/revalidate stale overrides — implemented
   - add HMI affordances for promote/materialize/save and stale-override state — deferred (UI task)

3. **OP-6 deterministic recovery baseline**
   - implement closed `RecoveryTrigger` enum
   - add typed deterministic recovery nodes
   - implement per-node bounded `recovery_policy`
   - keep current external stop-and-request path as fallback

4. **OP-9 model-event/audit implementation**
   - structured model event family
   - default redacted/hash-based logging
   - debug-export alignment
   - must land before or alongside `query_model` so auditable events are
     recorded from the first model call

5. **Model capability + assistant configuration hardening**
   - add per-endpoint capability flags (`supports_response_tools`,
     `supports_image_input`, `supports_json_object`, etc.)
   - separate assistant endpoint/model configuration from per-node
     `query_model` endpoint selection
   - add `/api/assistant/chat` plus the initial Composer assistant surface

6. **OP-8 HTTP/SSE MCP transport**
   - expose `/api/mcp` + `/api/mcp/sse`
   - propagate `principal_id`
   - validate from sandboxed external runtime
   - no internal dependency; needed only when external orchestration is
     targeted

### 12.3 After the above

Once the authority/recovery/model-call layer is stable:

1. proceed to **Phase 4** kernel event bridge + ROS2 sidecar
2. proceed to the **contact-rich execution** design spike and follow-on spec
   work
3. then implement the remaining optional Phase 6 features such as Composer
   assistant chat if still in scope
