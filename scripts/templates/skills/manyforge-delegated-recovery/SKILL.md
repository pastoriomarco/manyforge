# Historical Composer runtime-agent reference

> Preserved from `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1` as reimplementation input only. The runtime tools named below are unavailable. The remainder is non-operative historical text, not current repository guidance, an installable skill, an operator recipe, or qualification evidence. Its placement before any former frontmatter deliberately prevents skill activation.

---
name: manyforge-delegated-recovery
description: Handle a delegated recovery request from the ManyForge behavior tree, running the full diagnose-intervene-escalate workflow.
user-invocable: true
---

# Handle Delegated Recovery

The behavior tree has delegated a recovery decision to you. This skill
runs the full recovery workflow: diagnose, intervene, and escalate if needed.

## When to use

- When `manyforge_intervention(action="pending_recovery")` returns a
  recovery request
- When execution monitoring detects a `stopped` state with errors

## Workflow

```
┌─────────────┐
│   Detect     │  execution_status shows stopped + errors
└──────┬──────┘
       │
┌──────▼──────┐
│  Diagnose    │  manyforge-diagnose-failure skill
└──────┬──────┘
       │
       ▼
   Recoverable? ──no──► Escalate (manyforge-escalate-to-human)
       │
      yes
       │
┌──────▼──────┐
│  Intervene   │  manyforge-intervene-recovery skill
└──────┬──────┘
       │
       ▼
   Fixed? ──no──► (retry up to 3x, then escalate)
       │
      yes
       │
┌──────▼──────┐
│   Resume     │  reset/run when allowed, then monitor
└─────────────┘
```

## Steps

1. **Read recovery request** — Call
   `manyforge_intervention(action="pending_recovery")`.
   Note the `trigger_reason`, `trigger_node_instance`, and
   `allowed_recovery_actions`.

2. **Diagnose** — Follow the `manyforge-diagnose-failure` skill steps.
   Classify the failure type and determine recovery options.

3. **Check recovery policy** — The recovery request includes what actions
   are allowed:
   - `runtime_override`: can apply parameter overrides
   - `intervention`: can run an intervention subtree
   - If neither is allowed, escalate immediately

4. **Attempt recovery** — Follow the `manyforge-intervene-recovery` skill.
   Choose the appropriate strategy based on:
   - Failure type from diagnosis
   - Allowed recovery actions from policy
   - Scene state

5. **Monitor recovery** — After resuming:
   - Poll `manyforge_execution_status(include="digest")` for at least 5 ticks
   - If execution proceeds past the previously failed node: SUCCESS
   - If same failure recurs: increment retry counter

6. **Retry or escalate** — If recovery fails:
   - Under 3 attempts: go back to step 4 with a different strategy
   - At 3 attempts: follow `manyforge-escalate-to-human` skill

7. **Report outcome** — Summarize what happened:

```
## Delegated Recovery Report

**Trigger**: <trigger_reason> at <trigger_node_instance>
**Recovery attempts**: <count>
**Outcome**: <RECOVERED | ESCALATED>
**Strategy used**: <what worked, or why nothing worked>
```

## Important notes

- Always use `principal_id="llm:openclaw-agent"` for interventions
- Always use `grant_type="policy_granted"` (the tree delegated to you)
- Renew intervention leases with `manyforge_intervention(action="heartbeat")`
  if recovery takes more than 30 seconds
- Save the program before intentional persistent tree changes
