import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type {
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
} from '../types/api';
import { programWorkspaceTransitionResult } from '../hooks/useProgramWorkspace';

function workspace(draftId: string, revision: number): ProgramWorkspaceResponse {
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    draft: {
      draftId,
      draftRevision: revision,
      programLoaded: false,
      authoring: null,
      candidateAvailable: false,
      programRevision: null,
      program: null,
      diagnostics: [],
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
      new URL('./ProgramDocumentControls.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('New Program');
    expect(source).toContain('newDraft(confirmDiscard)');
    expect(source).toContain('Revert to Saved');
    expect(source).toContain('Discard changes');
    expect(source).toContain('return to the Applied Program');
    expect(source).toContain(
      'disabled={authoringBusy || workspace?.hasUnappliedChanges !== true}',
    );
    expect(source).toContain('disabled={authoringBusy || !hasAppliedUnsavedChanges}');
    expect(source).toContain('workspace?.saved.available');
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
    expect(source).toContain('api.programWorkspace.discard(requestFence)');
    expect(source).toContain('api.programWorkspace.revert(requestFence)');
    expect(source).toContain('authoring.unavailableReason');
    const admissionSource = readFileSync(
      new URL('../state/authoringWorkspace.ts', import.meta.url),
      'utf8',
    );
    expect(admissionSource).toContain("snapshot.programCycle.state !== 'idle_after_reset'");
    expect(source).not.toContain('const revert = useCallback((\\n    scenarioId');
    expect(source).toContain("type: 'request_failed'");
    expect(source).toContain(
      'Revert completed through authoritative Apply; Draft now matches Saved.',
    );
    expect(source).not.toContain('pending deployment');
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
    const revertedWorkspace = workspace('owner-reverted', 9);
    const reverted = programWorkspaceTransitionResult(
      'revert',
      transition('reverted', revertedWorkspace),
    );

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

  it('reports Apply only after the backend completed it', () => {
    const ownerWorkspace = workspace('apply-complete', 11);
    const result = programWorkspaceTransitionResult(
      'apply',
      transition('applied', ownerWorkspace),
    );

    expect(result.workspace).toBe(ownerWorkspace);
    expect(result.transitionStatus).toBe('applied');
    expect(result.message).toContain('Apply completed');
  });

  it('keeps Program Apply separate while Load remains editor-only', () => {
    const source = readFileSync(
      new URL('./ProgramDocumentControls.tsx', import.meta.url),
      'utf8',
    );
    expect(source).toContain('Apply this Program Draft to');
    expect(source).toContain('Its Deployment will not change.');
    expect(source).toContain('Runtime state is not changed.');
    expect(source).toContain('loadScenarioDocument(');
    expect(source).not.toContain('loadScenario(');
    expect(source).toContain('currentScenarioProjection?.currentScenario');
    expect(source).not.toContain('applyDraft(selectedScenario');
    expect(source).not.toContain('Load and Apply Program');
  });

  it('reflects backend idle admission instead of silently dropping UI actions', () => {
    const workspaceSource = readFileSync(
      new URL('./ProgramWorkspace.tsx', import.meta.url),
      'utf8',
    );
    const controlsSource = readFileSync(
      new URL('./ProgramDocumentControls.tsx', import.meta.url),
      'utf8',
    );
    expect(workspaceSource).toContain(
      'busy || authoringUnavailable || !mutationAvailable',
    );
    expect(workspaceSource).toContain(
      'workspace !== null && !mutationAvailable',
    );
    expect(workspaceSource).toContain('{mutationUnavailableReason}');
    expect(controlsSource).toContain('busy || !mutationAvailable');
    expect(workspaceSource).not.toContain('affect only later runs');
  });
});
