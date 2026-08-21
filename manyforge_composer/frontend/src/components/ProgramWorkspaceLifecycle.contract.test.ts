import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type {
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
} from '../types/api';
import { programWorkspaceTransitionResult } from '../hooks/useProgramWorkspace';

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
      unavailableReason: null,
    },
    hasUnsavedChanges: false,
    hasUnappliedChanges: null,
  };
}

function transition(
  status: ProgramWorkspaceTransitionResponse['status'],
  ownerWorkspace: ProgramWorkspaceResponse,
): ProgramWorkspaceTransitionResponse {
  return { status, headless: {}, workspace: ownerWorkspace };
}

describe('Program workspace lifecycle controls', () => {
  it('wires fenced New, Revert, and Discard with concrete prerequisites', () => {
    const source = readFileSync(
      new URL('./ProgramWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('New Program');
    expect(source).toContain('newDraft(confirmDiscard)');
    expect(source).toContain('Revert to Saved');
    expect(source).toContain('Discard to Applied');
    expect(source).toContain('disabled={busy || !workspace.applied.available}');
    expect(source).toContain('workspace.saved.available');
    expect(source).not.toContain('localStorage');
    expect(source).not.toContain('sendBeacon');
  });

  it('reports pending and failure through the shared workspace controller', () => {
    const source = readFileSync(
      new URL('../hooks/useProgramWorkspace.ts', import.meta.url),
      'utf8',
    );

    expect(source).toContain("'discard'");
    expect(source).toContain("'new'");
    expect(source).toContain("'revert'");
    expect(source).toContain('api.programWorkspace.discard(fence(workspace))');
    expect(source).toContain('api.programWorkspace.revert(fence(workspace))');
    expect(source).not.toContain('const revert = useCallback((\\n    scenarioId');
    expect(source).toContain("type: 'request_failed'");
    expect(source).toContain(
      'Revert is pending deployment; Draft and Saved were not replaced.',
    );
    expect(source).not.toContain(
      'Saved Program reverted through authoritative Apply; Draft now matches Saved.',
    );
  });

  it('links to exact Applied JSON without rendering the full documents', () => {
    const source = readFileSync(
      new URL('./ProgramRuntimePanel.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('href="/api/program/applied"');
    expect(source).toContain('Inspect exact Applied Program and Binding JSON');
    expect(source).not.toContain('JSON.stringify(state.applied');
  });

  it('preserves Revert status truth and the exact owner-returned workspace', () => {
    const pendingWorkspace = workspace('owner-pending', 8);
    const pending = programWorkspaceTransitionResult(
      'revert',
      transition('pending_deployment', pendingWorkspace),
    );
    const revertedWorkspace = workspace('owner-reverted', 9);
    const reverted = programWorkspaceTransitionResult(
      'revert',
      transition('reverted', revertedWorkspace),
    );

    expect(pending.workspace).toBe(pendingWorkspace);
    expect(pending.transitionStatus).toBe('pending_deployment');
    expect(pending.message).toContain('pending deployment');
    expect(pending.message).toContain('Draft and Saved were not replaced');
    expect(pending.message).not.toContain('Draft now matches Saved');
    expect(reverted.workspace).toBe(revertedWorkspace);
    expect(reverted.transitionStatus).toBe('reverted');
    expect(reverted.message).toContain('Draft now matches Saved');
    expect(reverted.message).not.toContain('pending deployment');
  });

  it('reports a no-effect Revert without fabricating an Apply', () => {
    const ownerWorkspace = workspace('owner-no-effect', 4);
    const result = programWorkspaceTransitionResult('revert', {
      status: 'reverted',
      headless: null,
      workspace: ownerWorkspace,
    });

    expect(result.workspace).toBe(ownerWorkspace);
    expect(result.message).toContain('No owner Apply was issued');
    expect(result.message).not.toContain('through authoritative Apply');
  });

  it('does not flatten Apply pending into an applied-success claim', () => {
    const ownerWorkspace = workspace('apply-pending', 11);
    const result = programWorkspaceTransitionResult(
      'apply',
      transition('pending_deployment', ownerWorkspace),
    );

    expect(result.workspace).toBe(ownerWorkspace);
    expect(result.transitionStatus).toBe('pending_deployment');
    expect(result.message).toBe(
      'Apply is pending deployment; Applied was not replaced.',
    );
    expect(result.message).not.toContain('Apply completed');
  });
});
