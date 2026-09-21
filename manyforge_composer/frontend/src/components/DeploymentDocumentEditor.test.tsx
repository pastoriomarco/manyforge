import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { DeploymentDocumentEditor } from './DeploymentDocumentEditor';
import { DeploymentDocumentControls } from './DeploymentDocumentControls';

function controller(): DeploymentWorkspaceController {
  return {
    workspace: {
      schemaVersion: 'manyforge.composer_deployment_workspace.v1',
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
      saved: { path: '/workspace/cell-a.deployment.yaml', deploymentRevision: null },
      managerApplied: { deploymentRevision: null, scenarioId: null },
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
    feedback: null,
    pendingApplyConfirmation: null,
    mutationAvailable: true,
    mutationUnavailableReason: null,
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
    confirmApply: vi.fn(),
    cancelApplyConfirmation: vi.fn(),
  };
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
    postApply.workspace = {
      ...postApply.workspace,
      draft: {
        ...postApply.workspace.draft,
        deploymentRevision: `sha256:${'2'.repeat(64)}`,
        differsFromSaved: true,
      },
      saved: {
        path: null,
        deploymentRevision: `sha256:${'1'.repeat(64)}`,
      },
      managerApplied: {
        deploymentRevision: `sha256:${'2'.repeat(64)}`,
        scenarioId: 'cell-a',
      },
      // Publication identity, rather than the transient runtime admission
      // projection, owns the document buttons while Apply settles.
      runAdmission: { ready: false, reason: 'runtime still converging' },
      savedScenarioCandidates: [{
        scenarioId: 'cell-a',
        scenarioCatalogRevision: `sha256:${'3'.repeat(64)}`,
        deploymentArtifactId: 'cell-a',
        deploymentRevision: `sha256:${'1'.repeat(64)}`,
        retainedBundleId: `sha256:${'4'.repeat(64)}`,
        programId: 'program-a',
        programRevision: `sha256:${'5'.repeat(64)}`,
        programBindingId: `sha256:${'6'.repeat(64)}`,
        description: null,
      }],
    };
    const appliedMarkup = renderToStaticMarkup(
      <DeploymentDocumentControls controller={postApply} />,
    );
    expect(appliedMarkup).toMatch(
      /<button[^>]*disabled=""[^>]*title="Draft already matches Manager Applied\."[^>]*>Discard<\/button>/,
    );
    expect(appliedMarkup).toMatch(/state-applied">applied<\/span>/);
    expect(appliedMarkup).toMatch(
      /<button[^>]*disabled=""[^>]*title="Draft already matches Manager Applied\."[^>]*>Apply<\/button>/,
    );
    expect(appliedMarkup).toMatch(/<button[^>]*>Revert<\/button>/);

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
      managerApplied: {
        deploymentRevision: `sha256:${'1'.repeat(64)}`,
        scenarioId: 'cell-a',
      },
      runAdmission: {
        ready: false,
        reason: 'Deployment Draft differs from Manager Applied',
      },
    };
    const unappliedMarkup = renderToStaticMarkup(
      <DeploymentDocumentControls controller={unapplied} />,
    );
    expect(unappliedMarkup).toMatch(
      /<button[^>]*title="Discard Draft changes and return to Manager Applied\."[^>]*>Discard<\/button>/,
    );
  });
});
