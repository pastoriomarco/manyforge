import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { DeploymentDocumentEditor } from './DeploymentDocumentEditor';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import { ScenarioActionControls } from './ScenarioActionControls';

function controller(): DeploymentWorkspaceController {
  return {
    workspace: {
      schemaVersion: 'manyforge.composer_deployment_workspace.v1',
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
        programDraftId: 'draft-1', programDraftRevision: 1,
        deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
      },
      draft: {
        id: 'deployment-draft-1',
        revision: 1,
        document: {
          api_version: 'manyforge.deployment.v1',
          deployment_artifact_id: 'cell-a',
          deployment_revision: `sha256:${'1'.repeat(64)}`,
          cell_id: 'cell-a',
          name: 'Cell A',
          graph: { runtime_units: [] },
          assistant: { provider: 'local', profile: 'bounded' },
          extension_section: { enabled: true },
        },
        deploymentArtifactId: 'cell-a',
        deploymentRevision: `sha256:${'1'.repeat(64)}`,
        graphRevision: `sha256:${'2'.repeat(64)}`,
        differsFromSaved: false,
      },
      saved: { path: '/workspace/cell-a.deployment.yaml', deploymentRevision: null, differsFromApplied: null },
      managerApplied: { activationOrder: null, deploymentRevision: null, scenarioId: null },
      runAdmission: { ready: false, reason: 'not applied' },
      binding: null,
      scenarioCandidates: [],
      savedScenarioCandidates: [],
      unavailableReasons: [],
    },
    loading: false,
    busy: false,
    applying: false,
    error: null,
    errorCauseCode: null,
    feedback: null,
    acceptedApply: null,
    acceptedApplyCorrelationId: null,
    activationScope: null,
    pendingApplyConfirmation: null,
    pendingConfirmationAction: null,
    comparisonUnavailableReason: null,
    mutationAvailable: true,
    mutationUnavailableReason: null,
    documentUnavailableReason: null,
    refresh: vi.fn(),
    clearFeedback: vi.fn(),
    dismissError: vi.fn(),
    open: vi.fn(),
    mutate: vi.fn(),
    updateControlledStop: vi.fn(),
    updateMotionTargetLimit: vi.fn(),
    revert: vi.fn(),
    save: vi.fn(),
    saveAs: vi.fn(),
    discard: vi.fn(),
    applyDraft: vi.fn(),
    addComponents: vi.fn(),
    previewDraft: vi.fn(),
    confirmApply: vi.fn(),
    cancelApplyConfirmation: vi.fn(),
  };
}

// A Program with nothing pending: Saved, Draft and Applied all match.
const settledProgram = {
  state: {
    status: 'ready',
    pendingAction: null,
    workspace: {
      draft: { program: {}, diagnostics: [] },
      saved: { available: true, programRevision: 'program.1' },
      applied: { available: true, programRevision: 'program.1', matchesDraft: true },
      hasUnappliedChanges: false,
      hasUnsavedChanges: false,
      applicability: { applyReady: true, unavailableReason: null },
    },
  },
  documentUnavailableReason: null,
  mutationUnavailableReason: null,
} as unknown as ProgramWorkspaceController;
const runtime = { state: { currentScenario: null } } as unknown as ProgramRuntimeController;

function scenarioActions(deployment: DeploymentWorkspaceController) {
  return renderToStaticMarkup(
    <ScenarioActionControls program={settledProgram} deployment={deployment} runtime={runtime} />,
  );
}

describe('DeploymentDocumentEditor', () => {
  it('keeps generated identities read-only and exposes every authored section', () => {
    const markup = renderToStaticMarkup(
      <DeploymentDocumentEditor controller={controller()} />,
    );

    expect(markup).toContain('value="identity" selected=""');
    expect(markup).toContain('value="graph"');
    expect(markup).toContain('value="assistant"');
    expect(markup).toContain('value="extension_section"');
    expect(markup).toContain('compiler-owned');
    expect(markup).toContain('readOnly=""');
  });

  it('enables Discard only for unapplied changes and keeps Revert for applied edits', () => {
    const postApply = controller();
    if (postApply.workspace === null) throw new Error('fixture workspace missing');
    // Applied but never saved: the Draft is Applied, the file is older.
    postApply.workspace = {
      ...postApply.workspace,
      draft: {
        ...postApply.workspace.draft,
        deploymentRevision: `sha256:${'2'.repeat(64)}`,
        differsFromSaved: true,
      },
      saved: {
        path: '/workspace/cell-a.scenario/deployment.yaml',
        deploymentRevision: `sha256:${'1'.repeat(64)}`,
        differsFromApplied: null,
      },
      managerApplied: { activationOrder: null,
        deploymentRevision: `sha256:${'2'.repeat(64)}`,
        scenarioId: 'cell-a',
      },
      // Publication identity, rather than the transient runtime admission
      // projection, owns the document buttons while Apply settles.
      runAdmission: { ready: false, reason: 'runtime still converging' },
    };
    const appliedMarkup = scenarioActions(postApply);
    expect(appliedMarkup).toMatch(
      /<button[^>]*disabled=""[^>]*title="Nothing to discard: the Scenario Drafts match Applied\."[^>]*>Discard<\/button>/,
    );
    expect(appliedMarkup).toMatch(
      /<button[^>]*disabled=""[^>]*title="Nothing to apply: the Scenario Drafts match Applied\."[^>]*>Apply<\/button>/,
    );
    // The Saved Deployment differs from Applied: Revert restores it and the Program.
    expect(appliedMarkup).toMatch(
      /<button type="button" title="Restore Draft and Applied to Saved: the Deployment and the Program\.">Revert<\/button>/,
    );

    const unapplied = controller();
    if (unapplied.workspace === null) throw new Error('fixture workspace missing');
    unapplied.workspace = {
      ...unapplied.workspace,
      draft: {
        ...unapplied.workspace.draft,
        document: { name: 'edited' },
        deploymentRevision: `sha256:${'2'.repeat(64)}`,
        differsFromSaved: true,
      },
      managerApplied: { activationOrder: null,
        deploymentRevision: `sha256:${'1'.repeat(64)}`,
        scenarioId: 'cell-a',
      },
      runAdmission: {
        ready: false,
        reason: 'Deployment Draft differs from Manager Applied',
      },
    };
    const unappliedMarkup = scenarioActions(unapplied);
    expect(unappliedMarkup).toMatch(
      /<button type="button" title="Discard unapplied Draft changes and return to Applied\.">Discard<\/button>/,
    );
    expect(unappliedMarkup).toMatch(
      /<button type="button" class="primary-action" title="Apply the Deployment and the Program\.">Apply<\/button>/,
    );
  });

  it('keeps Revert unavailable when no Saved Deployment exists', () => {
    const unsaved = controller();
    if (unsaved.workspace === null) throw new Error('fixture workspace missing');
    unsaved.workspace = { ...unsaved.workspace, saved: { path: null, deploymentRevision: null, differsFromApplied: null } };
    expect(scenarioActions(unsaved)).toMatch(
      /<button[^>]*disabled=""[^>]*title="Nothing to revert: the Saved Scenario matches Applied\."[^>]*>Revert<\/button>/,
    );
  });
});
