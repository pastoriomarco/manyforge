import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { programRunExists } from '../state/heldRunAuthoring';
import { applyResultText } from '../components/ApplyImpact';
import { programActionUnavailableReason } from '../state/authoringWorkspace';
import { deploymentActionUnavailableReason } from '../state/documentStates';
import {
  deploymentApplyResetsRun,
  nextResetApplyStep,
  resetApplyContext,
  resetApplyDismissible,
  resetApplyPhaseText,
  resetApplyTerminal,
  type ResetApplyDocument,
  type ResetApplyFacts,
  type ResetApplyPhase,
  type ResetApplyPlan,
  type ResetApplySequence,
} from '../state/resetApply';
import { errorMessage } from '../state/structuralResource';
import {
  useBriefNotice,
  usePublishWorkbenchNotices,
  type WorkbenchNotice,
} from '../state/workbenchNotices';
import type { ManagerApplyStatus } from '../types/api';
import type { ApplyPreview } from '../types/components';
import type { ComposerHeadlessState } from '../types/headless';
import type { DeploymentWorkspaceController } from './useDeploymentWorkspace';
import type { ProgramRuntimeController } from './useProgramRuntime';
import {
  ProgramDraftChangedError,
  type ProgramWorkspaceController,
} from './useProgramWorkspace';

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

export interface ResetApplyInputs {
  runtime: ProgramRuntimeController;
  program: ProgramWorkspaceController;
  deployment: DeploymentWorkspaceController;
  snapshot: ComposerHeadlessState | null;
  /** The exact Manager Apply status App currently observes, if any. */
  activation: ManagerApplyStatus | null;
}

/**
 * Program Apply while a Program run exists: one informed confirmation, then
 * the ordinary Reset of exactly the confirmed run, its observed correlated
 * Idle, and the ordinary Apply of exactly the confirmed Program Draft
 * revision. The Deployment Apply
 * offers the same chain while a run is stopped or held: its Apply is
 * the server Apply of exactly the confirmed Preview's scope. The backend
 * fences stay authoritative. State lives only in this page: reopening
 * observes owners and never resumes a chain.
 */
export function useResetApply({
  runtime,
  program,
  deployment,
  snapshot,
  activation,
}: ResetApplyInputs) {
  const [sequence, setSequence] = useState<ResetApplySequence | null>(null);
  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);
  // The one synchronous guard: a double click or a second Apply cannot start
  // a second chain, and the chain never refuses its own next stage.
  const chain = useRef<string | null>(null);
  // Each Apply is sent at most once per correlation, whatever re-renders.
  const applySent = useRef(new Set<string>());
  const latest = useRef({ runtime, program, deployment });
  useEffect(() => {
    latest.current = { runtime, program, deployment };
  });

  /** Apply must first Reset the run: one exists, whatever its motion. */
  const runExists = programRunExists(snapshot);
  /** A Deployment Apply must first Reset a stopped or held run. */
  const deploymentRunReset = deploymentApplyResetsRun(snapshot);
  const inProgress = sequence !== null && !resetApplyTerminal(sequence.phase);
  const programWorkspace = program.state.workspace;
  const deploymentWorkspace = deployment.workspace;

  /**
   * Why Program Apply (with its Reset) is unavailable while a run exists, or
   * null. Never depends on a motion preview.
   */
  const unavailableReason = useCallback((document: ResetApplyDocument = 'program'): string | null => {
    if (inProgress) return 'An Apply that Resets the run is in progress.';
    // The Draft is editable only while all Program motion is stopped; the
    // same holds for Applying it.
    const reason = document === 'deployment'
      ? deploymentActionUnavailableReason(deploymentWorkspace, 'apply', deployment.mutationUnavailableReason)
      : programActionUnavailableReason(programWorkspace, 'apply', program.mutationUnavailableReason);
    if (reason !== null) return reason;
    if (!runtime.runnerAllows('program_reset')) {
      const refusal = runtime.runnerRefusal('program_reset');
      return `Reset is not available now${refusal ? `: ${refusal.replaceAll('_', ' ')}` : ''}.`;
    }
    return null;
  }, [deployment.mutationUnavailableReason, deploymentWorkspace, inProgress, program.mutationUnavailableReason, programWorkspace, runtime]);

  const fail = useCallback((correlationId: string, phase: Extract<ResetApplyPhase, { kind: 'failed' }>) => {
    if (!mounted.current) return;
    setSequence((current) => current?.plan.correlationId === correlationId
      ? { ...current, phase } : current);
  }, []);
  const advance = useCallback((correlationId: string, phase: ResetApplyPhase) => {
    if (!mounted.current) return;
    setSequence((current) => current?.plan.correlationId === correlationId
      ? { ...current, phase } : current);
  }, []);

  /**
   * The operator confirmed Apply in its ordinary confirmation (which warns
   * that the run is Reset). Captures the exact run and Draft identities now;
   * resolves to a refusal message when nothing was started.
   */
  const confirm = useCallback(async (
    document: ResetApplyDocument = 'program',
    /** Deployment only: the Preview the operator confirmed. */
    scope: ApplyPreview['scope'] | null = null,
  ): Promise<string | null> => {
    if (chain.current !== null) return 'An Apply that Resets the run is in progress.';
    const reason = unavailableReason(document);
    if (reason !== null) return reason;
    const cycle = snapshot?.programCycle ?? null;
    if (cycle === null || cycle.programRunId === null) {
      return 'The observed cycle has no run to Reset. Wait for the run state to refresh.';
    }
    let draft: Pick<ResetApplyPlan, 'draftId' | 'draftRevision' | 'draftContentRevision' | 'appliedRevision'>;
    if (document === 'deployment') {
      if (deploymentWorkspace === null || scope === null) {
        return 'Preview the Deployment Draft before Reset and Apply.';
      }
      draft = {
        draftId: deploymentWorkspace.draft.id,
        draftRevision: deploymentWorkspace.draft.revision,
        draftContentRevision: deploymentWorkspace.draft.deploymentRevision,
        appliedRevision: deploymentWorkspace.managerApplied.deploymentRevision,
      };
    } else {
      const fence = program.fenceDraft;
      if (programWorkspace === null || fence === null || fence.programRevision === null) {
        return 'The Program Draft has no complete candidate to Apply.';
      }
      draft = {
        draftId: fence.draftId,
        draftRevision: fence.draftRevision,
        draftContentRevision: fence.programRevision,
        appliedRevision: programWorkspace.applied.programRevision,
      };
    }
    const plan: ResetApplyPlan = {
      correlationId: identity('reset-apply'),
      document,
      programRunId: cycle.programRunId,
      programRunnerOwnerEpoch: cycle.programRunnerOwnerEpoch,
      cycleGeneration: cycle.cycleGeneration,
      ...draft,
      scope: document === 'deployment' ? scope : null,
    };
    chain.current = plan.correlationId;
    setSequence({ plan, phase: { kind: 'recording' } });
    try {
      await api.authoringWorkspace.recordResetApplyContext(resetApplyContext(plan));
    } catch (error: unknown) {
      fail(plan.correlationId, {
        kind: 'failed', stage: 'record', unknown: false, apply: null,
        message: `Apply was not started: ${errorMessage(error, 'its context could not be recorded')}. Nothing was changed.`,
      });
      return null;
    }
    if (!mounted.current) return null;
    advance(plan.correlationId, { kind: 'resetting' });
    // Reset ends exactly the cycle the operator confirmed: the runtime
    // refuses, unsent, when the run changed while the context was recorded.
    const outcome = await latest.current.runtime.reset({
      correlationId: plan.correlationId,
      expected: {
        programRunId: plan.programRunId,
        cycleGeneration: plan.cycleGeneration,
      },
    });
    if (outcome.status === 'refused') {
      fail(plan.correlationId, {
        kind: 'failed', stage: 'reset', unknown: false, apply: null,
        message: `${outcome.message} The run continues held and nothing was Applied.`,
      });
      return null;
    }
    if (outcome.reset.programRunId === null) {
      fail(plan.correlationId, { kind: 'failed', stage: 'reset', unknown: true, apply: null,
        message: 'Reset returned an Idle cycle rather than the confirmed held run. Nothing was Applied.' });
      return null;
    }
    // A lost reply is not a refusal: follow the same correlated settlement,
    // never resend, and Apply nothing until it is observed.
    advance(plan.correlationId, {
      kind: 'awaiting_idle',
      reset: { ...outcome.reset, programRunId: outcome.reset.programRunId },
      delivery: outcome.status === 'sent' ? 'confirmed' : 'unconfirmed',
    });
    return null;
  }, [advance, deploymentWorkspace, fail, program.fenceDraft, programWorkspace, snapshot, unavailableReason]);

  // --- Drive the chain from owner observations (never from receipts alone).
  const programPending = program.state.status === 'loading'
    || (program.state.status === 'ready' && program.state.pendingAction !== null);
  const chainDocument = sequence?.plan.document ?? 'program';
  const facts = useMemo((): ResetApplyFacts => (chainDocument === 'deployment' ? {
    cycle: snapshot?.programCycle ?? null,
    applyAvailable: deployment.documentUnavailableReason === null,
    workspacePending: deployment.busy,
    draft: deploymentWorkspace === null ? null
      : { draftId: deploymentWorkspace.draft.id, draftRevision: deploymentWorkspace.draft.revision },
    appliedRevision: deploymentWorkspace?.managerApplied.deploymentRevision ?? null,
    activation: null,
  } : {
    cycle: snapshot?.programCycle ?? null,
    applyAvailable: program.documentUnavailableReason === null,
    workspacePending: programPending,
    // The content the next fence acts upon, not a possibly newer reply.
    draft: program.fenceDraft,
    appliedRevision: programWorkspace?.applied.programRevision ?? null,
    activation,
  }), [activation, chainDocument, deployment.busy, deployment.documentUnavailableReason, deploymentWorkspace, program.documentUnavailableReason, program.fenceDraft, programPending, programWorkspace, snapshot]);

  useEffect(() => {
    const current = sequence;
    if (current === null) return;
    const step = nextResetApplyStep(current, facts);
    const { plan } = current;
    const { correlationId } = plan;
    if (step.kind === 'phase') {
      advance(correlationId, step.phase);
      return;
    }
    if (step.kind !== 'send_apply' || current.phase.kind !== 'applying') return;
    if (applySent.current.has(correlationId)) return;
    applySent.current.add(correlationId);
    advance(correlationId, { ...current.phase, sent: true });
    const refused = (error: unknown) => fail(correlationId, error instanceof ProgramDraftChangedError
      ? {
        kind: 'failed', stage: 'apply', unknown: false, apply: null,
        message: `${error.message} Apply was not sent; the Draft keeps its edits.`,
      }
      : {
        kind: 'failed', stage: 'apply', unknown: true, apply: null,
        message: `Apply was not confirmed: ${errorMessage(error, 'unknown error')}. See Activity.`,
      });
    const lost = (detail: string | null) => fail(correlationId, {
      kind: 'failed', stage: 'apply', unknown: true, apply: null,
      message: `Apply was refused or its reply was lost${detail ? `: ${detail}` : ''}. The Draft keeps its edits; Activity shows any accepted activation.`,
    });
    if (plan.document === 'deployment') {
      // The server Apply runs to its end: its result settles the chain.
      void latest.current.deployment.applyConfirmedScope(plan.scope ?? {}).then((result) => {
        if (result === null) lost(latest.current.deployment.error);
        else if (result.outcome === 'failed') {
          fail(correlationId, { kind: 'failed', stage: 'apply', unknown: false, apply: null,
            message: `${applyResultText(result)} The Draft keeps its edits.` });
        } else advance(correlationId, { kind: 'applied', apply: null });
      }, refused);
      return;
    }
    void latest.current.program.apply({
      correlationId,
      expectedDraft: {
        draftId: plan.draftId,
        draftRevision: plan.draftRevision,
        programRevision: plan.draftContentRevision ?? '',
      },
    }).then((result) => {
      if (result === null) {
        lost(latest.current.program.state.error);
      } else if (result.transitionStatus === 'applied') {
        advance(correlationId, { kind: 'applied', apply: null });
      } else if (result.acceptedApply) {
        advance(correlationId, { kind: 'awaiting_apply', apply: result.acceptedApply });
      } else {
        lost('accepted without an exact identity to follow');
      }
    }, refused);
  }, [advance, facts, fail, sequence]);

  // Terminal phases release the guard; nothing is replayed or resumed.
  useEffect(() => {
    if (sequence === null || resetApplyTerminal(sequence.phase)) chain.current = null;
  }, [sequence]);

  const dismiss = useCallback(() => {
    // Stops following; never cancels or reverses an accepted effect.
    chain.current = null;
    setSequence(null);
  }, []);

  // One Activity notice follows the chain with its correlation. A completed
  // Apply is a result, not progress: it clears by itself without Dismiss.
  const current = useMemo(
    // eslint-disable-next-line react-hooks/refs -- resetApplyNotice stores dismiss without calling it during render.
    () => (sequence === null ? null : resetApplyNotice(sequence, dismiss)),
    [dismiss, sequence],
  );
  const applied = useBriefNotice(sequence?.phase.kind === 'applied' ? current : null);
  const notices = useMemo((): WorkbenchNotice[] => (
    sequence?.phase.kind === 'applied'
      ? (applied === null ? [] : [applied])
      : current === null ? [] : [current]
  ), [applied, current, sequence]);
  usePublishWorkbenchNotices('reset-apply', notices);

  return {
    /** A Program run exists: Apply Resets it first. */
    runExists,
    /** A stopped or held run: the Deployment Apply Resets it first. */
    deploymentRunReset,
    unavailableReason,
    confirm,
    inProgress,
    sequence,
    dismiss,
  };
}

export type ResetApplyController = ReturnType<typeof useResetApply>;

/** The Activity notice of one Reset-then-Apply chain. */
export function resetApplyNotice(
  sequence: ResetApplySequence,
  dismiss: () => void,
): WorkbenchNotice {
  const { phase, plan } = sequence;
  const failedPhase = phase.kind === 'failed';
  const name = plan.document === 'program' ? 'Program' : 'Deployment';
  return {
    id: `reset-apply.${plan.correlationId}`,
    // A stopped Apply blocks nothing: a warning until acknowledged.
    severity: failedPhase ? 'warning' : 'info',
    acknowledgeable: failedPhase,
    domain: 'activation',
    title: failedPhase
      ? `${name} Apply stopped`
      : phase.kind === 'applied'
        ? `${name} Applied after Reset`
        : `Resetting the run to Apply the ${name}`,
    message: `${resetApplyPhaseText(phase)} Correlation ${plan.correlationId}.`,
    busy: !resetApplyTerminal(phase),
    actions: resetApplyDismissible(phase) && phase.kind !== 'applied'
      ? [failedPhase
        ? { id: 'acknowledge', label: 'Acknowledge', title: 'Stop showing this Apply. Nothing is undone, resent or resumed.', run: dismiss }
        : { id: 'dismiss', label: 'Dismiss', title: 'Stop showing this Apply. Nothing is undone, resent or resumed.', run: dismiss }]
      : undefined,
  };
}
