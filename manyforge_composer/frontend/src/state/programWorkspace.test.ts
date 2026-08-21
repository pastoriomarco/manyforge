import { describe, expect, it } from 'vitest';

import type { ProgramWorkspaceResponse } from '../types/api';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
  replaceProgramWorkspaceDraft,
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
    applicability: {
      applyReady: true,
      affectedInstanceIds: [],
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

  it('shows the closed Draft mutation before refreshing owner observations', () => {
    const current = workspace('draft-1', 7);
    const returnedDraft: ProgramWorkspaceResponse['draft'] = {
      ...current.draft,
      draftRevision: 8,
    };
    const started = programWorkspaceReducer({
      ...INITIAL_PROGRAM_WORKSPACE_STATE,
      status: 'ready',
      pendingAction: null,
      workspace: current,
    }, {
      type: 'request_started',
      generation: 11,
      action: 'update_policies',
    });
    const updated = programWorkspaceReducer(started, {
      type: 'policy_mutation_succeeded',
      generation: 11,
      workspace: { ...current, draft: returnedDraft },
      message: 'Draft updated; Draft revision 8. Workspace owner status is refreshing.',
    });

    expect(updated.pendingAction).toBe('refresh');
    expect(updated.comparisonStatus).toBe('refreshing');
    expect(updated.workspace?.draft.draftRevision).toBe(8);
    expect(updated.workspace?.saved).toBe(current.saved);
    expect(updated.workspace?.applied).toBe(current.applied);
  });

  it('retains a Draft-only success when the separate owner refresh fails', () => {
    const current = workspace('draft-1', 7);
    const started = programWorkspaceReducer({
      ...INITIAL_PROGRAM_WORKSPACE_STATE,
      status: 'ready',
      pendingAction: null,
      workspace: current,
    }, {
      type: 'policy_mutation_succeeded',
      generation: 12,
      workspace: { ...current, draft: { ...current.draft, draftRevision: 8 } },
      message: 'Draft updated; Draft revision 8.',
    });
    const failed = programWorkspaceReducer(started, {
      type: 'workspace_refresh_failed',
      generation: 12,
      warning: 'Draft updated; workspace status refresh failed. Your Draft change remains; retry refresh.',
    });

    expect(failed.status).toBe('ready');
    expect(failed.pendingAction).toBeNull();
    expect(failed.workspace?.draft.draftRevision).toBe(8);
    expect(failed.refreshWarning).toBe(
      'Draft updated; workspace status refresh failed. Your Draft change remains; retry refresh.',
    );
    expect(failed.comparisonStatus).toBe('stale');
  });

  it('recomputes all Draft-relative comparisons from the closed mutation Draft', () => {
    const current = workspace('draft-1', 7);
    const revision = `sha256:${'1'.repeat(64)}`;
    const returnedDraft = {
      ...current.draft,
      draftRevision: 8,
      programLoaded: true,
      programRevision: revision,
      program: {
        api_version: 'manyforge.program.v1',
        program_id: 'program-1',
        program_revision: revision,
        program_profile: 'composable-cell-v1',
        name: 'Program',
        catalog: {
          api_version: 'manyforge.node_catalog.v2',
          content_version: '1',
        },
        component_roles: {},
        target_roles: {},
        tree: {
          node_id: 'timer',
          instance_id: 'root',
          parameters: {},
          children: [],
        },
      },
    };
    const withOwners: ProgramWorkspaceResponse = {
      ...current,
      draft: {
        ...current.draft,
        programLoaded: true,
        programRevision: `sha256:${'0'.repeat(64)}`,
        program: returnedDraft.program,
      },
      applicability: {
        applyReady: true,
        affectedInstanceIds: [],
        unavailableReason: null,
      },
      saved: {
        available: true,
        matchesDraft: false,
        path: '/program.program.yaml',
        programRevision: revision,
      },
      applied: {
        available: true,
        matchesDraft: false,
        programRevision: `sha256:${'2'.repeat(64)}`,
        unavailableReason: null,
      },
      hasUnsavedChanges: true,
      hasUnappliedChanges: true,
    };
    const replaced = replaceProgramWorkspaceDraft(withOwners, returnedDraft);

    expect(replaced.saved.matchesDraft).toBe(true);
    expect(replaced.applied.matchesDraft).toBe(false);
    expect(replaced.hasUnsavedChanges).toBe(false);
    expect(replaced.hasUnappliedChanges).toBe(true);
  });
});
