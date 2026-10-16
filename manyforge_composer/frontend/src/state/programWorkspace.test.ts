import { describe, expect, it } from 'vitest';

import type { ProgramWorkspaceResponse } from '../types/api';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
} from './programWorkspace';

it('keeps the draft and reports a refused edit without an error or success', () => {
  const current = workspace('draft-1', 7);
  const ready = programWorkspaceReducer(INITIAL_PROGRAM_WORKSPACE_STATE, {
    type: 'request_succeeded', generation: 0, workspace: current,
    message: null, transitionStatus: null,
  });
  const started = programWorkspaceReducer(ready, { type: 'request_started', generation: 1, action: 'mutate' });
  const refused = programWorkspaceReducer(started, {
    type: 'request_refused', generation: 1, message: 'Edit not applied',
  });
  expect(refused.workspace).toBe(current);
  expect(refused.error).toBeNull();
  expect(refused.message).toBe('Edit not applied');
  expect(refused.pendingAction).toBeNull();
  expect(refused.transitionStatus).toBeNull();
});

function workspace(draftId: string, revision: number): ProgramWorkspaceResponse {
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
      programDraftId: draftId, programDraftRevision: revision,
      deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
    },
    draft: {
      draftId,
      draftRevision: revision,
      programLoaded: false,
      authoring: null,
      candidateAvailable: false,
      programRevision: null,
      program: null,
      diagnostics: [],
      formulaReferences: { computedKeys: {}, nodes: {} },
    },
    applicability: {
      applyReady: true,
      unavailableReason: null,
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
      action: 'new_scenario_document',
    });
    const created = workspace('fresh-new-draft', 1);
    const succeeded = programWorkspaceReducer(started, {
      type: 'request_succeeded',
      generation: 10,
      workspace: created,
      message: 'New Scenario saved; activation accepted.',
      transitionStatus: null,
    });

    expect(started.pendingAction).toBe('new_scenario_document');
    expect(succeeded.workspace).toBe(created);
    expect(succeeded.pendingAction).toBeNull();
  });

  it('clears Revert action with the owner-returned completed workspace', () => {
    const previous = workspace('draft-before-revert', 7);
    const ownerReverted = workspace('owner-reverted', 8);
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
    const reverted = programWorkspaceReducer(started, {
      type: 'request_succeeded',
      generation: 9,
      workspace: ownerReverted,
      message: 'Revert completed through authoritative Apply; Draft now matches Saved.',
      transitionStatus: 'reverted',
    });

    expect(reverted.status).toBe('ready');
    expect(reverted.pendingAction).toBeNull();
    expect(reverted.workspace).toBe(ownerReverted);
    expect(reverted.transitionStatus).toBe('reverted');
    expect(reverted.message).toContain('Draft now matches Saved');
  });

});


it('retains a partial Close cancellation message without settling another pending request', () => {
  const ready = programWorkspaceReducer(INITIAL_PROGRAM_WORKSPACE_STATE, {
    type: 'request_succeeded', generation: 0, workspace: workspace('draft-1', 7),
    message: null, transitionStatus: null,
  });
  const pending = programWorkspaceReducer(ready, { type: 'request_started', generation: 1, action: 'mutate' });
  const reported = programWorkspaceReducer(pending, { type: 'close_canceled',
    message: 'Close cleanup canceled. 1 Scene object removal(s) confirmed; editors were kept.',
  });
  expect(reported.pendingAction).toBe('mutate');
  expect(reported.requestGeneration).toBe(1);
  expect(reported.workspace).toBe(pending.workspace);
  expect(reported.message).toContain('1 Scene object removal(s) confirmed');
});
