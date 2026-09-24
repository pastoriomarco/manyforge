import {
  useCallback,
  useEffect,
  useMemo,
  useReducer,
  useRef,
} from 'react';

import { api, isStaleWorkspaceError } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import {
  programLocalFence,
  publicationSupersedesProgram,
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

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
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
  if (response.status === 'accepted'
      && (action === 'apply' || action === 'revert')) {
    const applyId = response.headless?.apply_id;
    return {
      workspace: response.workspace,
      message: `${action === 'apply' ? 'Apply' : 'Revert'} accepted (${String(applyId)}). Check Runtime / history for completion.`,
      transitionStatus: response.status,
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
  const generation = useRef(0);
  const mounted = useRef(true);
  const workspaceRead = useRef<Promise<ProgramWorkspaceResponse> | null>(null);
  const workspaceReadGeneration = useRef<number | null>(null);
  const projectionIdentityRead = useRef<string | null>(null);
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
  }, [authoring]);

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
  }, [authoring.current, published, readWorkspace, run]);

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
        message: `Scenario Load accepted (${applyId}); activation is in progress. Check Runtime / history for completion; motion does not start automatically.`,
        transitionStatus: 'accepted',
      };
    });
  }, [run, scenarioFence]);

  const convertLegacyScenarioDocument = useCallback((input: {
    sourcePath: string;
    destinationPath: string;
    confirmOverwrite: boolean;
  }) => {
    const current = requireWorkspace();
    if (current === null) return;
    void run('convert_legacy_scenario_document', async () => {
      await api.scenarioDocuments.convertLegacy(
        input.sourcePath,
        input.destinationPath,
        input.confirmOverwrite,
      );
      return workspaceResult(
        current,
        'Legacy Scenario converted to a project; the source file and runtime state were unchanged.',
      );
    });
  }, [requireWorkspace, run]);

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
    saveScenarioDocument,
    loadScenarioDocument,
    convertLegacyScenarioDocument,
    exportScenarioDocument,
    // The selected project path and its source diagnostics are shared project
    // facts of the one publication, not per-hook state.
    scenarioDocumentPath: project === null ? null : project.documentPath,
    scenarioSourceDiagnostics: project === null ? [] : project.sourceDiagnostics,
    updateNodePolicies,
    clearFeedback,
  }), [
    admissionObserved,
    apply,
    clearFeedback,
    discard,
    loadScenarioDocument,
    convertLegacyScenarioDocument,
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
