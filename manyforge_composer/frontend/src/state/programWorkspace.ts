import type {
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionStatus,
} from '../types/api';

export type ProgramWorkspaceActionName =
  | 'refresh'
  | 'mutate'
  | 'open_draft'
  | 'apply'
  | 'discard'
  | 'revert'
  | 'load'
  | 'load_scenario_document'
  | 'new_scenario_document'
  | 'export_scenario_document'
  | 'save'
  | 'save_as'
  | 'save_scenario_document';

export type ProgramWorkspaceState =
  | {
      status: 'loading';
      requestGeneration: number;
      pendingAction: ProgramWorkspaceActionName;
      workspace: null;
      message: null;
      transitionStatus: null;
      error: null;
    }
  | {
      status: 'ready';
      requestGeneration: number;
      pendingAction: ProgramWorkspaceActionName | null;
      workspace: ProgramWorkspaceResponse;
      message: string | null;
      transitionStatus: ProgramWorkspaceTransitionStatus | null;
      error: string | null;
    }
  | {
      status: 'error';
      requestGeneration: number;
      pendingAction: null;
      workspace: ProgramWorkspaceResponse | null;
      message: null;
      transitionStatus: null;
      error: string;
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
      type: 'request_failed';
      generation: number;
      error: string;
    }
  | { type: 'request_refused'; generation: number; message: string }
  | { type: 'clear_feedback' }
  | {
      type: 'authoring_synced';
      workspace: ProgramWorkspaceResponse;
    };

export const INITIAL_PROGRAM_WORKSPACE_STATE: ProgramWorkspaceState = {
  status: 'loading',
  requestGeneration: 0,
  pendingAction: 'refresh',
  workspace: null,
  message: null,
  transitionStatus: null,
  error: null,
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
    } as ProgramWorkspaceState;
  }
  if (action.type === 'clear_feedback') {
    if (state.status === 'ready') {
      return {
        ...state,
        message: null,
        error: null,
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
      };
    }
    return state;
  }
  if (action.type === 'authoring_synced') {
    // Publishing the shared authoring half is not a user request, so it must
    // not be fenced by a request generation: it replaces only the authoring
    // truth and leaves the Applied projection, message and pending action
    // exactly as they are.
    if (state.workspace === null) return state;
    return { ...state, workspace: action.workspace };
  }
  if (action.generation !== state.requestGeneration) return state;
  if (action.type === 'request_refused') {
    if (state.workspace === null) return {
      ...state, status: 'error', pendingAction: null, message: null,
      transitionStatus: null, error: action.message,
    };
    return {
      status: 'ready', requestGeneration: action.generation,
      pendingAction: null, workspace: state.workspace,
      message: action.message, transitionStatus: null, error: null,
    };
  }
  if (action.type === 'request_succeeded') {
    return {
      status: 'ready',
      requestGeneration: action.generation,
      pendingAction: null,
      workspace: action.workspace,
      message: action.message,
      transitionStatus: action.transitionStatus,
      error: null,
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
  };
}
