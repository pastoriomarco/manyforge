import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api, isStaleWorkspaceError } from '../api/client';
import { errorMessage } from '../state/structuralResource';
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
  const [pendingApplyConfirmation, setPendingApplyConfirmation] = useState<
    DeploymentCandidateOperationResponse | null
  >(null);
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

  const mutationUnavailableReason = !enabled
    ? 'Deployment authoring requires the configured headless product.'
    : snapshot === null
      ? 'Backend status is disconnected or has not arrived.'
      : completedAdmission?.receiverEpoch === snapshot.receiverEpoch
          && snapshot.authoringAdmission.admissionGeneration <= completedAdmission.generation
          ? 'Waiting for updated backend authoring permissions.'
          : authoring.unavailableReason
            ?? (workspace === null
              ? 'Deployment workspace is not loaded.'
              : null);
  const mutationAvailable = mutationUnavailableReason === null;
  const published = authoring.published;

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

  const fence = useCallback((): LocalWorkspaceFenceRequest | null => {
    if (
      !mutationAvailable
      || snapshot === null
      || published === null
      || workspace === null
    ) return null;
    return programLocalFence(
      published,
      snapshot,
      ownerEpoch.current,
      requestId(),
    );
  }, [mutationAvailable, published, snapshot, workspace]);

  const run = useCallback(async (
    operation: (requestFence: LocalWorkspaceFenceRequest) => Promise<DeploymentWorkspaceResponse>,
    message: string,
  ) => {
    const requestFence = fence();
    if (requestFence === null || snapshot === null || inFlight.current) return null;
    inFlight.current = true;
    setBusy(true);
    setError(null);
    setFeedback(null);
    try {
      const next = await operation(requestFence);
      if (mounted.current) {
        // The HTTP post-image can arrive before the shared status stream.
        // Never submit the next edit with the just-consumed admission fence.
        setCompletedAdmission({
          receiverEpoch: snapshot.receiverEpoch,
          generation: requestFence.expectedAdmissionGeneration,
        });
        // A read that started before this mutation may still be in flight and
        // must not replace the newer owner-issued post-image when it arrives.
        refreshSequence.current += 1;
        setWorkspace(next);
        setFeedback(message);
      }
      return next;
    } catch (reason: unknown) {
      const message = errorMessage(reason, 'Deployment authoring request failed.');
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
  }, [fence, refresh, snapshot]);

  const runApply = useCallback(async (
    operation: (
      requestFence: LocalWorkspaceFenceRequest,
    ) => Promise<DeploymentCandidateOperationResponse>,
    message: string,
  ) => {
    const requestFence = fence();
    if (requestFence === null || snapshot === null || inFlight.current) return null;
    inFlight.current = true;
    setBusy(true);
    setApplying(true);
    setError(null);
    setFeedback(null);
    try {
      const response = await operation(requestFence);
      if (mounted.current) {
        setCompletedAdmission({
          receiverEpoch: snapshot.receiverEpoch,
          generation: requestFence.expectedAdmissionGeneration,
        });
        refreshSequence.current += 1;
        setWorkspace(response.workspace);
        if (response.result.status === 'confirmation_required') {
          setPendingApplyConfirmation(response);
          setFeedback(null);
        } else {
          setPendingApplyConfirmation(null);
          setFeedback(
            response.result.status === 'accepted'
              ? 'Deployment activation accepted; progress is shown in runtime status.'
              : message,
          );
        }
      }
      return response;
    } catch (reason: unknown) {
      const message = errorMessage(
        reason,
        'Deployment Apply request failed.',
      );
      // Confirmation is a one-shot fence for one exact compiled candidate.
      // A terminal request failure must return the UI to the ordinary Apply
      // action instead of leaving a disabled confirmation overlay behind.
      if (mounted.current) setPendingApplyConfirmation(null);
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
  }, [fence, refresh, snapshot]);

  const actions = useMemo(() => ({
    open: (path: string, confirmDiscard: boolean) => run(
      (requestFence) => api.deploymentWorkspace.openDraft(
        requestFence, path, confirmDiscard,
      ),
      'Deployment opened as Draft; Manager Applied was unchanged.',
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
    ),
    updateControlledStop: (update: DeploymentControlledStopUpdate) => run(
      (requestFence) => api.deploymentWorkspace.updateControlledStop(
        requestFence, update,
      ),
      'Controlled Stop Draft updated atomically; Manager Applied was unchanged.',
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
    ),
    save: () => run(
      (requestFence) => api.deploymentWorkspace.save(requestFence),
      'Deployment Draft saved; Manager Applied was unchanged.',
    ),
    saveAs: (path: string, confirmOverwrite: boolean) => run(
      (requestFence) => api.deploymentWorkspace.saveAs(
        requestFence, path, confirmOverwrite,
      ),
      'Deployment Draft saved to the selected path; Manager Applied was unchanged.',
    ),
    discard: () => run(
      (requestFence) => api.deploymentWorkspace.discard(requestFence),
      'Deployment Draft changes discarded; Manager Applied was unchanged.',
    ),
    revert: () => runApply(
      (requestFence) => api.deploymentWorkspace.revert(requestFence),
      'Saved Deployment was already current in Manager.',
    ),
    applyDraft: () => {
      setPendingApplyConfirmation(null);
      return runApply(
        (requestFence) => api.deploymentWorkspace.applyDraft(requestFence),
        'Deployment Draft compiled, applied by Manager, and rebound to the current Program.',
      );
    },
    confirmApply: () => {
      const token = pendingApplyConfirmation?.result.confirmationToken ?? null;
      const preparation = pendingApplyConfirmation?.result.preparation ?? null;
      if (token === null || preparation === null) return Promise.resolve(null);
      // Confirmation is only the user's authorization boundary. Once the
      // exact token and preparation identity have been captured, close the
      // overlay before dispatching the potentially long-running Apply. The
      // ordinary workspace busy/error state owns progress from this point.
      setPendingApplyConfirmation(null);
      return runApply(
        (requestFence) => api.deploymentWorkspace.applyDraft(
          requestFence, { token, preparation },
        ),
        'Deployment Draft compiled, applied by Manager, and rebound to the current Program.',
      );
    },
    cancelApplyConfirmation: () => setPendingApplyConfirmation(null),
  }), [pendingApplyConfirmation, run, runApply]);

  return {
    workspace,
    loading,
    busy,
    applying,
    error,
    feedback,
    pendingApplyConfirmation,
    mutationAvailable,
    mutationUnavailableReason,
    refresh,
    clearFeedback: () => setFeedback(null),
    dismissError: () => setError(null),
    ...actions,
  };
}

export type DeploymentWorkspaceController = ReturnType<typeof useDeploymentWorkspace>;
