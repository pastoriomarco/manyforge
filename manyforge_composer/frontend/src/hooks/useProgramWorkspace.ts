import { useCallback, useEffect, useMemo, useReducer, useRef } from 'react';

import { api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
  type ProgramWorkspaceActionName,
} from '../state/programWorkspace';
import type {
  DraftMutationResponse,
  JsonValue,
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramTreeNode,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  ProgramWorkspaceTransitionStatus,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { browserStableIdentity } from './useHeadlessState';

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

function fence(
  workspace: ProgramWorkspaceResponse | null,
  snapshot: ComposerHeadlessState | null,
  requestOwnerEpoch: string,
) {
  if (
    workspace === null
    || snapshot === null
    || snapshot.draftIdentity.draftId !== workspace.draft.draftId
    || snapshot.draftIdentity.draftRevision !== workspace.draft.draftRevision
    || !snapshot.authoringAdmission.authoringAllowed
    || snapshot.programCycle.state !== 'idle_after_reset'
  ) return null;
  return {
    draftId: workspace.draft.draftId,
    expectedDraftRevision: workspace.draft.draftRevision,
    expectedAdmissionGeneration: snapshot.authoringAdmission.admissionGeneration,
    requestOwnerEpoch,
    requestId: identity('composer-authoring'),
  };
}

interface ProgramWorkspaceRunResult {
  workspace: ProgramWorkspaceResponse;
  message: string | null;
  transitionStatus: ProgramWorkspaceTransitionStatus | null;
}

function workspaceResult(
  workspace: ProgramWorkspaceResponse,
  message: string | null,
): ProgramWorkspaceRunResult {
  return { workspace, message, transitionStatus: null };
}

export function programWorkspaceTransitionResult(
  action: 'apply' | 'load' | 'revert',
  response: ProgramWorkspaceTransitionResponse,
): ProgramWorkspaceRunResult {
  if (action === 'revert' && response.status === 'reverted') {
    return {
      workspace: response.workspace,
      message: response.headless === null
        ? 'Saved already matched Applied; Draft now matches Saved. No owner Apply was issued.'
        : 'Revert completed through authoritative Apply; Draft now matches Saved.',
      transitionStatus: response.status,
    };
  }
  if (action === 'load' && response.status === 'loaded') {
    return {
      workspace: response.workspace,
      message: 'Scenario loaded; its Deployment is converged and its Program is Applied.',
      transitionStatus: response.status,
    };
  }
  if (action === 'apply' && response.status === 'applied') {
    return {
      workspace: response.workspace,
      message: 'Apply completed; the owner-issued Applied state was replaced.',
      transitionStatus: response.status,
    };
  }
  throw new Error(`Unexpected ${response.status} status for ${action}.`);
}

export function useProgramWorkspace(snapshot: ComposerHeadlessState | null = null) {
  const [state, dispatch] = useReducer(
    programWorkspaceReducer,
    INITIAL_PROGRAM_WORKSPACE_STATE,
  );
  const generation = useRef(0);
  const mounted = useRef(true);
  const workspaceRead = useRef<Promise<ProgramWorkspaceResponse> | null>(null);
  const workspaceReadGeneration = useRef<number | null>(null);
  const initialReadStarted = useRef(false);
  const requestedSnapshotIdentity = useRef<string | null>(null);
  const requestOwnerEpoch = useRef(browserStableIdentity(
    'manyforge.composer.request_owner_epoch',
    'composer-authoring-owner',
  ));
  const mutationAvailable = state.workspace !== null
    && snapshot !== null
    && snapshot.draftIdentity.draftId === state.workspace.draft.draftId
    && snapshot.draftIdentity.draftRevision === state.workspace.draft.draftRevision
    && snapshot.authoringAdmission.authoringAllowed
    && snapshot.programCycle.state === 'idle_after_reset';
  const mutationUnavailableReason = state.workspace === null
    ? 'Program workspace is not loaded.'
    : snapshot === null
      ? 'Backend status is disconnected or has not arrived.'
      : snapshot.draftIdentity.draftId !== state.workspace.draft.draftId
        || snapshot.draftIdentity.draftRevision !== state.workspace.draft.draftRevision
        ? 'The displayed Draft is still synchronizing with the backend.'
        : snapshot.programCycle.state !== 'idle_after_reset'
          ? `Program execution is ${snapshot.programCycle.state.replaceAll('_', ' ')}; authoring requires Idle.`
          : !snapshot.authoringAdmission.authoringAllowed
            ? 'The backend has not admitted authoring; execution must be Idle and required components healthy.'
            : null;
  const admissionObserved = snapshot !== null;

  const readWorkspace = useCallback((requestGeneration: number) => {
    if (
      workspaceRead.current !== null
      && workspaceReadGeneration.current === requestGeneration
    ) {
      return workspaceRead.current;
    }
    const request = api.programWorkspace.get();
    workspaceRead.current = request;
    workspaceReadGeneration.current = requestGeneration;
    void request.then(
      () => {
        if (workspaceRead.current === request) {
          workspaceRead.current = null;
          workspaceReadGeneration.current = null;
        }
      },
      () => {
        if (workspaceRead.current === request) {
          workspaceRead.current = null;
          workspaceReadGeneration.current = null;
        }
      },
    );
    return request;
  }, []);

  const run = useCallback(async (
    action: ProgramWorkspaceActionName,
    request: () => Promise<ProgramWorkspaceRunResult>,
  ): Promise<ProgramWorkspaceRunResult | null> => {
    const current = generation.current + 1;
    generation.current = current;
    dispatch({ type: 'request_started', generation: current, action });
    try {
      const result = await request();
      if (mounted.current) {
        dispatch({
          type: 'request_succeeded',
          generation: current,
          workspace: result.workspace,
          message: result.message,
          transitionStatus: result.transitionStatus,
        });
      }
      return result;
    } catch (error: unknown) {
      if (mounted.current) {
        dispatch({
          type: 'request_failed',
          generation: current,
          error: errorMessage(error, 'Program workspace request failed.'),
        });
      }
      return null;
    }
  }, []);

  const refresh = useCallback(() => run(
    'refresh',
    () => readWorkspace(generation.current).then((workspace) =>
      workspaceResult(workspace, null)),
  ), [readWorkspace, run]);

  useEffect(() => {
    mounted.current = true;
    if (!initialReadStarted.current) {
      initialReadStarted.current = true;
      void refresh();
    }
    return () => {
      mounted.current = false;
    };
  }, [refresh]);

  useEffect(() => {
    if (snapshot === null || state.workspace === null) return;
    const displayed = state.workspace.draft;
    const observed = snapshot.draftIdentity;
    const backendHasNewerDraft = displayed.draftId !== observed.draftId
      || observed.draftRevision > displayed.draftRevision;
    if (!backendHasNewerDraft) {
      requestedSnapshotIdentity.current = null;
      return;
    }
    const identityKey = `${observed.draftId}:${observed.draftRevision}`;
    if (requestedSnapshotIdentity.current === identityKey) return;
    requestedSnapshotIdentity.current = identityKey;
    void refresh();
  }, [refresh, snapshot, state.workspace]);

  const requireWorkspace = useCallback((): ProgramWorkspaceResponse | null =>
    state.workspace, [state.workspace]);

  const mutate = useCallback(async (
    mutation: string,
    args: Record<string, JsonValue>,
  ): Promise<DraftMutationResponse | null> => {
    const workspace = requireWorkspace();
    const requestFence = fence(workspace, snapshot, requestOwnerEpoch.current);
    if (!requestFence) return null;
    let response: DraftMutationResponse | null = null;
    const result = await run(
      'mutate',
      async () => {
        response = await api.programWorkspace.mutate(requestFence, mutation, args);
        return workspaceResult(response.workspace, null);
      },
    );
    return result === null ? null : response;
  }, [requireWorkspace, run, snapshot]);

  const openDraft = useCallback((path: string, confirmDiscard: boolean) => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'open_draft',
      () => api.programWorkspace.openDraft(
        requestFence,
        path,
        confirmDiscard,
      ).then((next) => workspaceResult(
        next,
        'Program opened as Draft; runtime Applied state was not changed.',
      )),
    );
  }, [requireWorkspace, run, snapshot]);

  const newDraft = useCallback((confirmDiscard: boolean) => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'new',
      () => api.programWorkspace.newDraft(
        requestFence,
        confirmDiscard,
      ).then((next) => workspaceResult(
        next,
        'New unsaved Program Draft created; Saved, disk, and runtime were not changed.',
      )),
    );
  }, [requireWorkspace, run, snapshot]);

  const save = useCallback(() => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'save',
      () => api.programWorkspace.save(requestFence).then((next) =>
        workspaceResult(
          next,
          'Draft saved; runtime Applied state was not changed.',
        )),
    );
  }, [requireWorkspace, run, snapshot]);

  const saveAs = useCallback((path: string, confirmOverwrite: boolean) => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'save_as',
      () => api.programWorkspace.saveAs(
        requestFence,
        path,
        confirmOverwrite,
      ).then((next) => workspaceResult(
        next,
        'Draft saved to the selected path; runtime Applied state was not changed.',
      )),
    );
  }, [requireWorkspace, run, snapshot]);

  const apply = useCallback(() => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'apply',
      () => api.programWorkspace.apply(requestFence)
        .then((response) => programWorkspaceTransitionResult('apply', response)),
    );
  }, [requireWorkspace, run, snapshot]);

  const discard = useCallback(() => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'discard',
      () => api.programWorkspace.discard(requestFence).then((next) =>
        workspaceResult(
          next,
          'Draft discarded to the owner-issued Applied Program; Saved was unchanged.',
        )),
    );
  }, [requireWorkspace, run, snapshot]);

  const revert = useCallback(() => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'revert',
      () => api.programWorkspace.revert(requestFence)
        .then((response) => programWorkspaceTransitionResult('revert', response)),
    );
  }, [requireWorkspace, run, snapshot]);

  const loadScenario = useCallback((
    scenarioId: string,
    scenarioCatalogRevision: string,
    confirmDiscard: boolean,
  ) => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'load',
      () => api.programWorkspace.loadScenario(
        requestFence,
        scenarioId,
        scenarioCatalogRevision,
        confirmDiscard,
      ).then((response) => programWorkspaceTransitionResult('load', response)),
    );
  }, [requireWorkspace, run, snapshot]);

  const updateNodePolicies = useCallback((
    instanceId: string,
    modificationPolicy: ProgramNodeModificationPolicy,
    recoveryPolicy: ProgramNodeRecoveryPolicy,
    operatorStopPolicy: ProgramOperatorStopPolicy,
    executionGroups: NonNullable<ProgramTreeNode['execution_groups']>,
  ) => {
    mutate('program_draft_update_node_policies', {
      instance_id: instanceId,
      modification_policy: modificationPolicy as unknown as JsonValue,
      recovery_policy: recoveryPolicy as unknown as JsonValue,
      operator_stop_policy: operatorStopPolicy,
      execution_groups: executionGroups,
    });
  }, [mutate]);
  const clearFeedback = useCallback(() => {
    dispatch({ type: 'clear_feedback' });
  }, []);

  return useMemo(() => ({
    state,
    mutationAvailable,
    mutationUnavailableReason,
    admissionObserved,
    refresh,
    mutate,
    newDraft,
    openDraft,
    save,
    saveAs,
    apply,
    discard,
    revert,
    loadScenario,
    updateNodePolicies,
    clearFeedback,
  }), [
    admissionObserved,
    apply,
    clearFeedback,
    discard,
    loadScenario,
    mutate,
    mutationAvailable,
    mutationUnavailableReason,
    newDraft,
    openDraft,
    refresh,
    revert,
    save,
    saveAs,
    state,
    updateNodePolicies,
  ]);
}

export type ProgramWorkspaceController = ReturnType<typeof useProgramWorkspace>;
