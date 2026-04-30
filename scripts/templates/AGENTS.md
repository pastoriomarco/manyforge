> Runtime template. When editing this file in the repository, follow
> [`../../AGENTS.md`](../../AGENTS.md) and [`../../docs/INDEX.md`](../../docs/INDEX.md).

# ManyForge Runtime Agent Reference

This file is copied into ManyForge/OpenClaw MCP runtime sandboxes for
downstream LLM agents. It is not the ManyForge repository development workflow.

At runtime, the live MCP tool schemas, tool results, and current session state
are the immediate authority. If they contradict this file, use safe read-only
inspection tools and report the mismatch before taking state-changing actions.

You are interacting with **ManyForge**, a behavior-tree execution engine for
robotic manipulation. Programs are YAML-defined behavior trees that compose
deterministic nodes and optional model-query nodes. Programs run against a
deployment that describes robot hardware, adapters, capabilities, model
endpoints, and audit policy.

## Architecture

```
Program (.program.yaml)
  └─ Behavior Tree (nodes from catalog)
Deployment (.deployment.yaml)
  └─ Robot adapter, capabilities, model endpoints
CycleManager
  └─ Runtime states: idle, running, stopped, interrupt_handler, direct_control
Kernel
  └─ Execution state, validation, cache, collision monitoring, motion backend
```

- **Tick cadence**: Runtime-owned; do not assume a fixed rate unless the loaded
  deployment or runtime status reports one.
- **Execution**: CycleManager ticks the behavior tree. Nodes return SUCCESS,
  FAILURE, or RUNNING each tick.
- **Motion acknowledgement**: The runtime owns successful kernel terminal
  acknowledgement. Authored behavior-tree nodes must not auto-reset or
  acknowledge completed kernel executions. A successful acknowledgement is bound
  to the exact completed kernel `execution_id`; failed or stopped kernel
  executions require reset or recovery authority.
- **Runtime overrides**: Parameter changes applied without reloading the program.
  Staged overrides activate only from `idle`; do not resume a stopped cycle
  against a different effective program.
- **Interventions**: One-shot subtrees that run without modifying the saved
  program. Used for recovery and diagnostics.

## Lifecycle Commands

| Command | From state | To state | Notes |
|---------|-----------|----------|-------|
| `status` | any | unchanged | Inspect state before acting |
| `run` | `idle` | `running` | Activates staged overrides if present |
| `run` | `stopped` | `running` | Resumes without activating staged overrides |
| `stop` | `running` | `stopped` | Requests a halt/quiesce |
| `reset` | `running`/`stopped`/`idle` | `idle` | Rebuilds cycle state; staged overrides require `idle` |
| `velocity` | any | unchanged | Scales execution speed (0.0–1.0) |

## MCP Tools

The runtime currently exposes 19 tools. Prefer the live `tools/list` schema
when exact parameters matter.

### Scene

**`manyforge_scene_state()`**
Get robot joint positions, TCP pose, and all collision objects.
Returns: `{robot: {joints, tcp_pose, robot_id, group_id}, objects: [...], timestamp_ns}`

**`manyforge_scene_edit(operation, object_id, ...)`**
Edit the collision scene. Not allowed while cycle state is `running`.
- `operation`: "add", "remove", "update_pose", "attach", "detach"
- For "add": provide `shape_type` ("box"/"sphere"), dimensions, and pose
- For "attach"/"detach": provide `frame_id` (robot link)
Returns: `{success, error, timestamp_ns}`

**`manyforge_scene_snapshot(width=640, height=480, format="jpeg")`**
Render the 3D scene as a base64-encoded image. Requires HMI server.
Returns: `{image_base64, width, height, format, timestamp_ns}`

### Program

**`manyforge_program_load(program_path, deployment_path=None, force_discard_overrides=False)`**
Load a program (and optional deployment) from YAML files.
Paths must be within allowed filesystem roots.
Returns: `{name, description, robot: {...}, scene_count, parameter_count, source_path}`

**`manyforge_program_tree(action="read", tree=None, force_discard_overrides=False)`**
Read or replace the behavior tree.
- `action="read"`: returns current tree
- `action="write"`: replaces tree (validate first!)
Cannot write while cycle state is `running`.

**`manyforge_program_validate(tree)`**
Dry-run validation of a candidate tree. No side effects.
**Always call this before `manyforge_program_tree(action="write")`.**
Returns: `{valid, errors: [{path, message}], warnings: [{path, message}]}`

**`manyforge_program_save(path=None)`**
Save the loaded program to YAML. Defaults to original source path.

### Execution

**`manyforge_cycle_control(command, velocity_scale=None)`**
Control execution: "run", "stop", "reset", "status", "velocity".
Returned cycle states include `idle`, `running`, `stopped`,
`interrupt_handler`, and `direct_control`. Kernel terminal acknowledgement is
runtime-owned and is not a behavior-tree authoring action.

**`manyforge_execution_status(include="digest")`**
Get execution information.
- `"digest"`: state, current node, progress, errors, warnings
- `"history"`: event log for current/last run
- `"blackboard"`: py_trees blackboard key-value state
- `"all"`: everything combined

**`manyforge_debug_export(run_id=None)`**
Full diagnostic bundle: kernel snapshot, metrics, execution state,
scene trace, blackboard, node statuses. Use for post-mortem analysis.

### Runtime Overrides

**`manyforge_runtime_override(action, node_name=None, params=None, ...)`**
Inspect or mutate the runtime override ledger.
- `"status"`: list active/staged/stale overrides
- `"apply"`: stage a parameter override for a node instance
- `"activate"`: activate staged overrides from `idle`
- `"promote"`: materialize overrides into the loaded program or save to disk
- `"remove"`: remove one override
- `"clear"`: clear all overrides
- `"revalidate"`: re-check stale overrides against current program
- `"materialized_tree"`: preview the tree with overrides applied

### Intervention

**`manyforge_intervention(action, tree=None, principal_id=None, ...)`**
Execute or manage a temporary intervention subtree.
- `"execute"`: run a validated one-shot subtree (requires `principal_id`)
- `"status"`: check intervention session state
- `"pending_recovery"`: get delegated recovery request details
- `"abort"`: cancel a running intervention
- `"heartbeat"`: renew intervention lease
- `"promote"`: promote intervention values to the main program

### Discovery

**`manyforge_catalog(entry_id=None)`**
Query the behavior tree node catalog. Returns node types with schemas,
parameters, children constraints, and required capabilities.

**`manyforge_capabilities()`**
Query deployment-declared capabilities grouped by category.

**`manyforge_skills()`**
Get the skill catalog: validated subtree templates for standard operations
(pick, place, move, etc.), including capability and assistant-tool
requirements. Prefer skills over raw nodes.

**`manyforge_deployment()`**
Get the loaded deployment configuration: adapters, capabilities,
the optional model endpoint profile registry reference, resolved model
endpoints with declared model capabilities, assistant config, assistant modes,
tool catalog, prompt templates, audit policy.

### Kernel & Session

**`manyforge_kernel(include="snapshot")`**
Get kernel execution state and metrics.
- `"snapshot"`: current execution state (joint positions, active move, progress)
- `"metrics"`: accumulated counters (moves executed, planning time, etc.)
- `"all"`: both combined

**`manyforge_session(action="status")`**
Manage the composer session.
- `"status"`: current session info (loaded program, deployment, overrides, events)
- `"reset"`: reset cycle state and clear runtime overrides while keeping the
  loaded program; not allowed while execution or an interrupt handler is active

### Filesystem

**`manyforge_files(path=None, kind="program")`**
Browse files on disk. Filter by `kind`: "program" or "deployment".
Returns: `{path, parent_path, entries: [{name, path, type}], root_shortcuts}`

## Workflows

### Load and run a program

```
1. manyforge_files(kind="program")           # find .program.yaml files
2. manyforge_program_load(program_path=...,  # load program + deployment
      deployment_path=...)
3. manyforge_cycle_control(command="status") # confirm idle before running
4. manyforge_cycle_control(command="run")    # start execution
5. manyforge_execution_status(include="digest")  # poll for progress
```

### Edit a program safely

```
1. manyforge_cycle_control(command="status") # stop/reset first if running
2. manyforge_program_tree(action="read")     # inspect current tree
3. manyforge_program_validate(tree=...)      # validate candidate tree
4. manyforge_program_tree(action="write", tree=...)
5. manyforge_program_save()                  # persist only when intended
```

### Stop and diagnose

```
1. manyforge_cycle_control(command="stop")   # halt execution
2. manyforge_execution_status(include="all") # full state dump
3. manyforge_debug_export()                  # diagnostic bundle
4. manyforge_scene_state()                   # current scene
```

### Apply a runtime override

```
1. manyforge_cycle_control(command="status")
2. manyforge_cycle_control(command="stop")   # only if currently running
3. manyforge_cycle_control(command="reset")  # only if not already idle
4. manyforge_runtime_override(action="apply",
      node_name="pick_object_1",
      params={"target_pose": [0.5, 0.0, 0.1, 0, 0, 0, 1]})
5. manyforge_runtime_override(action="status") # confirm staged override
6. manyforge_cycle_control(command="run")    # activates staged override from idle
```

### Recovery via intervention

```
1. manyforge_intervention(action="pending_recovery")  # check if recovery needed
2. manyforge_scene_state()                            # assess current scene
3. manyforge_intervention(action="execute",
      tree={...recovery subtree...},
      principal_id="llm:openclaw-agent",
      grant_type="policy_granted",
      trigger_reason="node_failure")
4. manyforge_intervention(action="status")            # poll until complete
5. manyforge_execution_status(include="digest")       # inspect recovery outcome
6. manyforge_cycle_control(command="reset")           # or run, only if status allows it
```

## Safety Rules

1. **Never modify the tree while cycle state is `running`.** Stop first, then edit.
2. **Always validate before writing a tree.** Call `manyforge_program_validate`
   before `manyforge_program_tree(action="write")`.
3. **Scene edits are blocked while cycle state is `running`.** Stop execution first.
4. **Velocity 0.0 stalls the robot — use "stop" to pause.**
5. **Runtime overrides are staged, not immediate.** They activate on the next
   run/reset only from `idle`. Check `manyforge_runtime_override(action="status")`
   before resuming or rerunning.
6. **Interventions require a principal_id.** Use your agent identity
   (e.g., `"llm:openclaw-agent"`).
7. **Do not run concurrent interventions.** Only one intervention session at a time.
8. **Poll execution status, don't assume completion.** Use
   `manyforge_execution_status(include="digest")` to check progress.
9. **When in doubt, stop first.** Then inspect status and follow the runtime
   recovery/reset path for the reported state.
10. **Save before making destructive changes.** Use `manyforge_program_save()`
    before replacing the tree or clearing overrides.
11. **Do not fake terminal acknowledgement.** Successful kernel terminal
    acknowledgement is runtime-owned and bound to the completed execution ID;
    failed or stopped kernel executions require reset or recovery authority.
12. **Treat model output as advisory.** Validate proposed trees and inspect
    runtime state before applying, running, or saving changes.
