import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import {
  api, apiErrorCode, authoringRefusalMessage, isStaleWorkspaceError, preparationCauseCode,
} from '../api/client';
import { applyResultText } from '../components/ApplyImpact';
import type { AuthoringCommand } from '../state/busyOperation';
import { codedErrorText } from '../state/componentPresentation';
import { errorMessage } from '../state/structuralResource';
import type { ApplyPreview, ApplyResult } from '../types/components';
import {
  deploymentComparisonsStale,
  deploymentLocalFence,
  publicationSupersedesDeployment,
  DEPLOYMENT_RECHECKING_REASON,
  withSharedDeploymentAuthoring,
} from '../state/authoringWorkspace';
import type {
  DeploymentWorkspaceResponse,
  DeploymentControlledStopUpdate,
  JsonValue,
  LocalWorkspaceFenceRequest,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { browserStableIdentity } from './useHeadlessState';
import { deploymentActionUnavailableReason } from '../state/documentStates';

/**
 * The text of a refused Preview/Apply/Load/New: the
 * catalogue's text for its code, e.g. `apply_refused_run_in_progress`.
 */
export function cellApplyErrorText(reason: unknown, fallback: string): string {
  const message = errorMessage(reason, fallback);
  const code = apiErrorCode(reason);
  return code === null ? message : codedErrorText({ code, message });
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
  const [deploymentDraftInputResetEpoch, setDeploymentDraftInputResetEpoch] = useState(0);
  const [scenarioDraftInputResetEpoch, setScenarioDraftInputResetEpoch] = useState(0);
  const [loading, setLoading] = useState(enabled);
  const [busy, setBusy] = useState(false);
  const [applying, setApplying] = useState(false);
  const [completedAdmission, setCompletedAdmission] = useState<{
    receiverEpoch: string;
    generation: number;
  } | null>(null);
  const [error, setError] = useState<string | null>(null);
  /** The typed cause of the shown preparation refusal, if the preparer named one. */
  const [errorCauseCode, setErrorCauseCode] = useState<string | null>(null);
  const [feedback, setFeedback] = useState<string | null>(null);
  /** The Preview the operator confirms before the server-side Apply. */
  const [applyPreview, setApplyPreview] = useState<ApplyPreview | null>(null);
  /** Whether the shown Preview is of the Draft's Apply or of a Revert to Saved. */
  const [applyPreviewKind, setApplyPreviewKind] = useState<'apply' | 'revert'>('apply');
  /**
   * The Apply (or Revert) confirmation is open: it opens on click and shows
   * "Previewing impact…" until its Preview arrives.
   */
  const [applyConfirmationOpen, setApplyConfirmationOpen] = useState(false);
  const confirmationOpen = useRef(false);
  const openConfirmation = useCallback((open: boolean) => {
    confirmationOpen.current = open;
    setApplyConfirmationOpen(open);
    if (!open) setApplyPreview(null);
  }, []);
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
    setErrorCauseCode(null);
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
      setErrorCauseCode(null);
      setFeedback(null);
      openConfirmation(false);
    }
    return () => { mounted.current = false; };
  }, [enabled, openConfirmation, refresh]);

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
  // While the owner is read again after a publication, its comparisons are
  // unknown, so no Scenario action is offered; a failed read says so until a
  // later read succeeds. Draft edits stay available.
  const comparisonUnavailableReason = workspace?.runAdmission.reason !== DEPLOYMENT_RECHECKING_REASON
    ? null
    : !loading && error !== null
      ? `The Deployment could not be rechecked against Manager Applied: ${error} Refresh to retry.`
      : `${DEPLOYMENT_RECHECKING_REASON}…`;
  const published = authoring.published;

  useEffect(() => {
    if (workspace === null || published === null) return;
    // A mutation response can beat the control delta; never revert it.
    if (!publicationSupersedesDeployment(published, workspace)) return;
    // Deployment Draft and Saved are authoring truth and come from the one
    // shared publication; the Manager/Applied projection stays with this owner
    // read; when the publication outdates it, the owner is read again.
    const stale = deploymentComparisonsStale(workspace, published.snapshot);
    const merged = withSharedDeploymentAuthoring(workspace, published.snapshot);
    if (
      !stale
      && merged.draft.id === workspace.draft.id
      && merged.draft.revision === workspace.draft.revision
      && merged.draft.document === workspace.draft.document
      && merged.saved.path === workspace.saved.path
      && merged.saved.deploymentRevision === workspace.saved.deploymentRevision
    ) {
      return;
    }
    setWorkspace(merged);
    if (stale) void refresh();
  }, [published, refresh, workspace]);

  const fence = useCallback((
    kind: 'edit' | 'document',
  ): LocalWorkspaceFenceRequest | null => {
    if (
      (kind === 'edit' ? mutationUnavailableReason : documentUnavailableReason) !== null
      || snapshot === null
      || published === null
      || workspace === null
    ) return null;
    return deploymentLocalFence(
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
    command: AuthoringCommand,
    action?: 'discard',
  ) => {
    if (action && deploymentActionUnavailableReason(workspace, action, mutationUnavailableReason) !== null) return null;
    const requestFence = fence(kind);
    if (requestFence === null || snapshot === null || inFlight.current) return null;
    if (!authoring.beginMutation(kind, command)) return null;
    inFlight.current = true;
    setBusy(true);
    setError(null);
    setErrorCauseCode(null);
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
        if (action === 'discard' || command === 'open_draft') {
          setDeploymentDraftInputResetEpoch((epoch) => epoch + 1);
        }
      }
      return next;
    } catch (reason: unknown) {
      authoring.finishMutation(null, reason);
      if (mounted.current) setErrorCauseCode(preparationCauseCode(reason));
      const message = errorMessage(reason, 'Deployment authoring request failed.');
      // A stale fence is also refused before any effect, but it needs the
      // refresh below, so it is never handled as a plain refusal.
      const refusal = isStaleWorkspaceError(reason) ? null : authoringRefusalMessage(reason);
      if (refusal !== null) {
        if (mounted.current) setError(refusal);
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

  /**
   * The one server-side Apply: the server disables, applies
   * and prepares the affected components; they end prepared. Refused while a
   * Program run is in progress; allowed during a cell latch.
   */
  const sendApply = useCallback(async (
    operation: () => Promise<ApplyResult>,
    failure: string,
    command: 'apply' | 'revert',
  ) => {
    // The server decides admission; the claim only names the operation and
    // holds every conflicting command until the result arrives.
    if (inFlight.current || !authoring.beginMutation('edit', command)) return null;
    inFlight.current = true;
    setBusy(true);
    setApplying(true);
    setError(null);
    setErrorCauseCode(null);
    setFeedback(null);
    // The workspace is read again before the outcome is shown: the read
    // clears the error it last showed.
    try {
      const result = await operation();
      const refreshed = await refresh();
      if (mounted.current) {
        if (result.outcome === 'failed') setError(applyResultText(result));
        else setFeedback(applyResultText(result));
        // Revert is Scenario-wide; require both owner success and its refreshed
        // workspace post-image before clearing pending field text.
        if (command === 'revert' && result.outcome === 'succeeded' && refreshed !== null) {
          setScenarioDraftInputResetEpoch((epoch) => epoch + 1);
        }
      }
      return result;
    } catch (reason: unknown) {
      await refresh();
      if (mounted.current) {
        setError(cellApplyErrorText(reason, failure));
        setErrorCauseCode(preparationCauseCode(reason));
      }
      return null;
    } finally {
      authoring.finishMutation();
      inFlight.current = false;
      if (mounted.current) {
        setBusy(false);
        setApplying(false);
      }
    }
  }, [authoring, refresh]);

  /** An effect-free Preview, held as one operation like any other. */
  const preview = useCallback(async (
    kind: 'apply' | 'revert',
    command: 'preview' | 'apply_preview' | 'revert_preview',
  ): Promise<ApplyPreview | null> => {
    if (inFlight.current || !authoring.beginMutation(kind === 'revert' ? 'document' : 'edit', command)) return null;
    inFlight.current = true;
    setBusy(true);
    try {
      return kind === 'revert' ? await api.cellApply.revertPreview() : await api.cellApply.preview();
    } finally {
      authoring.finishMutation();
      inFlight.current = false;
      if (mounted.current) setBusy(false);
    }
  }, [authoring]);

  const showPreview = useCallback(async (kind: 'apply' | 'revert'): Promise<ApplyPreview | null> => {
    if (inFlight.current) return null;
    setError(null);
    setFeedback(null);
    setApplyPreviewKind(kind);
    openConfirmation(true);
    try {
      const shown = await preview(kind, kind === 'revert' ? 'revert_preview' : 'apply_preview');
      // Cancelled while previewing: the late Preview is not shown.
      if (mounted.current && confirmationOpen.current) {
        if (shown === null) openConfirmation(false);
        else setApplyPreview(shown);
      }
      return shown;
    } catch (reason: unknown) {
      if (mounted.current) {
        openConfirmation(false);
        setError(cellApplyErrorText(reason, kind === 'revert'
          ? 'Deployment Revert preview request failed.' : 'Deployment Preview request failed.'));
      }
      return null;
    }
  }, [openConfirmation, preview]);

  const actions = useMemo(() => ({
    open: (path: string, confirmDiscard: boolean) => run(
      (requestFence) => api.deploymentWorkspace.openDraft(
        requestFence, path, confirmDiscard,
      ),
      'Deployment opened as Draft; Manager Applied was unchanged.',
      'document',
      'open_draft',
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
      'edit',
    ),
    /**
     * Add complete component rows (Add component) and the attachment rows
     * that mount a new tool, in one Draft edit. The backend admits only a pure
     * addition; the components run only after the one confirmed Deployment
     * Apply.
     */
    addComponents: (
      rows: Array<Record<string, JsonValue>>,
      attachments: Array<Record<string, JsonValue>> = [],
    ) => run(
      (requestFence) => api.deploymentWorkspace.addComponents(requestFence, rows, attachments),
      'Component added to the Deployment Draft; it runs only after Apply.',
      'edit',
      'edit',
    ),
    /**
     * The per-component impact of applying the Draft; effect-free.
     * Null when another operation is in progress.
     */
    previewApply: (): Promise<ApplyPreview | null> => preview('apply', 'preview'),
    updateControlledStop: (update: DeploymentControlledStopUpdate) => run(
      (requestFence) => api.deploymentWorkspace.updateControlledStop(
        requestFence, update,
      ),
      'Controlled Stop Draft updated atomically; Manager Applied was unchanged.',
      'edit',
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
      'edit',
    ),
    save: () => run(
      (requestFence) => api.deploymentWorkspace.save(requestFence),
      'Deployment Draft saved; Manager Applied was unchanged.',
      'document',
      'save',
    ),
    saveAs: (path: string, confirmOverwrite: boolean) => run(
      (requestFence) => api.deploymentWorkspace.saveAs(
        requestFence, path, confirmOverwrite,
      ),
      'Deployment Draft saved to the selected path; Manager Applied was unchanged.',
      'document',
      'save_as',
    ),
    discard: () => run(
      (requestFence) => api.deploymentWorkspace.discard(requestFence),
      'Deployment Draft changes discarded; Manager Applied was unchanged.',
      'edit',
      'discard',
      'discard',
    ),
    /**
     * Revert on the one Apply path: Preview the Saved Scenario's impact,
     * then the operator confirms it (`confirmApply`), which applies it.
     */
    revert: () => (
      deploymentActionUnavailableReason(workspace, 'revert', documentUnavailableReason) !== null
        ? Promise.resolve(null)
        : showPreview('revert')
    ),
    /**
     * Apply: Preview the impact (effect-free, so also while a run is held),
     * then the operator confirms it (`confirmApply`, or Reset and Apply).
     * Resolves to the Preview, or null when it was refused.
     */
    applyDraft: () => (mutationUnavailableReason !== null ? Promise.resolve(null) : showPreview('apply')),
    /**
     * The Apply of a Preview confirmed earlier, sent by Reset and Apply once
     * the run's Reset settled. Resolves to null when it was refused.
     */
    applyConfirmedScope: (scope: ApplyPreview['scope']) => (
      sendApply(() => api.cellApply.apply(scope), 'Deployment Apply request failed.', 'apply')
    ),
    /**
     * Send the Apply (or Revert) the operator confirmed, bound to the Preview
     * shown: the server refuses it when the input changed since.
     */
    confirmApply: () => {
      if (applyPreview === null) return Promise.resolve(null);
      const { scope } = applyPreview;
      openConfirmation(false);
      return applyPreviewKind === 'revert'
        ? sendApply(() => api.cellApply.revert(scope), 'Deployment Revert request failed.', 'revert')
        : sendApply(() => api.cellApply.apply(scope), 'Deployment Apply request failed.', 'apply');
    },
    cancelApplyConfirmation: () => openConfirmation(false),
  }), [applyPreview, applyPreviewKind, documentUnavailableReason, mutationUnavailableReason, openConfirmation, preview, run, sendApply, showPreview, workspace]);

  return {
    workspace,
    deploymentDraftInputResetEpoch,
    scenarioDraftInputResetEpoch,
    loading,
    busy,
    applying,
    error,
    errorCauseCode,
    feedback,
    /** The authoring operation in progress (busy label and tooltip), or null. */
    busyOperation: authoring.operation,
    applyConfirmationOpen,
    applyPreview,
    applyPreviewKind,
    mutationAvailable,
    mutationUnavailableReason,
    documentUnavailableReason,
    comparisonUnavailableReason,
    refresh,
    clearFeedback: () => setFeedback(null),
    dismissError: () => {
      setError(null);
      setErrorCauseCode(null);
    },
    ...actions,
  };
}

export type DeploymentWorkspaceController = ReturnType<typeof useDeploymentWorkspace>;
