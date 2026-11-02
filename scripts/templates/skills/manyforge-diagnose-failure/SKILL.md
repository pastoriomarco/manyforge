# Historical Composer runtime-agent reference

> Preserved from `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1` as reimplementation input only. The runtime tools named below are unavailable. The remainder is non-operative historical text, not current repository guidance, an installable skill, an operator recipe, or qualification evidence. Its placement before any former frontmatter deliberately prevents skill activation.

---
name: manyforge-diagnose-failure
description: Diagnose why a ManyForge program stopped or failed, producing a structured failure report.
user-invocable: true
---

# Diagnose ManyForge Failure

Investigate a stopped or failed ManyForge program execution and produce
a structured diagnosis.

## When to use

- Program transitioned to `stopped` unexpectedly
- Execution status shows errors
- After the monitoring skill reports a failure

## Steps

1. **Confirm stopped state** — Call `manyforge_cycle_control(command="status")`.
   If still `running`, stop first with `manyforge_cycle_control(command="stop")`.

2. **Get execution digest** — Call `manyforge_execution_status(include="all")`.
   Extract:
   - `state`: should be "stopped"
   - `errors`: list of error messages
   - `current_node`: the node that was active when failure occurred
   - `completed_nodes` / `remaining_nodes`: progress at failure time

3. **Get scene state** — Call `manyforge_scene_state()`.
   Check:
   - Robot position: is it in an expected location?
   - Objects: are all expected objects still present?
   - Attachments: is anything attached/detached unexpectedly?

4. **Check for delegated recovery** — Call
   `manyforge_intervention(action="pending_recovery")`.
   If a recovery request exists, note:
   - `trigger_reason`: why recovery was requested
   - `trigger_node_instance`: which node triggered it
   - `allowed_recovery_actions`: what the program allows

5. **Export debug bundle** — Call `manyforge_debug_export()`.
   This captures the full diagnostic state for the report.

6. **Produce diagnosis** — Classify the failure:

   | Failure type | Indicators | Typical recovery |
   |-------------|------------|------------------|
   | `planning_failed` | Motion planner couldn't find path | Adjust target pose or remove obstacles |
   | `validation_failed` | Tree validation errors | Fix tree structure |
   | `execute_failed` | Execution error in kernel | Check scene state, retry |
   | `timeout` | Node ticked too long | Increase timeout or simplify task |
   | `scene_mismatch` | Expected objects missing | Restore scene objects |
   | `node_failure` | Behavior node returned FAILURE | Check node params |
   | `precondition_failed` | Precondition check failed | Fix preconditions |

## Output format

Report the diagnosis as:

```
## Failure Diagnosis

**State**: stopped
**Failed node**: <node_name> (<node_type>)
**Failure type**: <classification>
**Error**: <error message>

### Scene at failure
- Robot TCP: [x, y, z]
- Objects: <count> (<list>)
- Attachments: <any>

### Recovery options
- <option 1>
- <option 2>

### Recommended action
<what to do next>
```
