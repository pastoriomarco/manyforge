import { describe, expect, it } from 'vitest';
import { programActionUnavailableReason } from './authoringWorkspace';
import { deploymentActionUnavailableReason } from './documentStates';
import type { DeploymentWorkspaceResponse, ProgramWorkspaceResponse } from '../types/api';

describe('document action prerequisites shared by controls and handlers', () => {
  it('disables Program Revert while Saved equals Applied, even with Draft edits', () => {
    const workspace = {
      saved: { available: true, matchesDraft: false, programRevision: 'saved' },
      applied: { available: true, matchesDraft: false, programRevision: 'saved' },
    } as ProgramWorkspaceResponse;
    expect(programActionUnavailableReason(workspace, 'revert', null))
      .toBe('Nothing to revert: the Saved Program matches Applied.');
    workspace.saved.matchesDraft = true;
    expect(programActionUnavailableReason(workspace, 'revert', null)).not.toBe(null);
    workspace.applied.programRevision = 'older';
    expect(programActionUnavailableReason(workspace, 'revert', null)).toBe(null);
    workspace.saved.matchesDraft = false;
    expect(programActionUnavailableReason(workspace, 'revert', null)).toBe(null);
  });

  it('allows Discard of an invalid Program Draft but refuses Apply', () => {
    const workspace = {
      draft: { program: null }, applicability: { applyReady: false },
      applied: { available: true }, hasUnappliedChanges: true,
    } as ProgramWorkspaceResponse;
    expect(programActionUnavailableReason(workspace, 'discard', null)).toBe(null);
    expect(programActionUnavailableReason(workspace, 'apply', null)).not.toBe(null);
    expect(programActionUnavailableReason(workspace, 'discard', 'Manager pending')).toBe('Manager pending');
  });

  it('disables Deployment Revert while Saved equals Applied and keeps the admission reason', () => {
    const workspace = {
      savedScenarioCandidates: [{}], saved: { deploymentRevision: 'saved' },
      managerApplied: { activationOrder: null, deploymentRevision: 'saved' }, draft: { differsFromSaved: true },
    } as DeploymentWorkspaceResponse;
    expect(deploymentActionUnavailableReason(workspace, 'revert', null))
      .toBe('Nothing to revert: the Saved Deployment matches Manager Applied.');
    workspace.managerApplied.deploymentRevision = 'older';
    expect(deploymentActionUnavailableReason(workspace, 'revert', null)).toBe(null);
    workspace.draft.differsFromSaved = false;
    expect(deploymentActionUnavailableReason(workspace, 'revert', null)).toBe(null);
    expect(deploymentActionUnavailableReason(workspace, 'revert', 'Backend unavailable')).toBe('Backend unavailable');
  });
});
