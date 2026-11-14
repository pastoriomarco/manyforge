/**
 * Display helpers around a Program run. Whether a held run leaves the Draft
 * editable is one backend rule (Headless `held_run_authoring.py`), published
 * as `heldRunDraftEditAllowed` in the control state; the browser only reads
 * it.
 */
import type { ProgramCycleStatus, ProgramRunStateName } from '../types/api';
import type { ComponentState } from '../types/components';
import type { ComposerHeadlessState } from '../types/headless';
import { robotControlHasActiveOperation, type RobotControlState } from './robotControl';

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

/** The Program-cycle facts the document commands read about motion. */
export interface DocumentMotionCycle {
  state: ProgramCycleStatus['state'];
  run: { state: ProgramRunStateName } | null;
  runtimeControls: {
    executionSelections: { motionHeld: boolean; groups: ReadonlyArray<{ state: string }> };
  } | null;
  /** The Runner's commands; `program_reset` allowed is the Reset it offers. */
  allowedCommands?: ReadonlyArray<{ commandKind: string; allowed: boolean }>;
}

/**
 * Robots moving or stopping, as the Scenario document commands wait for them.
 *
 * `moving`: a Program run that is not positively held (`motionHeld`), a
 * starting cycle, or an active manual operation. A run's logic
 * between motions counts as moving: no published fact tells it apart.
 * `stopping`: a Stop in progress, of a manual operation, of a Program group,
 * or of the whole Program (Stop all, Halt). New has its own rule
 * (:func:`newMotionReason`).
 */
export function documentMotion(
  cycle: DocumentMotionCycle | null | undefined,
  robotControl: RobotControlState,
): { moving: boolean; stopping: boolean } {
  const selections = cycle?.runtimeControls?.executionSelections;
  const moving = robotControlHasActiveOperation(robotControl)
    || cycle?.state === 'starting'
    || (cycle?.state === 'running' && selections?.motionHeld !== true);
  const stopping = robotControl.phase === 'stopping'
    || cycle?.run?.state === 'stopping' || cycle?.run?.state === 'halting'
    || (selections?.groups.some((group) => group.state === 'stopping') ?? false);
  return { moving, stopping };
}

/** Why the document commands wait while robots move. */
export const DOCUMENT_MOTION_REASON = 'Not available while robots are moving. Stop motion first.';
/** Why New waits while a Stop is in progress. */
export const STOP_IN_PROGRESS_REASON = 'A Stop is in progress.';
/** Why New waits for a held run the held-run rule does not admit. */
export const HELD_RUN_NEW_REASON = 'Not available until all Program motion is confirmed stopped.';
/** Why New waits in `recovery_required` while the Runner offers a Reset. */
export const RECOVERY_RESET_NEW_REASON = 'Reset the Program first.';
/** Why New waits in `recovery_required` while no Reset is offered. */
export const RECOVERY_NEW_REASON = 'Not available until the Program cycle is recovered.';
/** Why New and Load wait while a component action runs (OD-194). */
export const COMPONENT_ACTION_REASON = 'Not available while a component action is running.';

/**
 * Any component has an action running (its `action`), as Composer's New
 * refuses `component_action_in_progress` (OD-194). Unknown components: false,
 * the server decides.
 */
export function componentActionRunning(
  components: ReadonlyArray<Pick<ComponentState, 'action'>> | null | undefined,
): boolean {
  return components?.some((component) => component.action !== undefined) ?? false;
}

/** Why a Program run leaves the Draft not editable (the held-run rule refuses). */
export function heldRunEditReason(cycleState: string): string {
  return `Program execution is ${cycleState.replaceAll('_', ' ')}; editing requires Idle or all Program motion stopped (Stop, Halt, a failure pause, or Executed but not yet Run).`;
}

/**
 * Why New waits for the robots, or `null` (maintainer, 2026-11-13: the
 * operator stops motion first). A Stop in progress; a manual operation, a
 * starting cycle or a Reset (which may move the robot) as motion. A running
 * cycle waits unless the published held-run rule (`heldRunDraftEditAllowed`,
 * the server's own) admits it: as motion while its motion isn't held, until
 * all its motion is confirmed stopped otherwise. A recovery-required cycle,
 * which the server always refuses, waits for its Reset (the Runner's
 * `program_reset`, as the Reset button) or its recovery. Then a running
 * component action (`componentActionRunning`, OD-194). A finished run or
 * Idle are the server's to decide.
 */
export function newMotionReason(
  cycle: DocumentMotionCycle | null | undefined,
  robotControl: RobotControlState,
  heldRunDraftEditAllowed: boolean,
  componentActionActive: boolean,
): string | null {
  if (documentMotion(cycle, robotControl).stopping) return STOP_IN_PROGRESS_REASON;
  if (robotControlHasActiveOperation(robotControl)
    || cycle?.state === 'starting' || cycle?.state === 'resetting') return DOCUMENT_MOTION_REASON;
  if (cycle?.state === 'running' && !heldRunDraftEditAllowed) {
    return cycle.runtimeControls?.executionSelections.motionHeld === true
      ? HELD_RUN_NEW_REASON : DOCUMENT_MOTION_REASON;
  }
  if (cycle?.state === 'recovery_required') {
    return cycle.allowedCommands?.some(
      (command) => command.commandKind === 'program_reset' && command.allowed,
    ) ? RECOVERY_RESET_NEW_REASON : RECOVERY_NEW_REASON;
  }
  if (componentActionActive) return COMPONENT_ACTION_REASON;
  return null;
}
