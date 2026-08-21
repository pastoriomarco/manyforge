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
      transitionStatus: null,
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
      transitionStatus: null,
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

  it('surfaces pending lifecycle truth and preserves the last owner workspace on failure', () => {
    const current = workspace('draft-1', 7);
    const started = programWorkspaceReducer({
      ...INITIAL_PROGRAM_WORKSPACE_STATE,
      status: 'ready',
      pendingAction: null,
      workspace: current,
    }, {
      type: 'request_started',
      generation: 8,
      action: 'revert',
    });
    expect(started.pendingAction).toBe('revert');
    const failed = programWorkspaceReducer(started, {
      type: 'request_failed',
      generation: 8,
      error: 'stale Draft fence',
    });
    expect(failed.workspace).toBe(current);
    expect(failed.error).toBe('stale Draft fence');
  });

  it('tracks New as an independent latest-generation lifecycle action', () => {
    const current = workspace('draft-before-new', 12);
    const started = programWorkspaceReducer({
      ...INITIAL_PROGRAM_WORKSPACE_STATE,
      status: 'ready',
      pendingAction: null,
      workspace: current,
    }, {
      type: 'request_started',
      generation: 10,
      action: 'new',
    });
    const created = workspace('fresh-new-draft', 1);
    const succeeded = programWorkspaceReducer(started, {
      type: 'request_succeeded',
      generation: 10,
      workspace: created,
      message: 'New unsaved Program Draft created.',
      transitionStatus: null,
    });

    expect(started.pendingAction).toBe('new');
    expect(succeeded.workspace).toBe(created);
    expect(succeeded.pendingAction).toBeNull();
  });

  it('clears Revert pending action with the owner-returned pending workspace', () => {
    const previous = workspace('draft-before-revert', 7);
    const ownerPending = workspace('owner-pending-revert', 8);
    const started = programWorkspaceReducer({
      ...INITIAL_PROGRAM_WORKSPACE_STATE,
      status: 'ready',
      pendingAction: null,
      workspace: previous,
    }, {
      type: 'request_started',
      generation: 9,
      action: 'revert',
    });
    const pending = programWorkspaceReducer(started, {
      type: 'request_succeeded',
      generation: 9,
      workspace: ownerPending,
      message: 'Revert is pending deployment; Draft and Saved were not replaced.',
      transitionStatus: 'pending_deployment',
    });

    expect(pending.status).toBe('ready');
    expect(pending.pendingAction).toBeNull();
    expect(pending.workspace).toBe(ownerPending);
    expect(pending.transitionStatus).toBe('pending_deployment');
    expect(pending.message).toContain('were not replaced');
    expect(pending.message).not.toContain('Draft now matches Saved');
  });
});
