import { useCallback, useEffect, useReducer, useRef } from 'react';

import { api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
  type ProgramWorkspaceActionName,
} from '../state/programWorkspace';
import type { JsonValue, ProgramWorkspaceResponse } from '../types/api';

function fence(workspace: ProgramWorkspaceResponse) {
  return {
    draftId: workspace.draft.draftId,
    expectedDraftRevision: workspace.draft.draftRevision,
  };
}
export function useProgramWorkspace() {
  const [state, dispatch] = useReducer(
    programWorkspaceReducer,
    INITIAL_PROGRAM_WORKSPACE_STATE,
  );
  const generation = useRef(0);
  const mounted = useRef(true);

  const run = useCallback(async (
    action: ProgramWorkspaceActionName,
    request: () => Promise<ProgramWorkspaceResponse>,
    successMessage: string | null,
  ) => {
    const current = generation.current + 1;
    generation.current = current;
    dispatch({ type: 'request_started', generation: current, action });
    try {
      const workspace = await request();
      if (mounted.current) {
        dispatch({
          type: 'request_succeeded',
          generation: current,
          workspace,
          message: successMessage,
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
    () => api.programWorkspace.get(),
    null,
  ), [run]);

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
    if (!workspace) return;
    void run(
      'mutate',
      () => api.programWorkspace.mutate(fence(workspace), mutation, args)
        .then((response) => response.workspace),
      'Draft updated.',
    );
  }, [requireWorkspace, run]);

  const openDraft = useCallback((path: string, confirmDiscard: boolean) => {
    const workspace = requireWorkspace();
    if (!workspace) return;
    void run(
      'open_draft',
      () => api.programWorkspace.openDraft(
        fence(workspace),
        path,
        confirmDiscard,
      ),
      'Program opened as Draft; runtime Applied state was not changed.',
    );
  }, [requireWorkspace, run]);

  const save = useCallback(() => {
    const workspace = requireWorkspace();
    if (!workspace) return;
    void run(
      'save',
      () => api.programWorkspace.save(fence(workspace)),
      'Draft saved; runtime Applied state was not changed.',
    );
  }, [requireWorkspace, run]);

  const saveAs = useCallback((path: string, confirmOverwrite: boolean) => {
    const workspace = requireWorkspace();
    if (!workspace) return;
    void run(
      'save_as',
      () => api.programWorkspace.saveAs(
        fence(workspace),
        path,
        confirmOverwrite,
      ),
      'Draft saved to the selected path; runtime Applied state was not changed.',
    );
  }, [requireWorkspace, run]);

  const apply = useCallback((
    scenarioId: string,
    scenarioCatalogRevision: string,
  ) => {
    const workspace = requireWorkspace();
    if (!workspace) return;
    void run(
      'apply',
      () => api.programWorkspace.apply(
        fence(workspace),
        scenarioId,
        scenarioCatalogRevision,
      ).then((response) => response.workspace),
      'Apply request accepted; the owner-issued Applied state is shown above.',
    );
  }, [requireWorkspace, run]);

  const load = useCallback((
    path: string,
    scenarioId: string,
    scenarioCatalogRevision: string,
    confirmDiscard: boolean,
  ) => {
    const workspace = requireWorkspace();
    if (!workspace) return;
    void run(
      'load',
      () => api.programWorkspace.load(
        fence(workspace),
        path,
        scenarioId,
        scenarioCatalogRevision,
        confirmDiscard,
      ).then((response) => response.workspace),
      'Load request accepted; Draft, Saved, and Applied state are shown above.',
    );
  }, [requireWorkspace, run]);

  return {
    state,
    refresh,
    mutate,
    openDraft,
    save,
    saveAs,
    apply,
    load,
    clearFeedback: () => dispatch({ type: 'clear_feedback' }),
  };
}

export type ProgramWorkspaceController = ReturnType<typeof useProgramWorkspace>;
