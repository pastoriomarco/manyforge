/**
 * Whether a held Program run leaves the Draft editable (maintainer,
 * 2026-10-02).
 *
 * While a Program run exists the Draft — Program, Scene and Deployment — is
 * editable only when the owners confirm that all of the run's motion is
 * stopped: a failure pause whose Stop of the other Program motion is
 * confirmed, an operator Stop that holds the whole Program, a settled Halt,
 * or a run that was Executed but never Run (every execution group still
 * `ready`), with every manual operation affirmatively settled. Unknown or settling facts are
 * "not editable". The Composer backend applies the same rule to every Draft
 * edit (`held_run_authoring.py`); this copy only explains it in the UI.
 */
import type { ComposerHeadlessState } from '../types/headless';

/**
 * One manual operation affirmatively settled. The Kernel's control-state row
 * carries only the operation state and `resident` (true while the Kernel
 * retains any authority over it), so settlement is proven only by a released
 * (non-resident) terminal result, or by the owner's never-dispatched proof
 * (`cancelled_pre_dispatch`). `interrupted`, `intervention_required`, any
 * in-progress, missing or unknown state is not settled. Mirrors
 * `manual_operation_settled` in `held_run_authoring.py`.
 */
export function manualOperationSettled(
  operation: { resident?: unknown; state?: unknown } | null | undefined,
): boolean {
  if (operation === null || operation === undefined) return false;
  if (operation.state === 'cancelled_pre_dispatch') return typeof operation.resident === 'boolean';
  return operation.resident === false
    && (operation.state === 'completed' || operation.state === 'stopped' || operation.state === 'failed');
}

/**
 * The run was Executed but no motion was ever started. The Program runner
 * publishes a group as `ready` only while the Kernel still holds every
 * effectful node of it with its initial never-released cause; with every
 * group `ready` no operation was ever accepted for the run. A recovery offer
 * means a node already failed. Mirrors `_never_run` in
 * `held_run_authoring.py`.
 */
function neverRun(cycle: NonNullable<ComposerHeadlessState['programCycle']>): boolean {
  if ((cycle.recoveryOffer ?? null) !== null) return false;
  const groups = cycle.runtimeControls?.executionSelections.groups;
  return Array.isArray(groups) && groups.length > 0
    && groups.every((group) => group.state === 'ready');
}

export function heldRunMotionStopped(snapshot: ComposerHeadlessState | null): boolean {
  const cycle = snapshot?.programCycle ?? null;
  const run = cycle?.run ?? null;
  if (
    cycle === null || cycle.state !== 'running' || run === null
    || cycle.programRunId === null || run.capture.programRunId !== cycle.programRunId
  ) return false;
  const control = snapshot?.executionControlStatus ?? null;
  if (
    control === null
    || !Array.isArray(control.manualOperations)
    || !control.manualOperations.every(manualOperationSettled)
  ) return false;
  if (run.state === 'full_stopped') return run.activeOperationCount === 0;
  if (run.state !== 'running' && run.state !== 'intervention_required') return false;
  if (run.state === 'running' && neverRun(cycle)) return true;
  // A failure pause proves its containment only through its published
  // recovery offer; a missing offer is unknown, not stopped.
  if (run.state === 'intervention_required' && !Array.isArray(cycle.recoveryOffer?.actions)) return false;
  const all = cycle.runtimeControls?.executionSelections.groups
    .filter((group) => group.groupId === 'all') ?? [];
  if (all.length !== 1) return false;
  const [group] = all;
  const held = group.state === 'held'
    || (group.state === 'mixed' && group.nodeCount > 0 && group.heldNodeCount === group.nodeCount);
  const containmentUnconfirmed = cycle.recoveryOffer?.actions.some(
    (action) => action.disabledReasonCode === 'failure_pause_containment_unconfirmed',
  ) ?? false;
  return held && !containmentUnconfirmed;
}

/** A Program run exists (the cycle is not Idle), whatever its motion. */
export function programRunExists(snapshot: ComposerHeadlessState | null): boolean {
  return snapshot !== null && snapshot.programCycle.state !== 'idle_after_reset';
}
