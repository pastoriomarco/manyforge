# Historical Composer runtime-agent reference

> Preserved from `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1` as reimplementation input only. The runtime tools named below are unavailable. The remainder is non-operative historical text, not current repository guidance, an installable skill, an operator recipe, or qualification evidence. Its placement before any former frontmatter deliberately prevents skill activation.

---
name: manyforge-escalate-to-human
description: Produce a structured escalation report when automated recovery fails or the situation requires human judgment.
user-invocable: true
---

# Escalate to Human Operator

When automated recovery fails or the situation is beyond autonomous
resolution, produce a structured escalation report for the human operator.

## When to use

- After 3 failed recovery attempts
- When the failure type is unknown or novel
- When the diagnosis suggests physical intervention is needed
- When safety boundaries would be violated by autonomous action
- When `recovery_policy.allow_intervention` is false for the failed node

## Steps

1. **Gather full state** — Collect all available diagnostic information:
   - `manyforge_cycle_control(command="status")`
   - `manyforge_execution_status(include="all")`
   - `manyforge_scene_state()`
   - `manyforge_debug_export()`

2. **Ensure safe state** — Make sure the robot is stopped:
   - If `running`: `manyforge_cycle_control(command="stop")`
   - Verify velocity is 0

3. **Produce escalation report** — Format as below

## Escalation report format

```
## ESCALATION: ManyForge Execution Failure

**Severity**: <CRITICAL | HIGH | MEDIUM>
**Time**: <timestamp>
**Program**: <program name>
**Failed node**: <node name> (<node type>)

### Situation
<1-2 sentence summary of what happened>

### Diagnosis
<failure type and root cause analysis>

### Recovery attempts
<list of what was tried and why it failed>

### Current state
- Cycle: stopped
- Robot TCP: [x, y, z]
- Scene objects: <count>
- Attached objects: <any>

### Recommended human action
<what the operator should do>

### Debug bundle
<debug export reference or key findings>
```

## Severity classification

- **CRITICAL**: Robot may be in contact with objects, potential collision,
  or hardware fault indicators
- **HIGH**: Execution failed repeatedly, program cannot proceed without
  human judgment
- **MEDIUM**: Single failure that might resolve with manual parameter
  adjustment or scene correction
