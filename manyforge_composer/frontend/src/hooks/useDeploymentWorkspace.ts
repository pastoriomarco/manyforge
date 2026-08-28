import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api, ApiError } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type {
  DeploymentWorkspaceResponse,
  DraftFenceRequest,
  JsonValue,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { browserStableIdentity } from './useHeadlessState';

function requestId(): string {
  return `composer-deployment.${globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`}`;
}

function isStaleDraftError(error: unknown): boolean {
  if (!(error instanceof ApiError) || error.status !== 409) return false;
  const payload = error.payload;
  if (payload === null || typeof payload !== 'object' || Array.isArray(payload)) {
    return false;
  }
  const detail = (payload as Record<string, unknown>).detail;
  return detail !== null
    && typeof detail === 'object'
    && !Array.isArray(detail)
    && (detail as Record<string, unknown>).code === 'deployment_draft_stale';
}

export function useDeploymentWorkspace(
  enabled: boolean,
  snapshot: ComposerHeadlessState | null,
) {
  const [workspace, setWorkspace] = useState<DeploymentWorkspaceResponse | null>(null);
  const [loading, setLoading] = useState(enabled);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [feedback, setFeedback] = useState<string | null>(null);
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
    }
    return () => { mounted.current = false; };
  }, [enabled, refresh]);

  const mutationUnavailableReason = !enabled
    ? 'Deployment authoring requires the configured headless product.'
    : snapshot === null
      ? 'Backend status is disconnected or has not arrived.'
      : snapshot.programCycle.state !== 'idle_after_reset'
        ? `Program execution is ${snapshot.programCycle.state.replaceAll('_', ' ')}; Deployment authoring requires Idle.`
        : !snapshot.authoringAdmission.authoringAllowed
          ? 'The backend has not admitted authoring; execution must be Idle and required components healthy.'
          : workspace === null
            ? 'Deployment workspace is not loaded.'
            : null;
  const mutationAvailable = mutationUnavailableReason === null;

  const fence = useCallback((): DraftFenceRequest | null => {
    if (!mutationAvailable || workspace === null || snapshot === null) return null;
    return {
      draftId: workspace.draft.id,
      expectedDraftRevision: workspace.draft.revision,
      expectedAdmissionGeneration: snapshot.authoringAdmission.admissionGeneration,
      requestOwnerEpoch: ownerEpoch.current,
      requestId: requestId(),
    };
  }, [mutationAvailable, snapshot, workspace]);

  const run = useCallback(async (
    operation: (requestFence: DraftFenceRequest) => Promise<DeploymentWorkspaceResponse>,
    message: string,
  ) => {
    const requestFence = fence();
    if (requestFence === null || inFlight.current) return null;
    inFlight.current = true;
    setBusy(true);
    setError(null);
    setFeedback(null);
    try {
      const next = await operation(requestFence);
      if (mounted.current) {
        // A read that started before this mutation may still be in flight and
        // must not replace the newer owner-issued post-image when it arrives.
        refreshSequence.current += 1;
        setWorkspace(next);
        setFeedback(message);
      }
      return next;
    } catch (reason: unknown) {
      const message = errorMessage(reason, 'Deployment authoring request failed.');
      if (isStaleDraftError(reason)) {
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
  }, [fence, refresh]);

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
  }), [run]);

  return {
    workspace,
    loading,
    busy,
    error,
    feedback,
    mutationAvailable,
    mutationUnavailableReason,
    refresh,
    clearFeedback: () => setFeedback(null),
    ...actions,
  };
}

export type DeploymentWorkspaceController = ReturnType<typeof useDeploymentWorkspace>;
