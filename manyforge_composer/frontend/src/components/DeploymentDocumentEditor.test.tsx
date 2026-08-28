import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { DeploymentDocumentEditor } from './DeploymentDocumentEditor';

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
      unavailableReasons: [],
    },
    loading: false,
    busy: false,
    error: null,
    feedback: null,
    mutationAvailable: true,
    mutationUnavailableReason: null,
    refresh: vi.fn(),
    clearFeedback: vi.fn(),
    open: vi.fn(),
    mutate: vi.fn(),
    save: vi.fn(),
    saveAs: vi.fn(),
    discard: vi.fn(),
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
});
