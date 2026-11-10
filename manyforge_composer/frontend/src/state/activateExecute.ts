// Activate and execute (the Deployment decides; every active
// component must be enabled). Pure reads of the backend's RunReadiness and
// component states; the backend runs every action.
import type { ComponentState, RunBlockedReason, RunReadiness } from '../types/components';
import { OWNER_NOT_PREPARED } from '../types/components';
import { actionResultText, codedErrorText, needsRecovery, ownerIds } from './componentPresentation';

/** Blocked reasons that Activate (Prepare all, then Enable all) clears. */
const ACTIVATION_CODES = new Set(['component_not_enabled', 'component_not_prepared', OWNER_NOT_PREPARED]);

/**
 * Whether Activate clears a blocked reason: a component not enabled or not
 * prepared, or an offline one, which Prepare all prepares
 * (an owner before its members; e.g. after an execution host restart).
 * A latched cell keeps its `cell_latched` reason, so Activate isn't offered.
 */
export function activationClears(reason: RunBlockedReason, states: readonly ComponentState[]): boolean {
  if (ACTIVATION_CODES.has(reason.code)) return true;
  if (reason.code !== 'component_unhealthy') return false;
  const state = states.find((candidate) => candidate.component_id === reason.component_id);
  if (state === undefined) return false;
  // A fault or e-stop that Recover and enable may clear (the dialog shows the causes;
  // one it can't recover keeps the button disabled), on the component or on a machine
  // whose members need it.
  return state.lifecycle === 'offline' || needsRecovery(state, states)
    || recoveryTargets(states).some((id) => states.find((member) => member.component_id === id)
      ?.owner_id === state.component_id);
}

/** Components Execute has to recover and enable: not enabled, not a machine, and their own or their machine's fault or e-stop. */
export function recoveryTargets(states: readonly ComponentState[]): string[] {
  const owners = ownerIds(states);
  return states.filter((state) => state.lifecycle !== 'enabled' && !owners.has(state.component_id)
    && needsRecovery(state, states)).map((state) => state.component_id);
}

/**
 * Reasons that may only persist because components are down: a signal is
 * fresh once its I/O component is enabled. Execute's normal readiness check
 * after activation decides; a hold that persists is the server's typed refusal.
 */
const ACTIVATION_MAY_CLEAR_CODES = new Set(['rule_holds', 'input_not_fresh']);

export function activationMayClear(reason: RunBlockedReason): boolean {
  return ACTIVATION_MAY_CLEAR_CODES.has(reason.code);
}

/**
 * Execute is blocked by components Activate brings up, and by nothing else
 * than rule holds or stale inputs those components may be the cause of.
 */
export function activationOffered(readiness: RunReadiness | null, states: readonly ComponentState[] | null): boolean {
  if (readiness === null || states === null || readiness.can_execute) return false;
  const reasons = readiness.blocked_reasons;
  return reasons.some((reason) => activationClears(reason, states))
    && reasons.every((reason) => activationClears(reason, states) || activationMayClear(reason));
}

/** The components that aren't ready to run: not enabled; an owner (no Enable) not prepared. */
export function inactiveComponentIds(states: readonly ComponentState[]): string[] {
  const owners = ownerIds(states);
  return states.filter((state) => state.lifecycle !== 'enabled'
    && !(owners.has(state.component_id) && state.lifecycle === 'prepared'))
    .map((state) => state.component_id);
}

/**
 * One group action of the activation, with each target's last action and
 * the cell's Stop generation before it was sent.
 */
export interface ActivationStep {
  action: 'prepare' | 'enable';
  targets: string[];
  before: Record<string, number | null>;
  stopGeneration: number | null;
}

export interface ActivationFailure {
  /** Null for a refused request or a reason not tied to one component. */
  componentId: string | null;
  text: string;
}

export function activationStep(
  action: ActivationStep['action'],
  targets: readonly string[],
  states: readonly ComponentState[],
  stopGeneration: number | null = null,
): ActivationStep {
  return {
    action,
    targets: [...targets],
    stopGeneration,
    before: Object.fromEntries(targets.map((id) => [
      id, states.find((state) => state.component_id === id)?.last_action?.started_at_ns ?? null,
    ])),
  };
}

/**
 * Where a step stands in the pushed states: done when every target reached
 * the action's state (prepare: prepared or enabled; enable: enabled); a
 * target that faulted, left the cell or whose action ended without success
 * is a failure. The group runs its targets one at a time, and a target's
 * fault is judged only once its own action for this step ran and ended (its
 * last action differs from the one before the step) or the whole group
 * finished: an action owns the transient states of its machine while it
 * runs (e.g. an xArm Enable's arming briefly stops the joint states, a
 * stale-stream fault that clears within the Enable, and may show on a
 * gripper or I/O member still waiting its turn). A Stop ends the group
 * (the cell's Stop generation changed since the step was sent): once nothing
 * runs, a faulted target is judged and a target that never got its action
 * is a failure too.
 */
export function stepProgress(
  step: ActivationStep,
  states: readonly ComponentState[],
  stopGeneration: number | null = null,
): { done: boolean; failures: ActivationFailure[] } {
  const failures: ActivationFailure[] = [];
  const reached = (state: ComponentState) => state.lifecycle === 'enabled'
    || (step.action === 'prepare' && state.lifecycle === 'prepared');
  const ran = (state: ComponentState) => state.action === undefined && state.last_action !== undefined
    && state.last_action.started_at_ns !== step.before[state.component_id];
  const targets = step.targets.map((componentId) => ({
    componentId, state: states.find((candidate) => candidate.component_id === componentId),
  }));
  const stopped = step.stopGeneration !== null && stopGeneration !== null
    && stopGeneration !== step.stopGeneration;
  const idle = targets.every(({ state }) => state?.action === undefined);
  // Finished: nothing runs and every target ran its action or is already
  // there, or a Stop ended the group.
  const groupFinished = idle && (stopped || targets.every(({ state }) => state === undefined
    || ran(state) || reached(state)));
  let done = true;
  for (const { componentId, state } of targets) {
    const last = state?.last_action;
    if (state === undefined) {
      failures.push({ componentId, text: 'Not in the cell' });
    } else if (state.action !== undefined) {
      done = false;
    } else if (state.fault !== undefined) {
      if (ran(state) || groupFinished) failures.push({ componentId, text: codedErrorText(state.fault) });
      else done = false;
    } else if (last !== undefined && last.action === step.action && last.outcome !== 'succeeded' && ran(state)) {
      failures.push({ componentId, text: actionResultText(last) });
    } else if (!reached(state)) {
      if (stopped && groupFinished && !ran(state)) {
        failures.push({ componentId, text: codedErrorText({ code: 'component_action_interrupted_by_stop', message: '' }) });
      } else done = false;
    }
  }
  return { done: done && failures.length === 0, failures };
}
