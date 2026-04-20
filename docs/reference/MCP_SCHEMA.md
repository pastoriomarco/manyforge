> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge MCP Tool Schema

Machine-readable interface contract for the ManyForge MCP server. Each tool
wraps one or more REST API endpoints from the Composer backend, operating on
the same in-process state with no HTTP overhead.

## Quick Start

```bash
# Start the MCP server over stdio
python -m manyforge_composer --mcp --catalog-path /path/to/node_catalog.yaml
```

The server uses the stdio transport by default. Configure it in your MCP
client's server list:

```json
{
  "mcpServers": {
    "manyforge": {
      "command": "python",
      "args": ["-m", "manyforge_composer", "--mcp", "--catalog-path", "/workspace/manyforge_behavior/resources/node_catalog.yaml"]
    }
  }
}
```

---

## Tools

### manyforge_scene_state

Get the full scene state including robot joint positions, TCP pose, and all
collision objects.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| *(none)* | | | |

**Returns**: JSON with `robot` (joint positions, TCP pose, IDs), `objects`
(list of scene objects with poses, shapes, attachment status), `timestamp_ns`.

**REST equivalent**: `GET /api/scene/state`

---

### manyforge_scene_edit

Edit the collision scene at runtime.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `operation` | string | yes | `"add"`, `"remove"`, `"update_pose"`, `"attach"`, `"detach"` |
| `object_id` | string | yes | Unique scene object identifier |
| `shape_type` | string | for add | `"box"` or `"sphere"` |
| `box_dimensions_m` | float[3] | for box | [x, y, z] dimensions in meters |
| `sphere_radius_m` | float | for sphere | Radius in meters |
| `pose_position_m` | float[3] | for add/update | [x, y, z] position |
| `pose_orientation_xyzw` | float[4] | for add/update | Quaternion [x, y, z, w] |
| `frame_id` | string | for attach/detach | Robot link frame name |

**Returns**: `{"success": bool, "error": string|null, "timestamp_ns": int}`

**REST equivalent**: `POST /api/scene/edit`

**Important**: After any scene edit that modifies the collision environment,
include a `reset_trajectories` node in your behavior tree before the next
motion node.

---

### manyforge_program_load

Load a program and optional deployment from YAML files on disk.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `program_path` | string | yes | Absolute path to `.program.yaml` |
| `deployment_path` | string | no | Absolute path to `.deployment.yaml` |

**Returns**: Program metadata (name, description, robot config, counts).

**REST equivalent**: `POST /api/program/load`

---

### manyforge_program_tree

Read or replace the current behavior tree.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | yes | `"read"` or `"write"` |
| `tree` | object | for write | Nested tree dict with `id`, `name`, `params`, `children` |

**Returns**: For read: `{"tree": {...}}`. For write: `{"status": "ok"}`.

**REST equivalents**: `GET /api/program/tree`, `PUT /api/program/tree`

**Rules**:
- Cannot write while execution is RUNNING (returns error)
- Always call `manyforge_program_validate` before writing

---

### manyforge_program_validate

Validate a candidate behavior tree without applying it (dry-run).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tree` | object | yes | Candidate tree dict |

**Returns**: `{"valid": bool, "errors": [{path, message}], "warnings": [{path, message}]}`

**REST equivalent**: `POST /api/program/validate`

**Always call this before `manyforge_program_tree(action="write")`.**

---

### manyforge_cycle_control

Control program execution lifecycle.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | yes | `"run"`, `"stop"`, `"reset"`, `"status"`, `"velocity"` |
| `velocity_scale` | float | for velocity | 0.0 to 1.0 |

**State machine**: `IDLE` -> `RUNNING` -> `STOPPED` -> `IDLE` (via reset).
During interventions, state is `DIRECT_CONTROL`.

**Returns**: Current state (`"idle"`, `"running"`, `"stopped"`,
`"direct_control"`) or velocity scale.

**REST equivalents**: `POST /api/cycle/run`, `POST /api/cycle/stop`,
`POST /api/cycle/reset`, `GET /api/cycle/status`,
`PUT /api/cycle/velocity-scale`

**Warning**: Setting `velocity_scale` to 0.0 stalls the robot. Use `"stop"`
to pause execution instead.

**Runtime-override interaction**:
- `run` auto-activates staged runtime overrides only from `IDLE`.
- `run` from `STOPPED` is rejected if staged runtime overrides exist; reset
  first so the paused tree is not resumed against a different effective
  program snapshot.

---

### manyforge_capabilities

Query the deployment-declared capability surface available to the current
runtime session.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| *(none)* | | | |

**Returns**: Capability keys grouped by category (`planning`, `execution`,
`scene`, `sensing`, `context`, `other`), `adapterIds`, `programProfile`,
`contactTier`, and `timestampNs`. This is currently derived from the loaded
deployment artifact, not dynamic backend self-reporting.

**REST equivalent**: `GET /api/capabilities`

---

### manyforge_skills

Get the skill catalog — validated subtree templates for common operations.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| *(none)* | | | |

**Returns**: List of skills with `id`, `name`, `description`, `parameters`,
`requires`/`provides`/`modifies` contracts, `body` (raw node subtree),
legacy alias `expands_to`, and `example`.

**REST equivalent**: `GET /api/skills`

**Prefer skills over raw nodes** for standard operations like pick/place.

---

### manyforge_intervention

Execute, monitor, or abort a temporary intervention subtree.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | yes | `"execute"`, `"status"`, `"pending_recovery"`, `"abort"`, `"heartbeat"` |
| `tree` | object | for execute | Subtree dict to run |
| `lease_timeout_ms` | integer | no | Lease duration override / heartbeat renewal |
| `resume_target` | string | no | `resume`, `stop`, `reset`, or `stay_idle` |
| `session_kind` | string | recommended for execute | Currently `manual_intervention` only |
| `authority_source` | string | recommended for execute | `human`, `llm`, `program_node`, or `policy` |
| `approval_mode` | string | recommended for execute | `human_approved`, `policy_granted`, or `not_required` |
| `trigger_reason` | string | no | Why the session was requested |
| `trigger_node_instance` | string | no | Node instance that triggered delegated recovery |
| `artifact_origin` | string | no | `saved_program` or `runtime_override` |

Interventions run a one-shot subtree without modifying the saved program.
Requires IDLE or STOPPED state.

Important gating rules:

- `IDLE -> DIRECT_CONTROL` is rejected unless the request is an explicitly
  human-approved `manual_intervention`.
- `delegated_recovery` is accepted only when a pending delegated-recovery
  request already exists from an explicit node policy hook.
- `program_only` delegated recovery remains reserved until an internal executor
  exists.

**Returns**: Session info (`session_id`, `session_kind`, `authority_source`,
`parent_run_id`, `entry_state`, `resume_target`, `trigger_reason`,
`trigger_node_instance`, `approval_mode`, `artifact_origin`, `status`,
`actual_outcome`, `error`, `started_at_ns`, `finished_at_ns`, `events`,
`timestamp_ns`) or, for `pending_recovery`, a request record
(`request_id`, `parent_run_id`, `trigger_node_instance`, `trigger_reason`,
`delegate_policy`, `allowed_authority_sources`, `allowed_actions`,
`allowed_resume_targets`, `default_resume_target`, `requested_at_ns`,
`timestamp_ns`).

**REST equivalents**: `POST /api/direct/execute`, `GET /api/direct/status`,
`GET /api/direct/pending-recovery`, `POST /api/direct/abort`,
`POST /api/direct/heartbeat`

---

### manyforge_runtime_override

Inspect or mutate the runtime-override ledger.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | yes | `"status"`, `"materialized_tree"`, `"apply"`, `"promote"`, `"activate"`, `"remove"`, `"clear"` |
| `node_name` | string | for apply/remove | Node instance name targeted by the override |
| `params` | object | for apply | Param patch to merge onto that node instance |
| `include_staged` | boolean | no | Include staged overrides when materializing/promoting |
| `target` | string | for promote | `"loaded_program"` or `"save"` |
| `path` | string | for save promote | Save target path (defaults to current program source path when available) |
| `authority_source` | string | for apply | `human` or `llm` |
| `approval_mode` | string | for apply/save promote | `human_approved` or `not_required` |
| `reason` | string | no | Human/LLM reason text for auditability |
| `session_id` | string | no | Optional intervention session correlation |
| `trigger_node_instance` | string | no | Optional triggering node instance |

Override rules:

- Runtime overrides are param-level patches on a named node instance.
- The targeted node instance must allow them via
  `modification_policy.runtime_override`.
- Mutation is allowed only in `IDLE` and `DIRECT_CONTROL`.
- Applying in `IDLE` activates immediately by rebuilding the effective program.
- Applying in `DIRECT_CONTROL` stages the override for later activation.
- Staged overrides cannot be activated while resuming from `STOPPED`; reset
  first.
- `promote` with `target=loaded_program` materializes selected overrides into
  the loaded in-memory program and clears the corresponding ledger entries.
- `promote` with `target=save` writes a materialized program artifact and
  enforces `modification_policy.persistent_edit`.

**Returns**: snake_case runtime-override ledger state:
`active`, `staged`, `active_count`, `staged_count`, `has_active`,
`has_staged`, `can_activate_now`, `requires_reset_to_activate`,
`current_tree_origin`, `next_effective_tree`, `timestamp_ns`; or, for
`materialized_tree` / `promote`, the corresponding materialized tree plus
runtime-override status.

**REST equivalents**: `GET /api/program/runtime-overrides`,
`GET /api/program/runtime-overrides/materialized-tree`,
`POST /api/program/runtime-overrides`,
`POST /api/program/runtime-overrides/promote`,
`POST /api/program/runtime-overrides/activate`,
`DELETE /api/program/runtime-overrides/{nodeName}`,
`POST /api/program/runtime-overrides/clear`

---

### manyforge_execution_status

Get execution status information.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include` | string | no | `"digest"` (default), `"history"`, `"blackboard"`, `"all"` |

- **digest**: High-level polling summary (state, current node, progress,
  completed/remaining nodes, object positions, errors, warnings). Poll every
  2-5 seconds during execution.
- **history**: Event log for current/last run.
  Supports backend-side filtering via REST query parameters such as
  `minSeverity`, `components`, `eventTypes`, `executionId`,
  `programRunId`, and `interventionRunId`.
- **blackboard**: py_trees blackboard key-value state.
- **all**: Everything combined.

**REST equivalents**: `GET /api/execution/digest`, `GET /api/execution/history`,
`GET /api/execution/blackboard`

---

### manyforge_debug_export

Export a full debug/trace bundle for post-mortem analysis.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `run_id` | string | no | Specific run ID (defaults to most recent) |

**Returns**: Complete bundle including kernel snapshot, metrics, execution
digest, history, scene state, diagnostics, scene trace, blackboard, node
statuses, and a normalized `medkitTrace` correlation export.

**REST equivalent**: `GET /api/debug/export`

---

### manyforge_catalog

Query the behavior tree node catalog.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `entry_id` | string | no | Specific node type ID (omit for full catalog) |

**Returns**: Node type entries with `id`, `name`, `category`, `description`,
`kind`, children constraints, parameters with types/constraints/defaults,
and required capabilities.

**REST equivalents**: `GET /api/catalog`, `GET /api/catalog/{id}`

---

## Composition Workflow

The recommended workflow for an LLM agent:

1. **Discover**: `manyforge_catalog()` + `manyforge_skills()` to understand
   available node types and pre-built skill templates.

2. **Load**: `manyforge_program_load(program_path, deployment_path)` to
   initialize the runtime.

3. **Observe**: `manyforge_scene_state()` to understand the current world.

4. **Compose**: Build a behavior tree using catalog nodes and skill patterns.

5. **Validate**: `manyforge_program_validate(tree)` to check for errors.

6. **Apply**: `manyforge_program_tree(action="write", tree=tree)` to set
   the tree.

7. **Execute**: `manyforge_cycle_control(command="run")` to start.

8. **Monitor**: Poll `manyforge_execution_status(include="digest")` every
   2-5 seconds until state is no longer `"running"`.

9. **Verify**: `manyforge_scene_state()` to confirm the final world state.

10. **Debug** (on failure): `manyforge_debug_export()` for full trace.

For quick one-off actions without modifying the saved program, use
`manyforge_intervention(action="execute", tree=subtree, session_kind="manual_intervention", authority_source="human", approval_mode="human_approved")`
instead of steps 5-7.

---

## Error Handling

All tools return JSON. Errors are signaled by an `"error"` key in the
response (not by MCP-level errors), keeping the tool call successful from
the protocol perspective. This allows the LLM to inspect the error message
and decide how to recover.

Common error patterns:
- `"No program loaded"` — call `manyforge_program_load` first
- `"Cannot modify tree while RUNNING"` — stop or reset first
- `"Scene edits not allowed while RUNNING"` — stop or reset first
- `"No deployment loaded"` — load a deployment for capability queries

## Key Naming Convention

MCP tool responses use **snake_case** keys (e.g. `expands_to`,
`compatible_profiles`, `requires_capabilities`). The equivalent REST API
responses use **camelCase** aliases (e.g. `expandsTo`, `compatibleProfiles`).
Wrapped REST payloads are normalized to snake_case before being returned over
MCP so LLM agents can treat the MCP surface as internally consistent.
