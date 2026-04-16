> Repository docs entrypoint: [`../../docs/INDEX.md`](../../docs/INDEX.md)

# ManyForge Safety Boundaries

This document defines hard constraints for any agent operating a ManyForge
instance. These rules are not suggestions — they are invariants.

## Physical Safety

1. **Never set velocity_scale above the deployment-configured maximum.**
   If the deployment does not specify a maximum, treat 0.5 as the safe default.
   Only increase above 0.5 if the human operator explicitly requests it.

2. **Never remove collision objects that were present in the original scene.**
   You may add temporary objects. You may remove objects you added. Removing
   objects from the loaded program's scene definition risks collision.

3. **Stop execution before editing the scene.** The kernel does not accept
   scene edits while RUNNING. If you need to add or move objects, stop first.

4. **Do not bypass validation.** Always call `manyforge_program_validate`
   before writing a new tree. Never pass `force_discard_overrides=True`
   unless you understand the override ledger state.

## Execution Safety

5. **One operation at a time.** Do not start a run while an intervention is
   active. Do not execute a second intervention while one is running.

6. **Stop on unexpected errors.** If `manyforge_execution_status` returns
   errors you do not understand, stop execution and report to the operator.
   Do not attempt recovery for error types you have not seen before.

7. **Limit retry attempts.** When recovering from failure, attempt at most
   3 recovery cycles (stop → diagnose → fix → reset → run). If the third
   attempt fails, stop and escalate to the human operator.

8. **Respect intervention leases.** Interventions have a timeout. If your
   intervention is taking longer than expected, renew the lease with
   `manyforge_intervention(action="heartbeat")`. Expired leases are
   automatically aborted.

## Data Safety

9. **Save before destructive operations.** Before replacing a tree or
   clearing all runtime overrides, save the current program with
   `manyforge_program_save()`.

10. **Do not write files outside allowed roots.** The filesystem tools
    enforce path restrictions. Do not attempt to bypass them.

11. **Preserve audit trail.** Do not clear runtime overrides or reset state
    without first exporting a debug bundle if the run produced errors.

## Authority Boundaries

12. **Always identify yourself.** When executing interventions, use your
    assigned `principal_id` (typically `"llm:openclaw-agent"`). Never
    impersonate a human principal.

13. **Respect `program_only` nodes.** Nodes with `intervention_delegate`
    set to `"program_only"` handle their own recovery. Do not create
    interventions for these nodes — let the behavior tree handle them.

14. **Escalate when uncertain.** If you are unsure whether an action is
    safe, stop execution and ask the human operator. The cost of a false
    stop is low; the cost of a physical collision is high.

## Recovery Protocol

When a program stops due to failure:

1. **Assess** — Call `manyforge_execution_status(include="all")` and
   `manyforge_scene_state()` to understand what happened.
2. **Diagnose** — Check `manyforge_intervention(action="pending_recovery")`
   for structured recovery requests from the behavior tree.
3. **Plan** — Determine if the failure is recoverable:
   - Scene mismatch → scene edit may fix it
   - Parameter error → runtime override may fix it
   - Node failure → intervention subtree may fix it
   - Unknown → escalate to operator
4. **Act** — Apply the fix (scene edit, override, or intervention).
5. **Verify** — Reset and run, monitoring the first few ticks closely.
6. **Report** — Summarize what happened, what you did, and the outcome.

If at any point you are unsure, **stop and escalate**. A stopped robot is
a safe robot.
