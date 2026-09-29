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
  it('places one conditional Scene source toggle after the camera controls', () => {
    const viewer = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url), 'utf8',
    );
    const scene = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url), 'utf8',
    );
    const header = viewer.slice(viewer.indexOf('className="viewer-header-controls"'));
    expect(header.indexOf('className="viewer-tool-strip"'))
      .toBeLessThan(header.indexOf('{sceneSourceControl}'));
    expect(scene).toContain('sceneSourceControl={draftSceneDifferent ? (');
    expect(scene).toContain('className="viewer-scene-source-toggle"');
    expect(scene).toContain("setSceneSourceChoice(programDraftPreview ? 'applied' : 'draft')");
    expect(scene).toContain("programDraftPreview ? 'Draft Scene' : 'Applied Scene'");
    expect(scene).toContain('robot models and observation status remain from Applied Deployment');
    expect(scene).not.toContain('Draft objects · Applied robots');
    // While execution is not Idle the live Scene is shown and the Draft is
    // neither previewed nor compared.
    expect(scene).toContain('const draftSceneDifferent = !executionActive && ');
    expect(scene).toContain('const programDraftPreview = !executionActive && sceneSourceIsDraft(');
    expect(scene).toContain('ownerSceneKnown && !executionActive');
  });

  it('wires fenced New, Revert, and Discard with concrete prerequisites', () => {
    const source = readFileSync(
      new URL('./ProgramDocumentControls.tsx', import.meta.url),
      'utf8',
    );
    const scenarioSource = readFileSync(
      new URL('./ScenarioDocumentCommands.tsx', import.meta.url),
      'utf8',
    );

    expect(scenarioSource).toContain('New Scenario');
    expect(scenarioSource).toContain('newDraft(confirmDiscard)');
    expect(source).toContain('Revert to saved');
    expect(source).toContain('Discard changes');
    expect(source).toContain('return to the Applied Program');
    expect(source).toContain(
      'disabled={reasons.discard !== null}',
    );
    expect(source).toContain('disabled={reasons.revert !== null}');
    expect(source).toContain('programActionUnavailableReason');
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
    expect(source).toContain('View Applied Program and Binding (JSON)');
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

  it('reports an accepted Apply as pending with its exact identity', () => {
    const result = programWorkspaceTransitionResult('apply', {
      status: 'accepted',
      headless: { apply_id: 'apply-42' },
      workspace: workspace('pending-apply', 12),
    });
    expect(result.transitionStatus).toBe('accepted');
    expect(result.message).toContain('apply-42');
    expect(result.message).not.toContain('completed');
  });

  it('loads a Scenario through one activation operation with only conditional dirty-work confirmation', () => {
    const source = readFileSync(
      new URL('./ProgramDocumentControls.tsx', import.meta.url),
      'utf8',
    );
    const scenarioSource = readFileSync(
      new URL('./ScenarioDocumentCommands.tsx', import.meta.url),
      'utf8',
    );
    expect(source).toContain('Apply this Program Draft to');
    expect(source).toContain('Its Deployment will not change.');
    expect(scenarioSource).toContain('loadScenarioDocument(');
    expect(scenarioSource).not.toContain('loadScenario(');
    expect(source).toContain('currentScenarioProjection?.currentScenario');
    expect(source).not.toContain('applyDraft(selectedScenario');
    expect(source).not.toContain('Load and Apply Program');
    expect(scenarioSource).toContain('Load replaces the current Scenario and applies its Deployment, Scene and Program.');
    expect(scenarioSource).toContain('Replace current unsaved or unapplied Draft changes');
    expect(scenarioSource).not.toContain('Open as Draft only');
    expect(scenarioSource).not.toContain('confirmScenarioDeployment');
    expect(scenarioSource).toContain('The list shows saved metadata, not current readiness.');
    expect(scenarioSource).toContain('The selected Scenario is validated when you load it.');
    expect(scenarioSource).not.toContain('scenarioStatusLabel');
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
    // Failures and transitions stay in the workbench notification rows. A
    // non-Idle cycle is the direct consequence of a Run: it is an editor
    // status beside the canvas, never a notification row.
    const appSource = readFileSync(new URL('../App.tsx', import.meta.url), 'utf8');
    expect(appSource).toContain('authoringUnavailableCause(authoringWorkspace.published, headlessSnapshot)');
    expect(appSource).toContain("authoringCause === 'not_admitted'");
    expect(appSource).not.toContain("authoringCause === 'execution_active'");
    expect(appSource).not.toContain('Authoring waits for Idle');
    expect(workspaceSource).toContain(
      'Run active. Draft inspection remains available; authoring waits for Idle.',
    );
    expect(workspaceSource).toContain('Authoring unavailable. ${mutationUnavailableReason}');
    expect(workspaceSource).toContain('dismissible: false');
    expect(workspaceSource).toContain('? mutationUnavailableReason');
    expect(controlsSource).toContain('!mutationAvailable ? mutationUnavailableReason');
    expect(workspaceSource).not.toContain('affect only later runs');
  });
});
