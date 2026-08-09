import { describe, expect, it } from 'vitest';

import type { ProgramWorkspaceResponse } from '../types/api';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
} from './programWorkspace';

function workspace(draftId: string, revision: number): ProgramWorkspaceResponse {
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v1',
    draft: {
      draftId,
      draftRevision: revision,
      programLoaded: false,
      programRevision: null,
      program: null,
    },
    saved: {
      available: false,
      matchesDraft: false,
      path: null,
      programRevision: null,
    },
    applied: {
      available: false,
      matchesDraft: null,
      programRevision: null,
      unavailableReason: {
        code: 'program_apply_unavailable',
        message: 'Apply waits for N7.',
        restorationGates: ['N7'],
      },
    },
    hasUnsavedChanges: false,
    hasUnappliedChanges: null,
  };
}

describe('Program workspace request fencing', () => {
  it('ignores an older response after a newer request starts', () => {
    const first = programWorkspaceReducer(INITIAL_PROGRAM_WORKSPACE_STATE, {
      type: 'request_started',
      generation: 1,
      action: 'refresh',
    });
    const second = programWorkspaceReducer(first, {
      type: 'request_started',
      generation: 2,
      action: 'open_draft',
    });
    const stale = programWorkspaceReducer(second, {
      type: 'request_succeeded',
      generation: 1,
      workspace: workspace('old', 1),
      message: null,
    });

    expect(stale).toBe(second);
  });

  it('accepts only the current rotated Draft identity', () => {
    const started = programWorkspaceReducer(INITIAL_PROGRAM_WORKSPACE_STATE, {
      type: 'request_started',
      generation: 3,
      action: 'open_draft',
    });
    const succeeded = programWorkspaceReducer(started, {
      type: 'request_succeeded',
      generation: 3,
      workspace: workspace('replacement', 1),
      message: 'Opened as Draft.',
    });

    expect(succeeded.status).toBe('ready');
    expect(succeeded.workspace?.draft.draftId).toBe('replacement');
    expect(succeeded.pendingAction).toBeNull();
  });

  it('keeps the last good workspace recoverable after a request failure', () => {
    const ready = programWorkspaceReducer({
      ...INITIAL_PROGRAM_WORKSPACE_STATE,
      status: 'ready',
      pendingAction: null,
      workspace: workspace('draft-1', 2),
    }, {
      type: 'request_started',
      generation: 4,
      action: 'save',
    });
    const failed = programWorkspaceReducer(ready, {
      type: 'request_failed',
      generation: 4,
      error: 'save failed',
    });
    const dismissed = programWorkspaceReducer(failed, { type: 'clear_feedback' });

    expect(dismissed.status).toBe('ready');
    expect(dismissed.workspace?.draft.draftId).toBe('draft-1');
    expect(dismissed.error).toBeNull();
  });
});
