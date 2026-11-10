import { act, useCallback, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  NodeEnvironmentResponse,
  ProgramAuthoringDocument,
  ProgramWorkspaceResponse,
} from '../types/api';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import { ProgramWorkspace } from './ProgramWorkspace';

vi.mock('./ProgramTreeCanvas', () => ({
  ProgramTreeCanvas: ({ onSelect }: { onSelect?: (id: string | null) => void }) => (
    <button
      type="button"
      data-testid="canvas-select"
      onClick={() => onSelect?.('late_root')}
    >
      Select root from canvas
    </button>
  ),
}));

const environment: NodeEnvironmentResponse = {
  environmentDigest: 'sha256:environment',
  implementations: [],
  portableAuthoringAvailable: true,
  executionAvailable: false,
  reasonCodes: [],
  restorationGates: [],
  active: null,
};

function workspaceWithDraft(): ProgramWorkspaceResponse {
  const authoring: ProgramAuthoringDocument = {
    api_version: 'manyforge.program.v3',
    program_id: 'selection-test',
    program_profile: 'composable-cell-v1',
    name: 'Selection test',
    catalog: { api_version: 'manyforge.node_catalog.v3', content_version: '2.4.0' },
    parameters: [],
    blackboard: { namespaces: [], keys: [] },
    schema_version: 'manyforge.composer_authoring_forest.v2',
    forest: [{
      node_id: 'test.leaf',
      instance_id: 'late_root',
      parameters: {},
      children: [],
    }],
    positions: {},
  };

  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: 1,
      programDraftId: 'draft-appeared',
      programDraftRevision: 1,
      deploymentDraftId: 'deployment-draft-1',
      deploymentDraftRevision: 1,
    },
    draft: {
      draftId: 'draft-appeared',
      draftRevision: 1,
      programLoaded: true,
      authoring,
      candidateAvailable: true,
      programRevision: null,
      program: null,
      diagnostics: [],
      formulaReferences: { computedKeys: {}, nodes: {} },
    },
    applicability: { applyReady: true, unavailableReason: null },
    saved: { available: false, matchesDraft: false, path: null, programRevision: null },
    applied: { available: false, matchesDraft: null, programRevision: null, unavailableReason: null },
    hasUnsavedChanges: false,
    hasUnappliedChanges: null,
  };
}

function controllerFor(workspace: ProgramWorkspaceResponse | null): ProgramWorkspaceController {
  return {
    state: {
      status: 'ready',
      requestGeneration: 1,
      pendingAction: null,
      workspace,
      message: null,
      transitionStatus: null,
      error: null,
    },
    mutationAvailable: true,
    mutationUnavailableReason: null,
    refresh: vi.fn(),
    mutate: vi.fn().mockResolvedValue(null),
    updateNodePolicies: vi.fn(),
    updateNodePresentation: vi.fn(),
  } as unknown as ProgramWorkspaceController;
}

const runtime = {
  state: { run: null, applied: null },
  cycleState: null,
  active: false,
} as unknown as ProgramRuntimeController;

afterEach(() => vi.unstubAllGlobals());

describe('ProgramWorkspace controlled selection', () => {
  it('uses the current Draft callback when the canvas selects after a Draft appears', async () => {
    vi.stubGlobal('requestAnimationFrame', () => 1);
    vi.stubGlobal('cancelAnimationFrame', () => undefined);
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
    vi.stubGlobal('MutationObserver', class { observe() {} disconnect() {} });

    const selectionCalls: Array<[string | null, string | null]> = [];
    const { host, find } = reactPropsHost();
    const root = createRoot(host as unknown as Element);

    function Harness() {
      const [workspace, setWorkspace] = useState<ProgramWorkspaceResponse | null>(null);
      const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
      const visibleDraftId = workspace?.draft.draftId ?? null;
      const onSelectedNodeIdChange = useCallback((id: string | null) => {
        selectionCalls.push([visibleDraftId, id]);
        setSelectedNodeId(id);
      }, [visibleDraftId]);

      return (
        <>
          <button type="button" data-testid="open-draft" onClick={() => setWorkspace(workspaceWithDraft())}>
            Open Draft
          </button>
          <output data-testid="selected-node" data-selected-node={selectedNodeId ?? ''} />
          <ProgramWorkspace
            selectedNodeId={selectedNodeId}
            onSelectedNodeIdChange={onSelectedNodeIdChange}
            controller={controllerFor(workspace)}
            runtime={runtime}
            catalog={{
              status: 'ready',
              data: {
                apiVersion: 'manyforge.node_catalog.v3',
                contentVersion: '2.4.0',
                catalogId: 'selection-test',
                entries: [],
                catalogDigest: 'sha256:catalog',
                environmentDigest: environment.environmentDigest,
              },
            }}
            environment={{ status: 'ready', data: environment }}
            sourceModel={{ status: 'absent', message: 'Not needed for selection.' }}
            onRetryCatalog={vi.fn()}
            onRetryEnvironment={vi.fn()}
          />
        </>
      );
    }

    await act(() => root.render(<Harness />));
    try {
      await act(() => (find((props) => props['data-testid'] === 'open-draft') as MountedProps).onClick?.());
      const callsBeforeCanvasSelection = selectionCalls.length;
      await act(() => (find((props) => props['data-testid'] === 'canvas-select') as MountedProps).onClick?.());

      expect(selectionCalls.slice(callsBeforeCanvasSelection)).toEqual([['draft-appeared', 'late_root']]);
      expect(find((props) => props['data-testid'] === 'selected-node')['data-selected-node'])
        .toBe('late_root');
    } finally {
      await act(() => root.unmount());
    }
  });
});
