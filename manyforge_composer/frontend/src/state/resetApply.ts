/**
 * Apply while a Program run exists: the ordinary Reset, then the ordinary
 * Apply (maintainer, 2026-10-02).
 *
 * The Draft (Program, Scene or Deployment) may be edited while a run is held
 * with all its motion stopped. Its edits reach the robot only through Apply,
 * and Apply first ends the run: after one informed confirmation the browser
 * sends the exact-identity Reset, observes that Reset's correlated cycle Idle
 * (a receipt is never settlement) and authoring admission, then sends the
 * ordinary Apply of exactly the confirmed Draft revision and follows it with
 * the existing Apply tracker. Nothing is Saved, Executed, Run, resumed or
 * replayed automatically.
 */
import type {
  ManagerApplyStatus,
  ProgramCycleStatus,
  ResetApplyContext,
} from '../types/api';
import {
  ACTIVATION_SCOPE_CHANGED,
  sameActivationScope,
  type DeploymentActivationScope,
} from './activationScope';
import { applyMayHaveActed, type ManagerApplyIdentity } from './managerApply';

export type ResetApplyDocument = 'program' | 'deployment';

/** Everything confirmed at once: the run Reset ends and the Draft Applied. */
export interface ResetApplyPlan {
  correlationId: string;
  document: ResetApplyDocument;
  programRunId: string;
  programRunnerOwnerEpoch: string;
  /** The held cycle the operator confirmed; Reset must end exactly this one. */
  cycleGeneration: number;
  draftId: string;
  draftRevision: number;
  /** The Program revision (Program) or Deployment revision of the Draft. */
  draftContentRevision: string | null;
  /** The Applied revision of the same document; it must not change. */
  appliedRevision: string | null;
  /** Deployment only: the complete activation input confirmed (the
   * Deployment Draft, the Program Draft it compiles with, the role
   * assignment and the source document). Null for a Program Apply. */
  activationScope: DeploymentActivationScope | null;
}

export function resetApplyContext(plan: ResetApplyPlan): ResetApplyContext {
  return {
    schemaVersion: 'manyforge.composer_reset_apply_context.v1',
    correlationId: plan.correlationId,
    document: plan.document,
    programRunId: plan.programRunId,
    programRunnerOwnerEpoch: plan.programRunnerOwnerEpoch,
    cycleGeneration: plan.cycleGeneration,
    draftId: plan.draftId,
    draftRevision: plan.draftRevision,
    draftContentRevision: plan.draftContentRevision,
    appliedRevision: plan.appliedRevision,
  };
}

/** The Reset this chain sent, as returned by the runtime controller. */
export interface ResetApplyResetIdentity {
  requestId: string;
  correlationId: string;
  programRunId: string;
  programRunnerOwnerEpoch: string;
  cycleGeneration: number;
}

export type ResetApplyPhase =
  | { kind: 'recording' }
  | { kind: 'resetting' }
  | {
    kind: 'awaiting_idle';
    reset: ResetApplyResetIdentity;
    /** `unconfirmed`: Reset was submitted but its reply was lost, so whether
     * the owner received it is unknown. It is never resent; only the cycle's
     * correlated settlement can prove what happened. */
    delivery: 'confirmed' | 'unconfirmed';
  }
  | {
    kind: 'applying';
    reset: ResetApplyResetIdentity;
    /** The correlated Idle that settled Reset; rechecked before Apply. */
    idle: { cycleGeneration: number };
    sent: boolean;
  }
  | { kind: 'awaiting_apply'; apply: ManagerApplyIdentity }
  /** The Manager reported a disruptive Deployment change: the ordinary
   * Apply confirmation now asks the operator; this chain is done. */
  | { kind: 'confirmation_required' }
  | { kind: 'applied'; apply: ManagerApplyIdentity | null }
  | {
    kind: 'failed';
    stage: 'record' | 'reset' | 'apply';
    message: string;
    /** The stage's effect cannot be proven absent. */
    unknown: boolean;
    apply: ManagerApplyIdentity | null;
  };

export interface ResetApplySequence {
  plan: ResetApplyPlan;
  phase: ResetApplyPhase;
}

/** The authoritative facts each step re-reads; never a cached owner state. */
export interface ResetApplyFacts {
  cycle: Pick<ProgramCycleStatus,
    | 'state' | 'programRunnerOwnerEpoch' | 'cycleGeneration' | 'programRunId'
    | 'causalCorrelationId' | 'failure'> | null;
  /** The ordinary Apply of this document is admitted (Idle, fences current). */
  applyAvailable: boolean;
  /** An operation of this document's workspace is still in flight. */
  workspacePending: boolean;
  /** The Draft identity the next fence acts upon. */
  draft: { draftId: string; draftRevision: number } | null;
  /** The document's current Applied revision, null while not projected. */
  appliedRevision: string | null;
  activation: Pick<ManagerApplyStatus, 'applyId' | 'applyGeneration' | 'state' | 'failure'> | null;
  /** Deployment only: the current complete activation input. */
  activationScope: DeploymentActivationScope | null;
  /** The Apply accepted after the operator confirmed a disruptive impact,
   * when it carries this chain's correlation. */
  correlatedApply: ManagerApplyIdentity | null;
}

export type ResetApplyStep =
  | { kind: 'wait' }
  | { kind: 'phase'; phase: ResetApplyPhase }
  | { kind: 'send_apply' };

function failed(
  stage: Extract<ResetApplyPhase, { kind: 'failed' }>['stage'],
  message: string,
  unknown = false,
  apply: ManagerApplyIdentity | null = null,
): ResetApplyStep {
  return { kind: 'phase', phase: { kind: 'failed', stage, message, unknown, apply } };
}

/**
 * The next action from current owner facts. Pure: the hook performs
 * `send_apply` once and applies `phase` transitions. A Reset receipt alone
 * is never settlement; only its correlated cycle Idle is.
 */
export function nextResetApplyStep(
  sequence: ResetApplySequence,
  facts: ResetApplyFacts,
): ResetApplyStep {
  const { plan, phase } = sequence;
  if (phase.kind === 'awaiting_idle') {
    const cycle = facts.cycle;
    if (cycle === null) return { kind: 'wait' };
    if (cycle.programRunnerOwnerEpoch !== phase.reset.programRunnerOwnerEpoch) {
      return failed('reset', 'The Program runner restarted before Reset settled. Its outcome is unknown; review the run state. Nothing was Applied.', true);
    }
    if (cycle.causalCorrelationId === plan.correlationId) {
      if (cycle.state === 'idle_after_reset'
        && cycle.cycleGeneration > phase.reset.cycleGeneration) {
        return {
          kind: 'phase',
          phase: {
            kind: 'applying', reset: phase.reset,
            idle: { cycleGeneration: cycle.cycleGeneration }, sent: false,
          },
        };
      }
      if (cycle.state === 'recovery_required') {
        const message = cycle.failure?.message;
        return failed('reset', `Reset did not complete${typeof message === 'string' && message
          ? `: ${message}` : ''}. Nothing was Applied.`);
      }
    } else if (cycle.cycleGeneration > phase.reset.cycleGeneration) {
      // Another request settled this cycle: never Apply on its Idle.
      return failed('reset', 'Another request ended the run first. Nothing was Applied; review the state and Apply again.');
    }
    return { kind: 'wait' };
  }
  if (phase.kind === 'applying') {
    if (phase.sent) return { kind: 'wait' };
    // The settled Reset's identity is rechecked on every observation while
    // waiting for admission and immediately before Apply is sent: the same
    // runner owner, the same correlated Idle cycle and no run since.
    const cycle = facts.cycle;
    if (cycle !== null) {
      if (cycle.programRunnerOwnerEpoch !== phase.reset.programRunnerOwnerEpoch) {
        return failed('apply', 'The Program runner restarted after Reset settled. Apply was not sent; review the run state and Apply again.');
      }
      if (
        cycle.state !== 'idle_after_reset'
        || cycle.cycleGeneration !== phase.idle.cycleGeneration
        || cycle.programRunId !== null
        || cycle.causalCorrelationId !== plan.correlationId
      ) {
        return failed('apply', 'The cycle changed after Reset reached Idle (another run or request). Apply was not sent; review the state and Apply again.');
      }
    }
    if (cycle === null || facts.workspacePending || !facts.applyAvailable) return { kind: 'wait' };
    const draft = facts.draft;
    if (
      draft === null
      || draft.draftId !== plan.draftId
      || draft.draftRevision !== plan.draftRevision
    ) {
      return failed('apply', 'The Draft changed after Reset (for example in another browser). Apply was not sent; review the Draft and Apply again.');
    }
    // A missing projection is not a change: wait for the next observation.
    if (plan.appliedRevision !== null && facts.appliedRevision === null) return { kind: 'wait' };
    if (facts.appliedRevision !== plan.appliedRevision) {
      return failed('apply', 'The Applied configuration changed after Reset. Apply was not sent; review the Draft and Apply again.');
    }
    if (
      plan.activationScope !== null
      && !sameActivationScope(plan.activationScope, facts.activationScope)
    ) {
      return failed('apply', `${ACTIVATION_SCOPE_CHANGED} Apply was not sent; the Draft keeps its edits.`);
    }
    return { kind: 'send_apply' };
  }
  if (phase.kind === 'confirmation_required' && facts.correlatedApply !== null) {
    // The operator confirmed the impact: follow that Apply to settlement.
    return { kind: 'phase', phase: { kind: 'awaiting_apply', apply: facts.correlatedApply } };
  }
  if (phase.kind === 'awaiting_apply') {
    const status = facts.activation;
    if (
      status === null
      || status.applyId !== phase.apply.applyId
      || status.applyGeneration !== phase.apply.applyGeneration
    ) return { kind: 'wait' };
    if (status.state === 'succeeded') {
      return { kind: 'phase', phase: { kind: 'applied', apply: phase.apply } };
    }
    if (status.state === 'failed' || status.state === 'unknown_effect') {
      return failed(
        'apply',
        `Apply ${status.state === 'failed' ? 'failed' : 'has an unknown effect'}${status.failure
          ? `: ${status.failure.message || status.failure.code}` : ''}. The Draft keeps its edits; see Activity.`,
        applyMayHaveActed(status.state),
        phase.apply,
      );
    }
  }
  return { kind: 'wait' };
}

/** Plain phase text for Activity; never claims more than observed. */
export function resetApplyPhaseText(phase: ResetApplyPhase): string {
  switch (phase.kind) {
    case 'recording': return 'Recording the Apply context…';
    case 'resetting': return 'Sending Reset…';
    case 'awaiting_idle': return phase.delivery === 'confirmed'
      ? 'Reset sent; waiting for the run to end and the cycle to reach Idle.'
      : 'Reset delivery unknown — reconciling from the observed cycle. Reset is not resent, and nothing is Applied until its settlement is observed.';
    case 'applying': return phase.sent ? 'Applying the Draft…' : 'Idle reached; waiting for authoring admission to Apply.';
    case 'awaiting_apply': return `Apply ${phase.apply.applyId} accepted; waiting for activation to settle.`;
    case 'confirmation_required': return 'Reset done. The Deployment change needs your confirmation of its impact before Apply.';
    case 'applied': return 'Applied; awaiting Execute/Run; not saved to Scenario.';
    case 'failed': return phase.message;
  }
}

/** Terminal phases and an unknown-delivery Reset may be dismissed;
 * dismissing only stops following and never undoes, resends or resumes. */
export function resetApplyDismissible(phase: ResetApplyPhase): boolean {
  return phase.kind === 'applied' || phase.kind === 'failed'
    || phase.kind === 'confirmation_required'
    || (phase.kind === 'awaiting_idle' && phase.delivery === 'unconfirmed');
}

export function resetApplyTerminal(phase: ResetApplyPhase): boolean {
  return phase.kind === 'applied' || phase.kind === 'failed'
    || phase.kind === 'confirmation_required';
}
