---
name: manyforge-monitor-execution
description: Monitor a running ManyForge program, polling execution status and reporting progress until completion or failure.
user-invocable: true
---

# Monitor ManyForge Execution

Monitor a running ManyForge behavior tree program. Poll execution status at
regular intervals, track node progress, and report completion or failure.

## When to use

- After starting a program with `manyforge_cycle_control(command="run")`
- When asked to watch a running program
- During recovery to verify a fix worked

## Steps

1. **Check current state** — Call `manyforge_cycle_control(command="status")`
   to confirm the program is `running`. If `idle`, report that nothing is
   running. If `stopped`, report the stopped state and check for errors.

2. **Poll execution digest** — Call
   `manyforge_execution_status(include="digest")` every 3–5 seconds.
   The digest contains:
   - `state`: current cycle state (idle/running/stopped)
   - `current_node`: the node being ticked
   - `completed_nodes` / `remaining_nodes`: progress tracking
   - `errors` / `warnings`: any issues

3. **Track progress** — Report significant state changes:
   - New node transitions (current_node changed)
   - Errors or warnings appearing
   - State transitions (`running` -> `stopped`)

4. **On completion (`idle`)** — The program finished successfully.
   Report the final status and any notable events from the run.

5. **On failure (`stopped` with errors)** — The program stopped due to an error.
   - Report the error details from the digest
   - Call `manyforge_scene_state()` to capture the current scene
   - Check `manyforge_intervention(action="pending_recovery")` for
     recovery requests
   - Suggest next steps: manual stop, runtime override, or recovery

## Tool invocations

Use the `manyforge_*` tools exposed to the OpenClaw agent by the ManyForge
workspace plugin:
- `manyforge_cycle_control(command="status")`
- `manyforge_execution_status(include="digest")`
- `manyforge_scene_state()`
- `manyforge_intervention(action="pending_recovery")`

The legacy `/sandbox/.manyforge/manyforge-mcp-bridge.py` script may still be
present for diagnostics, but it is not the primary agent integration path.

## Example monitoring output

```
Monitoring execution...
  [0s]  State: running | Node: sequence_root | Progress: 0/5
  [3s]  State: running | Node: move_to_pick  | Progress: 1/5
  [6s]  State: running | Node: close_gripper | Progress: 2/5
  [9s]  State: running | Node: move_to_place | Progress: 3/5
  [12s] State: running | Node: open_gripper  | Progress: 4/5
  [15s] State: idle    | Complete            | Progress: 5/5
Program completed successfully in ~15s.
```

## Failure example

```
Monitoring execution...
  [0s]  State: running | Node: sequence_root | Progress: 0/5
  [3s]  State: running | Node: move_to_pick  | Progress: 1/5
  [6s]  State: stopped | Error: planning_failed at move_to_pick
  
Execution stopped. Error: motion planner failed to find a valid path.
Scene state: robot at [0.3, 0.0, 0.5], no attached objects.
Recovery request: pending (trigger=planning_failed, node=move_to_pick)
Suggested action: check for collision objects blocking the path, or
apply a runtime override to adjust the target pose.
```
