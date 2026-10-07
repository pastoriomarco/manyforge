/**
 * Display helpers around a Program run. Whether a held run leaves the Draft
 * editable is one backend rule (Headless `held_run_authoring.py`), published
 * as `heldRunDraftEditAllowed` in the control state; the browser only reads
 * it.
 */
import type { ComposerHeadlessState } from '../types/headless';

/**
 * One manual operation affirmatively settled. The Kernel's control-state row
 * carries only the operation state and `resident` (true while the Kernel
 * retains any authority over it), so settlement is proven only by a released
 * (non-resident) terminal result, or by the owner's never-dispatched proof
 * (`cancelled_pre_dispatch`). `interrupted`, `intervention_required`, any
 * in-progress, missing or unknown state is not settled. Mirrors
 * `manual_operation_settled` in Headless `held_run_authoring.py`; display only.
 */
export function manualOperationSettled(
  operation: { resident?: unknown; state?: unknown } | null | undefined,
): boolean {
  if (operation === null || operation === undefined) return false;
  if (operation.state === 'cancelled_pre_dispatch') return typeof operation.resident === 'boolean';
  return operation.resident === false
    && (operation.state === 'completed' || operation.state === 'stopped' || operation.state === 'failed');
}

/** A Program run exists (the cycle is not Idle), whatever its motion. */
export function programRunExists(snapshot: ComposerHeadlessState | null): boolean {
  return snapshot !== null && snapshot.programCycle.state !== 'idle_after_reset';
}
