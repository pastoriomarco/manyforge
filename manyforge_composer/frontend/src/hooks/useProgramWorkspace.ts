import { useCallback, useEffect, useReducer, useRef } from 'react';

import { api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
  replaceProgramWorkspaceDraft,
  type ProgramWorkspaceActionName,
} from '../state/programWorkspace';
import type {
  JsonValue,
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramWorkspaceResponse,
  ProgramNodePolicyMutationResponse,
  ProgramWorkspaceTransitionResponse,
  ProgramWorkspaceTransitionStatus,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { browserStableIdentity } from './useHeadlessState';

const POLICY_REFRESH_FAILURE =
  'Draft updated; workspace status refresh failed. Your Draft change remains; retry refresh.';

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
    clientId: snapshot.clientId,
    snapshotReceiverEpoch: snapshot.receiverEpoch,
    snapshotSequence: snapshot.snapshotSequence,
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
      message: 'Load completed; Draft, Saved, and Applied now match the loaded Program.',
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
  ) => {
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
    } catch (error: unknown) {
      if (mounted.current) {
        dispatch({
          type: 'request_failed',
          generation: current,
          error: errorMessage(error, 'Program workspace request failed.'),
        });
      }
    }
  }, []);

  const refresh = useCallback(() => run(
    'refresh',
    () => readWorkspace(generation.current).then((workspace) =>
      workspaceResult(workspace, null)),
  ), [readWorkspace, run]);

  useEffect(() => {
    mounted.current = true;
    void refresh();
    return () => {
      mounted.current = false;
    };
  }, [refresh]);

  const requireWorkspace = useCallback((): ProgramWorkspaceResponse | null =>
    state.workspace, [state.workspace]);

  const mutate = useCallback((
    mutation: string,
    args: Record<string, JsonValue>,
  ) => {
    const workspace = requireWorkspace();
    const requestFence = fence(workspace, snapshot, requestOwnerEpoch.current);
    if (!requestFence) return;
    void run(
      'mutate',
      () => api.programWorkspace.mutate(requestFence, mutation, args)
        .then((response) => workspaceResult(response.workspace, 'Draft updated.')),
    );
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

  const apply = useCallback((
    scenarioId: string,
    scenarioCatalogRevision: string,
  ) => {
    const workspace = requireWorkspace();
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!requestFence) return;
    void run(
      'apply',
      () => api.programWorkspace.apply(
        requestFence,
        scenarioId,
        scenarioCatalogRevision,
      ).then((response) => programWorkspaceTransitionResult('apply', response)),
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

  const load = useCallback((
    path: string,
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
      () => api.programWorkspace.load(
        requestFence,
        path,
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
  ) => {
    const workspace = requireWorkspace();
    const expectedProgramRevision = workspace?.draft.programRevision;
    const requestFence = workspace
      ? fence(workspace, snapshot, requestOwnerEpoch.current) : null;
    if (!workspace || !workspace.draft.programLoaded || !expectedProgramRevision || !requestFence) return;

    const current = generation.current + 1;
    generation.current = current;
    dispatch({ type: 'request_started', generation: current, action: 'update_policies' });
    void (async () => {
      let response: ProgramNodePolicyMutationResponse;
      try {
        response = await api.programWorkspace.updateNodePolicies(
          requestFence,
          expectedProgramRevision,
          instanceId,
          modificationPolicy,
          recoveryPolicy,
          operatorStopPolicy,
        );
      } catch (error: unknown) {
        if (mounted.current && generation.current === current) {
          dispatch({
            type: 'request_failed',
            generation: current,
            error: `${errorMessage(error, 'Draft policy update failed.')} Refresh workspace before retrying.`,
          });
        }
        return;
      }
      if (!mounted.current || generation.current !== current) return;

      const revisionMessage = response.draft.draftRevision === workspace.draft.draftRevision
        ? `Draft policy unchanged; Draft revision ${response.draft.draftRevision} (no-op).`
        : `Draft updated; Draft revision ${response.draft.draftRevision}.`;
      dispatch({
        type: 'policy_mutation_succeeded',
        generation: current,
        workspace: replaceProgramWorkspaceDraft(workspace, response.draft),
        message: `${revisionMessage} Workspace owner status is refreshing.`,
      });

      try {
        const refreshed = await readWorkspace(current);
        if (mounted.current && generation.current === current) {
          dispatch({
            type: 'request_succeeded',
            generation: current,
            workspace: refreshed,
            message: revisionMessage,
            transitionStatus: null,
          });
        }
      } catch {
        if (mounted.current && generation.current === current) {
          dispatch({
            type: 'workspace_refresh_failed',
            generation: current,
            warning: POLICY_REFRESH_FAILURE,
          });
        }
      }
    })();
  }, [readWorkspace, requireWorkspace, snapshot]);

  return {
    state,
    mutationAvailable,
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
    load,
    updateNodePolicies,
    clearFeedback: () => dispatch({ type: 'clear_feedback' }),
  };
}

export type ProgramWorkspaceController = ReturnType<typeof useProgramWorkspace>;
