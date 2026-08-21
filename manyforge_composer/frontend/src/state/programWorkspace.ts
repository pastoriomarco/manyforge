import type {
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionStatus,
} from '../types/api';

export type ProgramWorkspaceActionName =
  | 'refresh'
  | 'new'
  | 'mutate'
  | 'update_policies'
  | 'open_draft'
  | 'apply'
  | 'discard'
  | 'revert'
  | 'load'
  | 'save'
  | 'save_as';

export type ProgramWorkspaceOwnerComparisonStatus =
  | 'observed'
  | 'refreshing'
  | 'stale';

export type ProgramWorkspaceState =
  | {
      status: 'loading';
      requestGeneration: number;
      pendingAction: ProgramWorkspaceActionName;
      workspace: null;
      message: null;
      transitionStatus: null;
      error: null;
      comparisonStatus?: ProgramWorkspaceOwnerComparisonStatus;
      refreshWarning?: string | null;
    }
  | {
      status: 'ready';
      requestGeneration: number;
      pendingAction: ProgramWorkspaceActionName | null;
      workspace: ProgramWorkspaceResponse;
      message: string | null;
      transitionStatus: ProgramWorkspaceTransitionStatus | null;
      error: string | null;
      comparisonStatus?: ProgramWorkspaceOwnerComparisonStatus;
      refreshWarning?: string | null;
    }
  | {
      status: 'error';
      requestGeneration: number;
      pendingAction: null;
      workspace: ProgramWorkspaceResponse | null;
      message: null;
      transitionStatus: null;
      error: string;
      comparisonStatus?: ProgramWorkspaceOwnerComparisonStatus;
      refreshWarning?: string | null;
    };

export type ProgramWorkspaceStateAction =
  | {
      type: 'request_started';
      generation: number;
      action: ProgramWorkspaceActionName;
    }
  | {
      type: 'request_succeeded';
      generation: number;
      workspace: ProgramWorkspaceResponse;
      message: string | null;
      transitionStatus: ProgramWorkspaceTransitionStatus | null;
    }
  | {
      type: 'policy_mutation_succeeded';
      generation: number;
      workspace: ProgramWorkspaceResponse;
      message: string;
    }
  | {
      type: 'workspace_refresh_failed';
      generation: number;
      warning: string;
    }
  | {
      type: 'request_failed';
      generation: number;
      error: string;
    }
  | { type: 'clear_feedback' };

export const INITIAL_PROGRAM_WORKSPACE_STATE: ProgramWorkspaceState = {
  status: 'loading',
  requestGeneration: 0,
  pendingAction: 'refresh',
  workspace: null,
  message: null,
  transitionStatus: null,
  error: null,
  comparisonStatus: 'observed',
  refreshWarning: null,
};

export function programWorkspaceReducer(
  state: ProgramWorkspaceState,
  action: ProgramWorkspaceStateAction,
): ProgramWorkspaceState {
  if (action.type === 'request_started') {
    return {
      status: state.workspace === null ? 'loading' : 'ready',
      requestGeneration: action.generation,
      pendingAction: action.action,
      workspace: state.workspace,
      message: null,
      transitionStatus: null,
      error: null,
      comparisonStatus: action.action === 'refresh'
        ? 'refreshing'
        : state.comparisonStatus,
      refreshWarning: null,
    } as ProgramWorkspaceState;
  }
  if (action.type === 'clear_feedback') {
    if (state.status === 'ready') {
      return {
        ...state,
        message: null,
        error: null,
        refreshWarning: null,
      };
    }
    if (state.status === 'error' && state.workspace !== null) {
      return {
        status: 'ready',
        requestGeneration: state.requestGeneration,
        pendingAction: null,
        workspace: state.workspace,
        message: null,
        transitionStatus: null,
        error: null,
        comparisonStatus: state.comparisonStatus,
        refreshWarning: null,
      };
    }
    return state;
  }
  if (action.generation !== state.requestGeneration) return state;
  if (action.type === 'request_succeeded') {
    return {
      status: 'ready',
      requestGeneration: action.generation,
      pendingAction: null,
      workspace: action.workspace,
      message: action.message,
      transitionStatus: action.transitionStatus,
      error: null,
      comparisonStatus: 'observed',
      refreshWarning: null,
    };
  }
  if (action.type === 'policy_mutation_succeeded') {
    return {
      status: 'ready',
      requestGeneration: action.generation,
      pendingAction: 'refresh',
      workspace: action.workspace,
      message: action.message,
      transitionStatus: null,
      error: null,
      comparisonStatus: 'refreshing',
      refreshWarning: null,
    };
  }
  if (action.type === 'workspace_refresh_failed') {
    if (state.workspace === null) return state;
    return {
      status: 'ready',
      requestGeneration: action.generation,
      pendingAction: null,
      workspace: state.workspace,
      message: state.message,
      transitionStatus: null,
      error: null,
      comparisonStatus: 'stale',
      refreshWarning: action.warning,
    };
  }
  return {
    status: 'error',
    requestGeneration: action.generation,
    pendingAction: null,
    workspace: state.workspace,
    message: null,
    transitionStatus: null,
    error: action.error,
    comparisonStatus: state.comparisonStatus === 'refreshing'
      ? 'stale'
      : state.comparisonStatus,
    refreshWarning: null,
  };
}

/**
 * Replace only the closed Draft projection returned by a Draft-only mutation.
 * Saved and Applied remain the last owner-issued observations until the
 * follow-up workspace read succeeds.
 */
export function replaceProgramWorkspaceDraft(
  workspace: ProgramWorkspaceResponse,
  draft: ProgramWorkspaceResponse['draft'],
): ProgramWorkspaceResponse {
  const savedMatchesDraft = (
    draft.programLoaded
    && draft.programRevision !== null
    && workspace.saved.available
    && workspace.saved.programRevision === draft.programRevision
  );
  const appliedMatchesDraft = workspace.applied.available
    ? (
      draft.programLoaded
      && draft.programRevision !== null
      && workspace.applied.programRevision === draft.programRevision
    )
    : null;
  return {
    ...workspace,
    draft,
    saved: {
      ...workspace.saved,
      matchesDraft: savedMatchesDraft,
    },
    applied: {
      ...workspace.applied,
      matchesDraft: appliedMatchesDraft,
    },
    hasUnsavedChanges: draft.programLoaded && !savedMatchesDraft,
    hasUnappliedChanges: workspace.applied.available
      ? draft.programLoaded && !appliedMatchesDraft
      : null,
  };
}
