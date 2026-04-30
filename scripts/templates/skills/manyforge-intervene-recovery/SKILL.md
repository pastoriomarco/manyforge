---
name: manyforge-intervene-recovery
description: Attempt to recover a failed ManyForge execution by applying scene edits, runtime overrides, or intervention subtrees.
user-invocable: true
---

# Intervene and Recover ManyForge Execution

Apply a recovery action to fix a failed program execution and resume.

## When to use

- After diagnosing a failure (use `manyforge-diagnose-failure` first)
- When the diagnosis suggests a recoverable failure
- When a delegated recovery request is pending

## Prerequisites

- Program must be in `stopped` or `idle` state, depending on recovery action
- Failure must be diagnosed (know the cause)
- Recovery attempt count must be under 3
- Runtime overrides can only be changed from `idle` or `direct_control`; reset
  a stopped cycle to `idle` before applying an override.

## Recovery strategies

### Strategy A: Scene edit

Use when objects are missing, misplaced, or blocking the path.

```
1. manyforge_scene_edit(operation="add"|"remove"|"update_pose",
      object_id="...", ...)
2. manyforge_cycle_control(command="reset")
3. manyforge_cycle_control(command="run")
4. Monitor with manyforge_execution_status(include="digest")
```

**Caution**: Never remove objects from the original scene. Only add
temporary objects or adjust poses of objects you understand.

### Strategy B: Runtime override

Use when node parameters need adjustment (wrong target pose, speed, etc).

```
1. manyforge_cycle_control(command="status")
2. manyforge_cycle_control(command="reset")  # if currently stopped
3. manyforge_runtime_override(action="apply",
      node_name="<failed_node>",
      params={"<param>": <new_value>},
      authority_source="llm",
      approval_mode="not_required",
      reason="<diagnosis summary>")
4. manyforge_runtime_override(action="status")
5. manyforge_cycle_control(command="run")
6. Monitor with manyforge_execution_status(include="digest")
```

### Strategy C: Intervention subtree

Use when the failure requires running a custom recovery sequence.

```
1. Build a recovery subtree using manyforge_catalog() and
   manyforge_skills() to find appropriate nodes
2. manyforge_program_validate(tree=<recovery_tree>)
3. manyforge_intervention(action="execute",
      tree=<recovery_tree>,
      principal_id="llm:openclaw-agent",
      grant_type="policy_granted",
      trigger_reason="<from diagnosis>")
4. Poll manyforge_intervention(action="status") until complete
5. manyforge_execution_status(include="digest")
6. Resume with reset/run only if the reported state and recovery policy allow it
```

## Safety rules

1. **Max 3 recovery attempts** — if 3 attempts fail, escalate to human
2. **Save before persistent edits** — call `manyforge_program_save()` before
   intentional saved tree changes
3. **Validate before executing** — always validate intervention trees
4. **Monitor after recovery** — watch at least 2 digest polls after resume
5. **Don't remove original scene objects** — only add or adjust
6. **Identify yourself** — always use `principal_id="llm:openclaw-agent"`

## Output format

Report each recovery attempt as:

```
## Recovery Attempt <N>/3

**Strategy**: <A/B/C>
**Action**: <what was done>
**Result**: <SUCCESS/FAILURE>
**New state**: <running/stopped/idle>
```
