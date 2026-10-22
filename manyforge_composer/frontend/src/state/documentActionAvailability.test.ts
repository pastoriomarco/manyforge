import { describe, expect, it } from 'vitest';
import { programActionUnavailableReason } from './authoringWorkspace';
import {
  deploymentActionUnavailableReason,
  deploymentSavedDiffersFromApplied,
  deploymentUnapplied,
  scenarioActionRoute,
} from './documentStates';
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
      savedScenarioCandidates: [{}], saved: { deploymentRevision: 'saved', differsFromApplied: null },
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

  it('routes a Draft-only edit over a reverted Saved Deployment to Discard, never Revert', () => {
    // The live shape: an intent Draft and Saved carry no revision, so only the
    // server knows that Saved is the Applied Deployment.
    const workspace = {
      draft: { document: {}, deploymentRevision: null, differsFromSaved: true },
      saved: { path: 'cell/deployment.yaml', deploymentRevision: null, differsFromApplied: false },
      managerApplied: { activationOrder: null, deploymentRevision: `sha256:${'a'.repeat(64)}`, scenarioId: 'cell' },
      runAdmission: { ready: false, reason: 'Deployment Draft differs from Manager Applied' },
    } as unknown as DeploymentWorkspaceResponse;
    const reasons = (action: 'apply' | 'revert' | 'discard') => ({
      reason: deploymentActionUnavailableReason(workspace, action, null),
      pending: action === 'revert'
        ? deploymentSavedDiffersFromApplied(workspace) : deploymentUnapplied(workspace),
    });
    const program = { reason: 'Nothing to apply: the Program Draft matches Applied.', pending: false };
    expect(reasons('revert')).toEqual({
      reason: 'Nothing to revert: the Saved Deployment matches Manager Applied.', pending: false,
    });
    expect(scenarioActionRoute('revert', program, reasons('revert')).target).toBeNull();
    expect(scenarioActionRoute('discard', program, reasons('discard'))).toEqual({ target: 'scenario', reason: null });
    // Unknown on the server keeps today's answer: Revert stays available.
    workspace.saved.differsFromApplied = null;
    expect(scenarioActionRoute('revert', program, reasons('revert')).target).toBe('deployment');
  });
});
