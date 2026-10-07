import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api, authoringRefusalMessage, isStaleWorkspaceError } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type { ManagerApplyIdentity } from '../state/managerApply';
import {
  programLocalFence,
  publicationSupersedesDeployment,
  withSharedDeploymentAuthoring,
} from '../state/authoringWorkspace';
import type {
  DeploymentWorkspaceResponse,
  DeploymentCandidateOperationResponse,
  DeploymentControlledStopUpdate,
  JsonValue,
  LocalWorkspaceFenceRequest,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { browserStableIdentity } from './useHeadlessState';
import { deploymentActionUnavailableReason } from '../state/documentStates';
import {
  ACTIVATION_SCOPE_CHANGED,
  deploymentActivationScope,
  sameActivationScope,
  type DeploymentActivationScope,
} from '../state/activationScope';

/** The exact Deployment Draft content a caller acts on (a confirmed Apply). */
export interface ExpectedDeploymentDraft {
  draftId: string;
  draftRevision: number;
}

/**
 * The installed Deployment Draft is not the content the caller confirmed:
 * nothing was sent.
 */
export class DeploymentDraftChangedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'DeploymentDraftChangedError';
  }
}

function requestId(): string {
  return `composer-deployment.${globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`}`;
}

export function useDeploymentWorkspace(
  enabled: boolean,
  snapshot: ComposerHeadlessState | null,
  authoring: AuthoringWorkspaceController,
) {
  const [workspace, setWorkspace] = useState<DeploymentWorkspaceResponse | null>(null);
  const [loading, setLoading] = useState(enabled);
  const [busy, setBusy] = useState(false);
  const [applying, setApplying] = useState(false);
  const [completedAdmission, setCompletedAdmission] = useState<{
    receiverEpoch: string;
    generation: number;
  } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [feedback, setFeedback] = useState<string | null>(null);
  const [acceptedApply, setAcceptedApply] = useState<ManagerApplyIdentity | null>(null);
  /** The shared Reset/Apply correlation the accepted Apply carries, if any. */
  const [acceptedApplyCorrelationId, setAcceptedApplyCorrelationId] = useState<string | null>(null);
  const [pendingApplyConfirmation, setPendingApplyConfirmation] = useState<
    DeploymentCandidateOperationResponse | null
  >(null);
  // What the disruptive-impact confirmation must carry to its Apply: the
  // shared correlation and the exact activation scope the first Apply sent.
  const pendingConfirmationContext = useRef<{
    correlationId: string | undefined;
    scope: DeploymentActivationScope | null;
  } | null>(null);
  const mounted = useRef(true);
  const inFlight = useRef(false);
  const refreshSequence = useRef(0);
  const ownerEpoch = useRef(browserStableIdentity(
    'manyforge.composer.deployment_request_owner_epoch',
    'composer-deployment-owner',
  ));

  const refresh = useCallback(async () => {
    if (!enabled) return null;
    const sequence = ++refreshSequence.current;
    setLoading(true);
    setError(null);
    try {
      const next = await api.deploymentWorkspace.get();
      if (mounted.current && sequence === refreshSequence.current) {
        setWorkspace(next);
      }
      return next;
    } catch (reason: unknown) {
      if (mounted.current && sequence === refreshSequence.current) {
        setError(errorMessage(reason, 'Deployment workspace request failed.'));
      }
      return null;
    } finally {
      if (mounted.current && sequence === refreshSequence.current) {
        setLoading(false);
      }
    }
  }, [enabled]);

  useEffect(() => {
    mounted.current = true;
    if (enabled) void refresh();
    else {
      refreshSequence.current += 1;
      setWorkspace(null);
      setLoading(false);
      setError(null);
      setFeedback(null);
      setPendingApplyConfirmation(null);
    }
    return () => { mounted.current = false; };
  }, [enabled, refresh]);

  // A Draft edit (and Discard) only changes the Draft: it is also admitted
  // while a held run has all motion stopped, and it consumes no admission
  // generation. Every other action (Open, Save, Revert, the ordinary Apply)
  // requires Idle and waits for the generation its claim consumed.
  const unavailableBefore = !enabled
    ? 'Deployment authoring requires the configured headless product.'
    : snapshot === null
      ? 'Backend status is disconnected or has not arrived.'
      : null;
  const mutationUnavailableReason = unavailableBefore
    ?? authoring.editUnavailableReason
    ?? (workspace === null ? 'Deployment workspace is not loaded.' : null);
  const mutationAvailable = mutationUnavailableReason === null;
  const documentUnavailableReason = unavailableBefore
    ?? (completedAdmission?.receiverEpoch === snapshot!.receiverEpoch
      && snapshot!.authoringAdmission.admissionGeneration <= completedAdmission.generation
      ? 'Waiting for updated backend authoring permissions.'
      : authoring.unavailableReason
        ?? (workspace === null ? 'Deployment workspace is not loaded.' : null));
  const published = authoring.published;
  /** The complete activation input of the installed publication. */
  const activationScope = useMemo(() => deploymentActivationScope(published), [published]);

  useEffect(() => {
    if (workspace === null || published === null) return;
    // A mutation response can beat the control delta; never revert it.
    if (!publicationSupersedesDeployment(published, workspace)) return;
    // Deployment Draft and Saved are authoring truth and come from the one
    // shared publication; the Manager/Applied projection stays with this owner
    // read.
    const merged = withSharedDeploymentAuthoring(workspace, published.snapshot);
    if (
      merged.draft.id === workspace.draft.id
      && merged.draft.revision === workspace.draft.revision
      && merged.draft.document === workspace.draft.document
      && merged.saved.path === workspace.saved.path
      && merged.saved.deploymentRevision === workspace.saved.deploymentRevision
    ) {
      return;
    }
    setWorkspace(merged);
  }, [published, workspace]);

  const fence = useCallback((
    kind: 'edit' | 'document',
    expected?: ExpectedDeploymentDraft,
  ): LocalWorkspaceFenceRequest | null => {
    if (
      (kind === 'edit' ? mutationUnavailableReason : documentUnavailableReason) !== null
      || snapshot === null
      || published === null
      || workspace === null
    ) return null;
    if (expected !== undefined && (
      published.identity.deploymentDraftId !== expected.draftId
      || published.identity.deploymentDraftRevision !== expected.draftRevision
    )) {
      throw new DeploymentDraftChangedError(
        `The Deployment Draft changed (revision ${published.identity.deploymentDraftRevision}, expected ${expected.draftRevision}).`,
      );
    }
    return programLocalFence(
      published,
      snapshot,
      ownerEpoch.current,
      requestId(),
    );
  }, [documentUnavailableReason, mutationUnavailableReason, published, snapshot, workspace]);

  const run = useCallback(async (
    operation: (requestFence: LocalWorkspaceFenceRequest) => Promise<DeploymentWorkspaceResponse>,
    message: string,
    kind: 'edit' | 'document',
    action?: 'discard',
  ) => {
    if (action && deploymentActionUnavailableReason(workspace, action, mutationUnavailableReason) !== null) return null;
    const requestFence = fence(kind);
    if (requestFence === null || snapshot === null || inFlight.current) return null;
    if (!authoring.beginMutation(kind)) return null;
    inFlight.current = true;
    setBusy(true);
    setError(null);
    setFeedback(null);
    try {
      const next = await operation(requestFence);
      authoring.finishMutation();
      if (mounted.current) {
        // The HTTP post-image can arrive before the shared status stream.
        // Never submit the next claimed action with the just-consumed
        // admission fence (a Draft edit takes no claim and consumes none).
        if (kind === 'document') {
          setCompletedAdmission({
            receiverEpoch: snapshot.receiverEpoch,
            generation: requestFence.expectedAdmissionGeneration,
          });
        }
        // A read that started before this mutation may still be in flight and
        // must not replace the newer owner-issued post-image when it arrives.
        refreshSequence.current += 1;
        setWorkspace(next);
        setFeedback(message);
      }
      return next;
    } catch (reason: unknown) {
      authoring.finishMutation(null, reason);
      const message = errorMessage(reason, 'Deployment authoring request failed.');
      // A stale fence is also refused before any effect, but it needs the
      // refresh below, so it is never handled as a plain refusal.
      const refusal = isStaleWorkspaceError(reason) ? null : authoringRefusalMessage(reason);
      if (refusal !== null) {
        if (mounted.current) setFeedback(refusal);
        return null;
      }
      if (isStaleWorkspaceError(reason)) {
        const latest = await refresh();
        if (mounted.current && latest !== null) setError(message);
      } else if (mounted.current) {
        setError(message);
      }
      return null;
    } finally {
      inFlight.current = false;
      if (mounted.current) setBusy(false);
    }
  }, [authoring, fence, refresh, snapshot, workspace, mutationUnavailableReason]);

  const runApply = useCallback(async (
    operation: (
      requestFence: LocalWorkspaceFenceRequest,
      scope: DeploymentActivationScope | null,
    ) => Promise<DeploymentCandidateOperationResponse>,
    message: string,
    action: 'apply' | 'revert' = 'apply',
    expected?: ExpectedDeploymentDraft,
    /** The activation scope the operator confirmed; it must be unchanged. */
    expectedScope?: DeploymentActivationScope | null,
    correlationId?: string,
  ) => {
    if (deploymentActionUnavailableReason(workspace, action, documentUnavailableReason) !== null) return null;
    const requestFence = fence('document', expected);
    if (requestFence === null || snapshot === null || inFlight.current) return null;
    if (expectedScope != null && !sameActivationScope(expectedScope, activationScope)) {
      throw new DeploymentDraftChangedError(ACTIVATION_SCOPE_CHANGED);
    }
    const scope = expectedScope ?? activationScope;
    if (!authoring.beginMutation()) return null;
    inFlight.current = true;
    setBusy(true);
    setApplying(true);
    setError(null);
    setFeedback(null);
    try {
      const response = await operation(requestFence, scope);
      const accepted = response.result.status === 'accepted' ? response.result.managerApply : null;
      authoring.finishMutation(accepted === null ? null : {
        applyId: accepted.applyId, applyGeneration: accepted.applyGeneration,
      });
      if (mounted.current) {
        setCompletedAdmission({
          receiverEpoch: snapshot.receiverEpoch,
          generation: requestFence.expectedAdmissionGeneration,
        });
        refreshSequence.current += 1;
        setWorkspace(response.workspace);
        if (response.result.status === 'confirmation_required') {
          pendingConfirmationContext.current = { correlationId, scope };
          setPendingApplyConfirmation(response);
          setFeedback(null);
        } else {
          pendingConfirmationContext.current = null;
          setPendingApplyConfirmation(null);
          const managerApply = response.result.managerApply;
          if (response.result.status === 'accepted' && managerApply !== null) {
            setAcceptedApply({
              applyId: managerApply.applyId,
              applyGeneration: managerApply.applyGeneration,
            });
            setAcceptedApplyCorrelationId(correlationId ?? null);
          }
          setFeedback(
            response.result.status === 'accepted'
              ? 'Deployment activation accepted; progress is shown under Activity.'
              : message,
          );
        }
      }
      return response;
    } catch (reason: unknown) {
      authoring.finishMutation(null, reason);
      const message = errorMessage(
        reason,
        'Deployment Apply request failed.',
      );
      // Confirmation is a one-shot fence for one exact compiled candidate.
      // A terminal request failure must return the UI to the ordinary Apply
      // action instead of leaving a disabled confirmation overlay behind.
      if (mounted.current) setPendingApplyConfirmation(null);
      pendingConfirmationContext.current = null;
      // A stale fence is also refused before any effect, but it needs the
      // refresh below, so it is never handled as a plain refusal.
      const refusal = isStaleWorkspaceError(reason) ? null : authoringRefusalMessage(reason);
      if (refusal !== null) {
        if (mounted.current) setFeedback(refusal);
        return null;
      }
      if (isStaleWorkspaceError(reason)) {
        const latest = await refresh();
        if (mounted.current && latest !== null) setError(message);
      } else if (mounted.current) {
        setError(message);
      }
      return null;
    } finally {
      inFlight.current = false;
      if (mounted.current) setBusy(false);
      if (mounted.current) setApplying(false);
    }
  }, [activationScope, authoring, documentUnavailableReason, fence, refresh, snapshot, workspace]);

  const actions = useMemo(() => ({
    open: (path: string, confirmDiscard: boolean) => run(
      (requestFence) => api.deploymentWorkspace.openDraft(
        requestFence, path, confirmDiscard,
      ),
      'Deployment opened as Draft; Manager Applied was unchanged.',
      'document',
    ),
    mutate: (
      path: Array<string | number>,
      operation: 'set' | 'append' | 'remove',
      value?: JsonValue,
    ) => run(
      (requestFence) => api.deploymentWorkspace.mutate(
        requestFence, path, operation, value,
      ),
      'Deployment Draft updated; Manager Applied was unchanged.',
      'edit',
    ),
    updateControlledStop: (update: DeploymentControlledStopUpdate) => run(
      (requestFence) => api.deploymentWorkspace.updateControlledStop(
        requestFence, update,
      ),
      'Controlled Stop Draft updated atomically; Manager Applied was unchanged.',
      'edit',
    ),
    updateMotionTargetLimit: (
      controllableComponentId: string,
      commandTargetId: string,
      maximumTcpLinearSpeedMps: number,
    ) => run(
      (requestFence) => api.deploymentWorkspace.updateMotionTargetLimit(
        requestFence,
        controllableComponentId,
        commandTargetId,
        maximumTcpLinearSpeedMps,
      ),
      'Target maximum TCP speed updated in Draft; Manager Applied was unchanged.',
      'edit',
    ),
    save: () => run(
      (requestFence) => api.deploymentWorkspace.save(requestFence),
      'Deployment Draft saved; Manager Applied was unchanged.',
      'document',
    ),
    saveAs: (path: string, confirmOverwrite: boolean) => run(
      (requestFence) => api.deploymentWorkspace.saveAs(
        requestFence, path, confirmOverwrite,
      ),
      'Deployment Draft saved to the selected path; Manager Applied was unchanged.',
      'document',
    ),
    discard: () => run(
      (requestFence) => api.deploymentWorkspace.discard(requestFence),
      'Deployment Draft changes discarded; Manager Applied was unchanged.',
      'edit',
      'discard',
    ),
    revert: () => runApply(
      (requestFence) => api.deploymentWorkspace.revert(requestFence),
      'Saved Deployment was already current in Manager.',
      'revert',
    ),
    /**
     * The ordinary Apply (Idle only; with a run, the Apply-after-Reset chain
     * calls this once the run has ended). With `expectedDraft`, only exactly
     * that Draft is Applied; otherwise it rejects with
     * `DeploymentDraftChangedError` before anything is sent.
     */
    applyDraft: (
      options: {
        correlationId?: string;
        expectedDraft?: ExpectedDeploymentDraft;
        /** The complete activation input the operator confirmed. */
        expectedScope?: DeploymentActivationScope | null;
      } = {},
    ) => {
      if (documentUnavailableReason !== null || inFlight.current) return Promise.resolve(null);
      pendingConfirmationContext.current = null;
      setPendingApplyConfirmation(null);
      return runApply(
        (requestFence, scope) => api.deploymentWorkspace.applyDraft(
          requestFence, null, undefined, options.correlationId, scope,
        ),
        'Deployment Draft compiled, applied by Manager, and rebound to the current Program.',
        'apply',
        options.expectedDraft,
        options.expectedScope,
        options.correlationId,
      );
    },
    confirmApply: () => {
      if (documentUnavailableReason !== null || inFlight.current) return Promise.resolve(null);
      const token = pendingApplyConfirmation?.result.confirmationToken ?? null;
      const preparation = pendingApplyConfirmation?.result.preparation ?? null;
      if (token === null || preparation === null) return Promise.resolve(null);
      // Confirmation is only the user's authorization boundary. Once the
      // exact token and preparation identity have been captured, close the
      // overlay before dispatching the potentially long-running Apply. The
      // ordinary workspace busy/error state owns progress from this point.
      // The confirmed impact belongs to the exact input the first Apply sent
      // and to its shared Reset/Apply correlation: carry both to the Apply.
      const context = pendingConfirmationContext.current;
      pendingConfirmationContext.current = null;
      setPendingApplyConfirmation(null);
      return runApply(
        (requestFence, scope) => api.deploymentWorkspace.applyDraft(
          requestFence, { token, preparation }, undefined, context?.correlationId, scope,
        ),
        'Deployment Draft compiled, applied by Manager, and rebound to the current Program.',
        'apply',
        undefined,
        context?.scope ?? null,
        context?.correlationId,
      );
    },
    cancelApplyConfirmation: () => {
      pendingConfirmationContext.current = null;
      setPendingApplyConfirmation(null);
    },
  }), [documentUnavailableReason, pendingApplyConfirmation, run, runApply]);

  return {
    workspace,
    loading,
    busy,
    applying,
    error,
    feedback,
    acceptedApply,
    acceptedApplyCorrelationId,
    activationScope,
    pendingApplyConfirmation,
    mutationAvailable,
    mutationUnavailableReason,
    documentUnavailableReason,
    refresh,
    clearFeedback: () => setFeedback(null),
    dismissError: () => setError(null),
    ...actions,
  };
}

export type DeploymentWorkspaceController = ReturnType<typeof useDeploymentWorkspace>;
