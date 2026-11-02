import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ResetApplyController } from '../hooks/useResetApply';
import { applyPreviewFixture } from '../test/applyPreview';
import { DeploymentDocumentEditor } from './DeploymentDocumentEditor';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import { authoringOperation } from '../state/busyOperation';
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
    busyOperation: null,
    applyConfirmationOpen: false,
    applyPreview: null,
    applyPreviewKind: 'apply',
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
    applyConfirmedScope: vi.fn(),
    addComponents: vi.fn(),
    previewApply: vi.fn(),
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

  it('shows the Preview\'s per-component impact in the Apply confirmation (CONTRACTS §11)', () => {
    const previewed = controller();
    previewed.applyConfirmationOpen = true;
    previewed.applyPreview = applyPreviewFixture([
      { component_id: 'lite6_robot', disposition: 'replace', reasons: ['mount'], restarts: true },
      { component_id: 'lite_gripper', disposition: 'replace', reasons: ['mount'], restarts: true },
      { component_id: 'camera', disposition: 'retain', reasons: [], restarts: false },
    ]);
    const markup = scenarioActions(previewed);
    expect(markup).toContain('Apply Deployment configuration change?');
    expect(markup).toContain('Restarts lite6_robot and lite_gripper; the others keep running.');
    expect(markup).toContain('Affected components come back prepared');
    expect(markup).toContain('Confirm apply');
    expect(markup).not.toContain('Deactivate');
  });

  it('offers Reset run and Apply when a stopped or held run blocks the Deployment Apply (AUD-4)', () => {
    const previewed = controller();
    previewed.applyConfirmationOpen = true;
    previewed.applyPreview = applyPreviewFixture([
      { component_id: 'lite6_robot', disposition: 'replace', reasons: ['mount'], restarts: true },
    ]);
    const resetApply = {
      runExists: true, deploymentRunReset: true, unavailableReason: () => null, confirm: vi.fn(),
    } as unknown as ResetApplyController;
    const held = renderToStaticMarkup(
      <ScenarioActionControls program={settledProgram} deployment={previewed} runtime={runtime} resetApply={resetApply} />,
    );
    expect(held).toContain('Applying Resets the current run.');
    expect(held).toContain('Reset run and Apply');
    // A finished run awaiting Reset: the server's Apply resets it itself.
    const finished = renderToStaticMarkup(
      <ScenarioActionControls program={settledProgram} deployment={previewed} runtime={runtime}
        resetApply={{ ...resetApply, deploymentRunReset: false }} />,
    );
    expect(finished).not.toContain('Reset run and Apply');
    expect(finished).toContain('Confirm apply');
  });

  it('confirms a Deployment Revert through its Preview, with the Program warning (CONTRACTS §11)', () => {
    const previewed = controller();
    previewed.applyConfirmationOpen = true;
    previewed.applyPreviewKind = 'revert';
    previewed.applyPreview = applyPreviewFixture([
      { component_id: 'lite6_robot', disposition: 'remove', reasons: [], restarts: false },
    ], { program: { executable: false, program_id: 'pick', unresolved_references: [{
      component_role_id: 'arm', target_role_id: null, controllable_component_id: 'lite6_robot',
      command_target_id: null, reason: 'component_removed', detail: '' }],
    affected_nodes: [], affected_execution_groups: [] } });
    const markup = scenarioActions(previewed);
    expect(markup).toContain('Revert Deployment to saved?');
    expect(markup).toContain('Removes lite6_robot.');
    expect(markup).toContain('will not be executable after this Apply');
    expect(markup).toContain('Confirm revert');
  });

  it('opens the Apply confirmation at once with "Previewing impact…" and holds every Scenario action', () => {
    const previewing = controller();
    const operation = authoringOperation('apply_preview');
    Object.assign(previewing, { busy: true, busyOperation: operation, applyConfirmationOpen: true });
    // The shared authoring gate names the operation to the Program too.
    const program = { ...settledProgram, busyOperation: operation,
      documentUnavailableReason: 'Apply preview is in progress.',
      mutationUnavailableReason: 'Apply preview is in progress.' } as unknown as ProgramWorkspaceController;
    const markup = renderToStaticMarkup(
      <ScenarioActionControls program={program} deployment={previewing} runtime={runtime} />,
    );
    expect(markup).toContain('Apply Deployment configuration change?');
    expect(markup).toContain('Previewing impact…');
    expect(markup).toMatch(/<button[^>]*disabled=""[^>]*>Previewing…<\/button>/);
    expect(markup).toMatch(/<button[^>]*disabled=""[^>]*title="Previewing impact…"[^>]*>Confirm apply<\/button>/);
    expect(markup).toMatch(/<button[^>]*disabled=""[^>]*>Discard<\/button>/);
    expect(markup).toMatch(/<button[^>]*disabled=""[^>]*>Revert<\/button>/);
  });
});
