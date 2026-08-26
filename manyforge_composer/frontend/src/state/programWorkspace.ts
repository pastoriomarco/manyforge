import type {
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionStatus,
} from '../types/api';

export type ProgramWorkspaceActionName =
  | 'refresh'
  | 'new'
  | 'mutate'
  | 'open_draft'
  | 'apply'
  | 'discard'
  | 'revert'
  | 'load'
  | 'save'
  | 'save_as';

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
  | { type: 'clear_feedback' };

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
