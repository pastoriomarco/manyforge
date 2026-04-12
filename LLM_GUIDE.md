# ManyForge LLM Composition Guide

Machine-readable guide for LLM agents composing and operating ManyForge robot
programs. Each pattern references an executable example or integration test.

> **MCP alternative**: All endpoints below are also available as MCP tools
> via `python -m manyforge_composer --mcp`. See [`MCP_SCHEMA.md`](MCP_SCHEMA.md)
> for the tool interface contract.

---

## 1. API Quick Reference

### Program Management
```
POST /api/program/load       — Load program + deployment from YAML files
GET  /api/program             — Current program metadata
GET  /api/program/tree        — Read the current (working) behavior tree
GET  /api/program/saved/tree  — Read the last-saved behavior tree
PUT  /api/program/tree        — Replace the behavior tree
POST /api/program/validate    — Dry-run validation (no side effects)
POST /api/program/save        — Save program to disk
```

### Deployment Management
```
POST /api/deployment/load        — Load a deployment from YAML
GET  /api/deployment             — Current deployment metadata
GET  /api/deployment/full        — Full deployment with capabilities
GET  /api/deployment/saved/full  — Last-saved deployment (full)
PUT  /api/deployment             — Replace deployment in memory
POST /api/deployment/save        — Save deployment to disk
```

### Cycle Control
```
GET  /api/cycle/status         — Current cycle state + velocity
POST /api/cycle/run            — Start execution
POST /api/cycle/stop           — Pause execution
POST /api/cycle/reset          — Reset to IDLE
PUT  /api/cycle/velocity-scale — Set velocity (0.0 to 1.0)
```

### Session
```
POST /api/session/apply      — Apply a combined program + deployment session
```

### Observation
```
GET /api/scene/state         — Full world snapshot (robot + objects)
GET /api/execution/digest    — High-level status for polling (every 2-5s)
GET /api/kernel/snapshot     — Execution state + joint positions
GET /api/kernel/metrics      — Performance counters
GET /api/kernel/events       — Native kernel events (since=N for polling)
GET /api/execution/history   — Event log for current/last run
GET /api/execution/blackboard — py_trees blackboard key-value state
GET /api/scene/diagnostics   — Backend vs HMI comparison
GET /api/debug/export        — Full trace bundle for post-mortem
GET /api/debug/scene-trace   — Scene edit audit trail
```

### Discovery
```
GET /api/catalog             — All available node types with schemas
GET /api/catalog/{id}        — Single node type schema
GET /api/skills              — Skill catalog with contracts and examples
GET /api/capabilities        — Deployment-declared capability summary
```

### Scene Mutation
```
POST /api/scene/edit         — Add/remove/update objects at runtime
GET  /api/scene/snapshot     — Render 3D viewport as JPEG/PNG image
```

### Direct Control (Intervention)
```
POST /api/direct/execute     — Run a temporary subtree (IDLE requires explicit human-approved manual intervention)
GET  /api/direct/status      — Current/last intervention session status
POST /api/direct/heartbeat   — Renew intervention lease
POST /api/direct/abort       — Abort a running intervention
```

Current authority rule:
- Public ad-hoc direct control from `IDLE` is allowed only for explicitly
  human-approved `manual_intervention` sessions.
- Public `delegated_recovery` entry is allowed only when a pending delegated
  recovery request already exists from an explicit node policy hook.
- `program_only` delegated recovery is still reserved until an internal
  executor exists.

### Runtime Overrides
```
GET    /api/program/runtime-overrides            — Active/staged override ledger
GET    /api/program/runtime-overrides/materialized-tree — Preview materialized override tree
POST   /api/program/runtime-overrides            — Apply or stage one node-level runtime override
POST   /api/program/runtime-overrides/activate   — Activate staged overrides from IDLE
POST   /api/program/runtime-overrides/promote    — Materialize into loaded program or save to disk
DELETE /api/program/runtime-overrides/{nodeName} — Remove one override
POST   /api/program/runtime-overrides/clear      — Clear all overrides
```

Current override rules:
- Runtime overrides are parameter patches on a specific node instance name, not
  arbitrary tree rewrites.
- They are allowed only in `IDLE` and `DIRECT_CONTROL`.
- `STOPPED` may hold staged overrides, but they cannot take effect until reset.
- Active runtime overrides change what `GET /api/program/tree` returns and what
  the runner/HMI execute, but they do not silently modify the saved artifact.
- Promotion is explicit:
  - preview first with `materialized-tree`,
  - then either promote into the loaded in-memory program or save a materialized
    artifact with persistent-edit policy checks.
- The node instance must explicitly allow runtime overrides through
  `modification_policy.runtime_override`.

### Delegated Recovery
```
GET  /api/direct/pending-recovery — Structured pending delegated-recovery request
```

Current delegated-recovery behavior:
- A node with `modification_policy.intervention_delegate` may stop the program
  and raise a pending recovery request when it fails.
- The pending request includes the triggering node, trigger reason, allowed
  authorities, allowed actions, and allowed resume targets.
- Starting a `delegated_recovery` session without such a pending request is
  rejected.

### Infrastructure
```
GET  /api/health             — Server health check
GET  /api/files/browse       — Browse allowlisted backend filesystem roots
WS   /ws/state               — Real-time state broadcast (JSON frames)
```

---

## 2. Composition Rules

### Always Follow

1. **Validate before applying**: Call `POST /api/program/validate` with your
   candidate tree before `PUT /api/program/tree`. This catches structural errors,
   unknown node IDs, and parameter constraint violations without side effects.

2. **`reset_trajectories` after scene changes**: Any node that modifies the
   collision scene (`attach_object_to_link`, `detach_object_from_link`,
   `add_collision_object`, `remove_collision_object`) invalidates cached
   trajectories. Always follow scene-changing sequences with `reset_trajectories`.

3. **Approach before tight moves**: Never plan directly to a grasp or place pose.
   First move to an approach position offset from the target (typically +Z), then
   descend. This gives the planner collision-free intermediate configurations.

4. **Use `relative_move` for object-relative positioning**: When the target
   depends on a scene object's pose, use `motion_type: relative_move` with
   `target_reference: {type: object, id: "..."}`. The planner resolves the
   object's current pose at planning time, so this works for dynamic scenes.

5. **Use `joint_goal` for known configurations**: Home, rest, stow positions
   should use `motion_type: joint_goal` with explicit joint values. These are
   deterministic and cache-friendly.

6. **Parameter substitution**: Use `"${param:name}"` syntax to reference program
   parameters. The runtime substitutes these from the `parameters:` section at
   tree build time.

### Never Do

- Never compose a tree with unknown `id` values — check `GET /api/catalog` first.
- Never skip `reset_trajectories` after attach/detach — old trajectories will
  collide with the newly attached geometry.
- Never set `velocity_scale` to 0.0 at the move level — the robot will not move.
  Use `POST /api/cycle/stop` to pause execution instead.
- Never modify the tree while the cycle is RUNNING — `PUT /api/program/tree`
  returns 409. Stop or reset first.
- Never assume a staged runtime override is already active — check
  `GET /api/program/runtime-overrides` or the execution digest first.

---

## 3. Composition Patterns

### Pattern: Pick an Object

Pick an object from the scene using approach/descend/grasp/attach/retreat.

```yaml
# Source: tests/fixtures/pick_and_place.program.yaml (phases 1-5)
# Skill: GET /api/skills → pick_object

id: sequence
name: pick_graspable
children:
  - id: move_manipulator_action
    name: approach_graspable
    params:
      move_id: pick_approach_graspable
      motion_type: relative_move
      relative_move:
        target_reference: {type: object, id: graspable}
        offset: {position_m: [0.0, 0.0, 0.05]}
      velocity_scale: 0.5

  - id: move_manipulator_action
    name: descend_to_graspable
    params:
      move_id: pick_descend_graspable
      motion_type: relative_move
      relative_move:
        target_reference: {type: object, id: graspable}
        offset: {position_m: [0.0, 0.0, 0.0]}
      velocity_scale: 0.5

  - id: set_key_bool_value
    name: close_gripper
    params: {key: gripper_closed, value: true}

  - id: attach_object_to_link
    name: attach_graspable
    params: {object_id: graspable, link_frame: tool0}

  - id: move_manipulator_action
    name: retreat_graspable
    params:
      move_id: pick_retreat_graspable
      motion_type: relative_move
      relative_move:
        target_reference: {type: robot_link, id: tool0}
        offset: {position_m: [0.0, 0.0, 0.05]}
      velocity_scale: 0.5

  - id: reset_trajectories
    name: invalidate_after_pick
```

### Pattern: Place an Object

Place an attached object at a target pose.

```yaml
# Skill: GET /api/skills → place_object

id: sequence
name: place_graspable
children:
  - id: move_manipulator_action
    name: approach_place
    params:
      move_id: place_approach
      motion_type: pose_goal
      pose_goal:
        position_m: [0.3, 0.3, 0.20]          # target + approach_height
        orientation_xyzw: [0.0, 0.0, 0.0, 1.0]
      velocity_scale: 0.5

  - id: move_manipulator_action
    name: descend_place
    params:
      move_id: place_descend
      motion_type: pose_goal
      pose_goal:
        position_m: [0.3, 0.3, 0.15]          # actual target
        orientation_xyzw: [0.0, 0.0, 0.0, 1.0]
      velocity_scale: 0.5

  - id: set_key_bool_value
    name: open_gripper
    params: {key: gripper_closed, value: false}

  - id: detach_object_from_link
    name: detach_graspable
    params: {object_id: graspable, link_frame: tool0}

  - id: move_manipulator_action
    name: retreat_place
    params:
      move_id: place_retreat
      motion_type: relative_move
      relative_move:
        target_reference: {type: robot_link, id: tool0}
        offset: {position_m: [0.0, 0.0, 0.05]}
      velocity_scale: 0.5

  - id: reset_trajectories
    name: invalidate_after_place
```

### Pattern: Move to Named Configuration

```yaml
# Skill: GET /api/skills → move_to_named_config

id: move_manipulator_action
name: move_to_home
params:
  move_id: move_to_home
  motion_type: joint_goal
  joint_goal_names: [shoulder_pan_joint, shoulder_lift_joint, elbow_joint,
                     wrist_1_joint, wrist_2_joint, wrist_3_joint]
  joint_goal_positions_rad: [0.0, -1.5708, 1.5708, -1.5708, -1.5708, 0.0]
  velocity_scale: 0.3
```

### Pattern: Conditional Retry with Fallback

```yaml
id: fallback
name: robust_pick
children:
  - id: sequence
    name: try_primary
    children:
      - id: check_key_bool_value
        name: check_sensor_ready
        params: {key: sensor_ready, expected_value: true}
      - id: move_manipulator_action
        name: primary_approach
        params:
          move_id: primary_approach
          motion_type: relative_move
          relative_move:
            target_reference: {type: object, id: graspable}
            offset: {position_m: [0.0, 0.0, 0.05]}
          velocity_scale: 0.5
  - id: retry
    name: retry_alternative
    children:
      - id: sequence
        name: retry_body
        children:
          - id: timer
            name: wait_before_retry
            params: {duration_s: 1.0}
          - id: move_manipulator_action
            name: alternative_approach
            params:
              move_id: alt_approach
              motion_type: joint_goal
              joint_goal_names: [j1, j2, j3, j4, j5, j6]
              joint_goal_positions_rad: [0.1, -1.2, 1.0, -1.5, -1.3, 0.4]
              velocity_scale: 0.3
    params: {num_attempts: 3}
```

### Pattern: Continuous Loop

```yaml
id: repeat
name: continuous_cycle
children:
  - id: sequence
    name: cycle_body
    children:
      - # ... pick sequence
      - # ... place sequence
      - # ... return home
params: {num_cycles: -1}   # -1 = infinite
```

---

## 4. Monitoring Workflow

### Polling Loop (recommended for LLM agents)

```
1. POST /api/cycle/run
2. Loop every 2-5 seconds:
   a. GET /api/execution/digest
   b. Check "state" field:
      - "running" → continue polling
      - "idle" → program completed (check "error" for failures)
      - "stopped" → execution was explicitly stopped; reset or resume
      - "direct_control" → an intervention subtree is executing
   c. If error is not null → investigate:
      - GET /api/execution/history for event timeline
      - GET /api/kernel/snapshot for kernel state
      - GET /api/debug/export for full trace bundle
3. On completion: GET /api/scene/state to verify final world state
```

### Error Recovery

When execution fails:
1. `GET /api/execution/digest` — check `error` field
2. `GET /api/execution/history` — find the failing event
3. `GET /api/scene/state` — check object positions
4. `POST /api/cycle/reset` — return to IDLE
5. Fix the tree or scene, then re-run

Common failure modes:
- **Planning timeout**: The planner could not find a collision-free path within
  the timeout. Try: different approach pose, remove obstacles, increase timeout.
- **Execution timeout**: The robot did not reach the goal within the execution
  timeout. Try: increase timeout, reduce distance, check for obstruction.
- **Collision detected**: A planned trajectory intersects a collision object.
  Try: ensure `reset_trajectories` was called after scene changes, check object
  poses are correct.

---

## 5. Node Catalog Summary

Retrieve the full catalog via `GET /api/catalog`. Key nodes for composition:

| Node ID | Category | Purpose |
|---------|----------|---------|
| `sequence` | control_flow | Execute children left-to-right, fail on first failure |
| `fallback` | control_flow | Try children left-to-right, succeed on first success |
| `parallel` | control_flow | Execute all children simultaneously |
| `repeat` | control_flow | Repeat child N times (-1 = infinite) |
| `retry` | control_flow | Retry child on failure up to N times |
| `inverter` | control_flow | Invert child result |
| `timer` | control_flow | Wait for duration_s seconds |
| `move_manipulator_action` | motion | Plan + validate + execute a motion |
| `reset_trajectories` | motion | Invalidate trajectory cache |
| `add_collision_object` | scene | Add box/sphere to collision scene |
| `remove_collision_object` | scene | Remove object from collision scene |
| `update_collision_object_pose` | scene | Move an existing object |
| `attach_object_to_link` | scene | Attach object to robot link (TCP) |
| `detach_object_from_link` | scene | Detach object from robot link |
| `set_key_bool_value` | logic | Set a blackboard boolean |
| `check_key_bool_value` | logic | Condition: check blackboard boolean |
| `wait_for_key_bool` | logic | Block until blackboard boolean matches |
| `get_link_pose` | geometry | Query FK and store pose on blackboard |
| `check_pose_distance` | geometry | Condition: check distance between poses |

---

## 6. Motion Type Reference

### `joint_goal`
Move to explicit joint positions. Deterministic, cache-friendly.
```yaml
motion_type: joint_goal
joint_goal_names: [j1, j2, j3, j4, j5, j6]
joint_goal_positions_rad: [0.0, -1.57, 1.57, -1.57, -1.57, 0.0]
```

### `pose_goal`
Move TCP to a Cartesian pose in the base frame.
```yaml
motion_type: pose_goal
pose_goal:
  position_m: [0.3, -0.3, 0.25]
  orientation_xyzw: [0.707, -0.707, 0.0, 0.0]
```

### `relative_move`
Move relative to a reference (object, frame, or current TCP).
```yaml
motion_type: relative_move
relative_move:
  target_reference: {type: object, id: graspable}      # resolve object pose
  offset: {position_m: [0.0, 0.0, 0.05]}               # 5cm above object
```

Reference types:
- `object` — resolve the named scene object's pose at planning time
- `frame` — use a named scene frame
- `robot_link` — use the current FK pose of a link (e.g. `tool0` for TCP-relative)
- `pose` — use an inline pose expressed in base_frame

---

## 7. Skills vs Raw Nodes

**Prefer skills** (`GET /api/skills`) for standard operations like pick/place.
Skills are validated subtree templates with documented pre/postconditions.

**Use raw nodes** when:
- The skill catalog doesn't cover your use case
- You need fine-grained control over individual moves
- You're composing a novel sequence not representable as a skill

Skills expand to the same catalog nodes — they're a convenience layer, not a
separate runtime. An LLM can inspect a skill's `body` field (or its
`expandsTo` alias) to understand exactly what nodes it produces.
