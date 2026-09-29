import {
  useCallback,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';

import { api, authoringRefusalMessage, isStaleWorkspaceError } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type { ManagerApplyIdentity } from '../state/managerApply';
import {
  programLocalFence,
  publicationSupersedesProgram,
  programActionUnavailableReason,
  scenarioLocalFence,
  withSharedProgramAuthoring,
} from '../state/authoringWorkspace';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
  type ProgramWorkspaceActionName,
} from '../state/programWorkspace';
import type {
  DraftMutationResponse,
  JsonValue,
  LocalWorkspaceFenceRequest,
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramTreeNode,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  ProgramWorkspaceTransitionStatus,
  ScenarioDocumentFenceRequest,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { browserStableIdentity } from './useHeadlessState';
import { programRuntimeWakeKey } from './useProgramRuntime';

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

interface ProgramWorkspaceRunResult {
  workspace: ProgramWorkspaceResponse;
  message: string | null;
  transitionStatus: ProgramWorkspaceTransitionStatus | null;
  /** Exact Manager operation accepted by this request, when one was. */
  acceptedApply?: ManagerApplyIdentity | null;
}

function acceptedApplyIdentity(
  applyId: unknown,
  applyGeneration: unknown,
): ManagerApplyIdentity | null {
  return typeof applyId === 'string' && applyId.length > 0
    && typeof applyGeneration === 'number' && Number.isSafeInteger(applyGeneration)
    ? { applyId, applyGeneration }
    : null;
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
  if (response.status === 'accepted'
      && (action === 'apply' || action === 'revert')) {
    const applyId = response.headless?.apply_id;
    return {
      workspace: response.workspace,
      message: `${action === 'apply' ? 'Apply' : 'Revert'} accepted (${String(applyId)}); activation progress is shown under Activity.`,
      transitionStatus: response.status,
      acceptedApply: acceptedApplyIdentity(applyId, response.headless?.apply_generation),
    };
  }
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

export function useProgramWorkspace(
  snapshot: ComposerHeadlessState | null,
  authoring: AuthoringWorkspaceController,
) {
  const [state, dispatch] = useReducer(
    programWorkspaceReducer,
    INITIAL_PROGRAM_WORKSPACE_STATE,
  );
  const [acceptedApply, setAcceptedApply] = useState<ManagerApplyIdentity | null>(null);
  const generation = useRef(0);
  const mounted = useRef(true);
  const workspaceRead = useRef<Promise<ProgramWorkspaceResponse> | null>(null);
  const workspaceReadGeneration = useRef<number | null>(null);
  const projectionIdentityRead = useRef<string | null>(null);
  const appliedIdentityRead = useRef<string | null>(null);
  const requestOwnerEpoch = useRef(browserStableIdentity(
    'manyforge.composer.request_owner_epoch',
    'composer-authoring-owner',
  ));
  // Authoring identity, both Draft projections' fences and the admission gate
  // come from the one shared publication. This hook contributes the
  // Applied/applicability projection it reads from the Program workspace route,
  // which owns runtime truth.
  const mutationUnavailableReason = state.workspace === null
    ? 'Program workspace is not loaded.'
    : authoring.unavailableReason;
  const mutationAvailable = mutationUnavailableReason === null;
  const admissionObserved = snapshot !== null;
  const published = authoring.published;
  const project = authoring.project;

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
    if ((action === 'apply' || action === 'revert' || action === 'discard')
      && programActionUnavailableReason(state.workspace, action, mutationUnavailableReason) !== null) return null;
    if (action !== 'refresh' && !authoring.beginMutation()) return null;
    const current = generation.current + 1;
    generation.current = current;
    dispatch({ type: 'request_started', generation: current, action });
    try {
      const result = await request();
      if (action !== 'refresh') authoring.finishMutation(result.acceptedApply ?? null);
      if (mounted.current && result.acceptedApply) {
        setAcceptedApply(result.acceptedApply);
      }
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
      if (action !== 'refresh') authoring.finishMutation(null, error);
      const refusal = action === 'refresh' ? null : authoringRefusalMessage(error);
      if (refusal !== null) {
        if (mounted.current) dispatch({ type: 'request_refused', generation: current, message: refusal });
        return null;
      }
      if (mounted.current) {
        dispatch({
          type: 'request_failed',
          generation: current,
          error: errorMessage(error, 'Program workspace request failed.'),
        });
      }
      // Another client's accepted edit moved the workspace ahead of this
      // request. Refresh both owners' truth once and never repeat the edit.
      if (isStaleWorkspaceError(error)) {
        void readWorkspace(current).then(
          (workspace) => {
            if (mounted.current) {
              dispatch({
                type: 'request_succeeded',
                generation: current,
                workspace,
                message: null,
                transitionStatus: null,
              });
            }
          },
          () => undefined,
        );
        void authoring.refresh();
      }
      return null;
    }
  }, [authoring, state.workspace, mutationUnavailableReason, readWorkspace]);

  const refresh = useCallback(() => {
    const result = run(
      'refresh',
      () => readWorkspace(generation.current).then((workspace) =>
        workspaceResult(workspace, null)),
    );
    // The shared publication is the authoring half of this projection, so an
    // operator refresh reads both owners' truth instead of only the Applied
    // projection.
    void authoring.refresh();
    return result;
  }, [authoring, readWorkspace, run]);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  useEffect(() => {
    if (!authoring.current || published === null) return;
    const identityKey = `${published.identity.workspaceEpoch}:${published.identity.workspaceRevision}`;
    if (projectionIdentityRead.current === identityKey) return;
    projectionIdentityRead.current = identityKey;
    const requestGeneration = generation.current + 1;
    void run(
      'refresh',
      () => readWorkspace(requestGeneration).then((workspace) =>
        workspaceResult(workspace, null)),
    );
  }, [authoring, published, readWorkspace, run]);

  // The Applied half of this projection changes without a new authoring
  // publication when an accepted Load/Apply/Revert settles. Re-read once per
  // Applied runtime identity, as the runtime projection does, and never over
  // an in-flight operation, whose own result is still to be shown.
  const appliedIdentity = programRuntimeWakeKey(snapshot);
  const operationPending = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  useEffect(() => {
    if (snapshot === null || state.workspace === null || operationPending) return;
    if (appliedIdentityRead.current === appliedIdentity) return;
    const initial = appliedIdentityRead.current === null;
    appliedIdentityRead.current = appliedIdentity;
    if (initial) return;
    const requestGeneration = generation.current + 1;
    void run(
      'refresh',
      () => readWorkspace(requestGeneration).then((workspace) =>
        workspaceResult(workspace, null)),
    );
  }, [appliedIdentity, operationPending, readWorkspace, run, snapshot, state.workspace]);

  useEffect(() => {
    if (state.workspace === null || published === null) return;
    // Never let a lagging publication revert a just-committed post-image.
    if (!publicationSupersedesProgram(published, state.workspace)) return;
    // Publish the shared authoring half into the projection this hook displays.
    // The publication is the only authoring writer, so this can never resurrect
    // an older Draft over a newer one.
    const merged = withSharedProgramAuthoring(
      state.workspace,
      published.snapshot,
    );
    if (
      merged.draft === state.workspace.draft
      && merged.saved === state.workspace.saved
      && merged.hasUnsavedChanges === state.workspace.hasUnsavedChanges
    ) {
      return;
    }
    dispatch({ type: 'authoring_synced', workspace: merged });
  }, [published, state.workspace]);

  const requireWorkspace = useCallback((): ProgramWorkspaceResponse | null =>
    state.workspace, [state.workspace]);

  /** The one local Program fence, or ``null`` while authoring is gated. */
  const localFence = useCallback((): LocalWorkspaceFenceRequest | null => {
    if (
      mutationUnavailableReason !== null
      || published === null
      || snapshot === null
    ) return null;
    return programLocalFence(
      published,
      snapshot,
      requestOwnerEpoch.current,
      identity('composer-authoring'),
    );
  }, [mutationUnavailableReason, published, snapshot]);

  /**
   * The one Scenario document fence.
   *
   * Both Draft halves come from the same installed publication the backend
   * rechecks, so this needs no second workspace read.
   */
  const scenarioFence = useCallback((): ScenarioDocumentFenceRequest | null => {
    if (
      mutationUnavailableReason !== null
      || published === null
      || snapshot === null
    ) return null;
    return scenarioLocalFence(
      published,
      snapshot,
      requestOwnerEpoch.current,
      identity('composer-document'),
    );
  }, [mutationUnavailableReason, published, snapshot]);

  const mutate = useCallback(async (
    mutation: string,
    args: Record<string, JsonValue>,
  ): Promise<DraftMutationResponse | null> => {
    const requestFence = localFence();
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
  }, [localFence, run]);

  const openDraft = useCallback((path: string, confirmDiscard: boolean) => {
    const requestFence = localFence();
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
  }, [localFence, run]);

  const newDraft = useCallback((confirmDiscard: boolean) => {
    const requestFence = localFence();
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
  }, [localFence, run]);

  const save = useCallback(() => {
    const requestFence = localFence();
    if (!requestFence) return;
    void run(
      'save',
      () => api.programWorkspace.save(requestFence).then((next) =>
        workspaceResult(
          next,
          'Draft saved; runtime Applied state was not changed.',
        )),
    );
  }, [localFence, run]);

  const saveAs = useCallback((path: string, confirmOverwrite: boolean) => {
    const requestFence = localFence();
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
  }, [localFence, run]);

  const apply = useCallback(() => {
    const requestFence = localFence();
    if (!requestFence) return;
    void run(
      'apply',
      () => api.programWorkspace.apply(requestFence)
        .then((response) => programWorkspaceTransitionResult('apply', response)),
    );
  }, [localFence, run]);

  const discard = useCallback(() => {
    const requestFence = localFence();
    if (!requestFence) return;
    void run(
      'discard',
      () => api.programWorkspace.discard(requestFence).then((next) =>
        workspaceResult(
          next,
          'Draft discarded to the owner-issued Applied Program; Saved was unchanged.',
        )),
    );
  }, [localFence, run]);

  const revert = useCallback(() => {
    const requestFence = localFence();
    if (!requestFence) return;
    void run(
      'revert',
      () => api.programWorkspace.revert(requestFence)
        .then((response) => programWorkspaceTransitionResult('revert', response)),
    );
  }, [localFence, run]);

  const saveScenarioDocument = useCallback((input: {
    path: string;
    scenarioId: string;
    name: string;
    description: string;
    confirmOverwrite: boolean;
  }) => {
    void run('save_scenario_document', async () => {
      const requestFence = scenarioFence();
      if (requestFence === null) throw new Error('Scenario Save is not admitted.');
      const response = await api.scenarioDocuments.save(requestFence, input);
      return workspaceResult(
        response.workspace,
        `Scenario saved (${response.lastKnownStatus}); runtime state was not changed.`,
      );
    });
  }, [run, scenarioFence]);

  const exportScenarioDocument = useCallback((destinationPath: string) => {
    const requestFence = scenarioFence();
    const current = requireWorkspace();
    if (requestFence === null || current === null) return;
    void run('export_scenario_document', async () => {
      const response = await api.scenarioDocuments.export(
        requestFence, destinationPath,
      );
      return workspaceResult(
        current,
        `Portable Scenario exported to ${response.path}.`,
      );
    });
  }, [requireWorkspace, run, scenarioFence]);

  const loadScenarioDocument = useCallback((
    path: string,
    confirmDiscard: boolean,
  ) => {
    void run('load_scenario_document', async () => {
      const requestFence = scenarioFence();
      if (requestFence === null) throw new Error('Scenario Load is not admitted.');
      const response = await api.scenarioDocuments.load(
        requestFence,
        path,
        confirmDiscard,
      );
      if (response.status === 'applied') {
        return {
          workspace: response.workspace,
          message: 'Scenario loaded; its Deployment and Program already match the Applied configuration. No activation was needed.',
          transitionStatus: 'applied',
        };
      }
      const applyId = response.activation.apply_id;
      return {
        workspace: response.workspace,
        message: `Scenario Load accepted (${applyId}); activation progress is shown under Activity. Motion does not start automatically.`,
        transitionStatus: 'accepted',
        acceptedApply: acceptedApplyIdentity(
          applyId,
          response.activation.apply_generation,
        ),
      };
    });
  }, [run, scenarioFence]);

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
    acceptedApply,
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
    saveScenarioDocument,
    loadScenarioDocument,
    exportScenarioDocument,
    // The selected project path and its source diagnostics are shared project
    // facts of the one publication, not per-hook state.
    scenarioDocumentPath: project === null ? null : project.documentPath,
    scenarioDocument: {
      id: project?.documentId ?? null,
      name: project?.documentName ?? null,
      description: project?.documentDescription ?? null,
    },
    scenarioSourceDiagnostics: project === null ? [] : project.sourceDiagnostics,
    updateNodePolicies,
    clearFeedback,
  }), [
    acceptedApply,
    admissionObserved,
    apply,
    clearFeedback,
    discard,
    loadScenarioDocument,
    exportScenarioDocument,
    mutate,
    mutationAvailable,
    mutationUnavailableReason,
    newDraft,
    openDraft,
    project,
    refresh,
    revert,
    save,
    saveAs,
    saveScenarioDocument,
    state,
    updateNodePolicies,
  ]);
}

export type ProgramWorkspaceController = ReturnType<typeof useProgramWorkspace>;
