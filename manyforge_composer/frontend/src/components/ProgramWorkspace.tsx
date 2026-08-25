import { useEffect, useMemo, useState } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  CatalogResponse,
  NodeEnvironmentResponse,
} from '../types/api';
import { flattenProgramTree } from '../utils/programAuthoring';
import type { ProgramInsertionRequest } from '../utils/programCanvasInteraction';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import { ProgramNodePalette } from './ProgramNodePalette';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';

interface ProgramWorkspaceProps {
  controller: ProgramWorkspaceController;
  runtime: ProgramRuntimeController;
  catalog: StructuralResourceState<CatalogResponse>;
  environment: StructuralResourceState<NodeEnvironmentResponse>;
  onRetryCatalog: () => void;
  onRetryEnvironment: () => void;
}

export function ProgramWorkspace({
  controller,
  runtime,
  catalog,
  environment,
  onRetryCatalog,
  onRetryEnvironment,
}: ProgramWorkspaceProps) {
  const {
    state,
    mutationAvailable,
    refresh,
    mutate,
    updateNodePolicies,
    clearFeedback,
  } = controller;
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [insertionRequest, setInsertionRequest] = useState<ProgramInsertionRequest | null>(null);
  const [paletteCollapsed, setPaletteCollapsed] = useState(false);
  const [inspectorCollapsed, setInspectorCollapsed] = useState(false);
  const [inspectorView, setInspectorView] = useState<'properties' | 'declarations'>('properties');
  const workspace = state.workspace;
  const program = workspace?.draft.program ?? null;
  const busy = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const catalogEntries = catalog.status === 'ready' ? catalog.data.entries : [];
  const catalogIdentity = catalog.status === 'ready' ? {
    catalogId: catalog.data.catalogId,
    contentVersion: catalog.data.contentVersion,
    catalogDigest: catalog.data.catalogDigest,
    environmentDigest: catalog.data.environmentDigest,
  } : null;
  const nodeEnvironment = environment.status === 'ready' ? environment.data : null;
  const environmentAligned = catalog.status === 'ready'
    && environment.status === 'ready'
    && catalog.data.environmentDigest === environment.data.environmentDigest;
  const authoringUnavailable = nodeEnvironment !== null
    && !nodeEnvironment.portableAuthoringAvailable;
  const authoringBusy = busy || authoringUnavailable || !mutationAvailable;

  const validNodeIds = useMemo(
    () => new Set(program ? flattenProgramTree(program.tree)
      .map(({ node }) => node.instance_id) : []),
    [program],
  );
  useEffect(() => {
    if (!program) {
      setSelectedNodeId(null);
      return;
    }
    if (!selectedNodeId || !validNodeIds.has(selectedNodeId)) {
      setSelectedNodeId(program.tree.instance_id);
    }
  }, [program, selectedNodeId, validNodeIds]);

  const gridClassName = [
    'program-authoring-grid',
    program ? '' : 'program-authoring-empty',
    paletteCollapsed ? 'palette-collapsed' : '',
    inspectorCollapsed ? 'inspector-collapsed' : '',
  ].filter(Boolean).join(' ');

  return (
    <section className="panel program-workspace" aria-label="Program authoring workspace">
      <div className="program-workspace-notices">
        {state.status === 'loading' && workspace === null && (
          <div className="program-workspace-notice" role="status">
            Reading the Program workspace…
          </div>
        )}
        {state.status === 'error' && workspace === null && (
          <div className="program-workspace-notice error-state" role="alert">
            <span><strong>Program workspace unavailable.</strong> {state.error}</span>
            <button type="button" onClick={refresh}>Retry workspace</button>
          </div>
        )}
        {(state.message || state.error || state.refreshWarning) && workspace !== null && (
          <div
            className={`program-workspace-notice${state.error || state.refreshWarning ? ' error-state' : ''}`}
            role={state.error || state.refreshWarning ? 'alert' : 'status'}
          >
            <span>{state.error ?? state.refreshWarning ?? state.message}</span>
            <button type="button" className="text-button" onClick={clearFeedback}>Dismiss</button>
          </div>
        )}
        {runtime.active && (
          <div className="program-workspace-notice" role="status">
            <strong>Run active.</strong>
            <span>Draft inspection remains available; backend-owned authoring actions stay disabled until Idle.</span>
          </div>
        )}
        {!runtime.active && workspace !== null && !mutationAvailable && (
          <div className="program-workspace-notice" role="status">
            <strong>Authoring unavailable.</strong>
            <span>Waiting for current backend Idle admission and the matching Draft revision.</span>
          </div>
        )}
        {catalog.status !== 'ready' && (
          <div className="program-workspace-notice error-state" role="alert">
            <span><strong>Node catalog unavailable.</strong> {catalog.status === 'error' || catalog.status === 'absent' ? catalog.message : 'Loading…'}</span>
            <button type="button" onClick={onRetryCatalog}>Retry catalog</button>
          </div>
        )}
        {environment.status !== 'ready' && (
          <div className="program-workspace-notice error-state" role="alert">
            <span><strong>Authoring availability unavailable.</strong> {environment.status === 'error' || environment.status === 'absent' ? environment.message : 'Loading…'}</span>
            <button type="button" onClick={onRetryEnvironment}>Retry environment</button>
          </div>
        )}
        {catalog.status === 'ready' && environment.status === 'ready' && !environmentAligned && (
          <div className="program-workspace-notice error-state" role="alert">
            <span><strong>Catalog identity mismatch.</strong> Editing remains disabled.</span>
            <button type="button" onClick={onRetryCatalog}>Retry catalog</button>
            <button type="button" onClick={onRetryEnvironment}>Retry environment</button>
          </div>
        )}
        {environmentAligned && nodeEnvironment && authoringUnavailable && (
          <div className="program-workspace-notice error-state" role="alert">
            <span>
              <strong>Program mutations unavailable.</strong>{' '}
              {nodeEnvironment.reasonCodes.join(', ') || 'Portable authoring is not available.'}
            </span>
          </div>
        )}
      </div>

      <div className={gridClassName}>
        {paletteCollapsed ? (
          <button
            type="button"
            className="program-side-rail program-side-rail-left"
            aria-label="Expand node palette"
            onClick={() => setPaletteCollapsed(false)}
          >
            <span aria-hidden="true">›</span><span>Nodes</span>
          </button>
        ) : program ? (
          <ProgramNodePalette
            root={program.tree}
            catalogEntries={catalogEntries}
            catalogIdentity={catalogIdentity}
            environment={nodeEnvironment}
            selectedId={selectedNodeId}
            busy={authoringBusy}
            onMutate={mutate}
            dropRequest={insertionRequest}
            onDropRequestHandled={() => setInsertionRequest(null)}
            onCollapse={() => setPaletteCollapsed(true)}
          />
        ) : (
          <section className="program-palette" aria-label="Node palette">
            <header className="program-subpanel-header">
              <h3>Nodes</h3>
              <button type="button" className="icon-button" aria-label="Collapse node palette" onClick={() => setPaletteCollapsed(true)}>‹</button>
            </header>
            <div className="program-empty-guidance">
              <strong>Open a Draft to use the palette</strong>
              <span>Use Open in the persistent header.</span>
            </div>
          </section>
        )}

        {program ? (
          <ProgramTreeCanvas
            root={program.tree}
            catalogEntries={catalogEntries}
            environment={nodeEnvironment}
            selectedId={selectedNodeId}
            busy={authoringBusy}
            onSelect={setSelectedNodeId}
            onMutate={mutate}
            onStageInsert={(request) => {
              setSelectedNodeId(request.targetId);
              setInsertionRequest(request);
            }}
          />
        ) : (
          <section className="program-tree-surface program-tree-empty" aria-label="Program tree canvas">
            <strong>No Program Draft is open</strong>
            <span>Choose New or Open in the persistent header.</span>
          </section>
        )}

        {inspectorCollapsed ? (
          <button
            type="button"
            className="program-side-rail program-side-rail-right"
            aria-label="Expand Program inspector"
            onClick={() => setInspectorCollapsed(false)}
          >
            <span aria-hidden="true">‹</span><span>Inspector</span>
          </button>
        ) : (
          <aside className="program-inspector" aria-label="Program inspector">
            <div className="program-inspector-tabs">
              <button
                type="button"
                className={inspectorView === 'properties' ? 'active' : ''}
                aria-pressed={inspectorView === 'properties'}
                onClick={() => setInspectorView('properties')}
              >
                Properties
              </button>
              <button
                type="button"
                className={inspectorView === 'declarations' ? 'active' : ''}
                aria-pressed={inspectorView === 'declarations'}
                onClick={() => setInspectorView('declarations')}
              >
                Declarations
              </button>
              <button
                type="button"
                className="icon-button"
                aria-label="Collapse Program inspector"
                title="Collapse Program inspector"
                onClick={() => setInspectorCollapsed(true)}
              >
                ›
              </button>
            </div>
            <div className="program-inspector-scroll">
              {!program ? (
                <div className="program-empty-guidance">
                  <strong>Select a node to inspect properties</strong>
                  <span>Properties and declarations appear after a Draft is open.</span>
                </div>
              ) : inspectorView === 'properties' ? (
                <ProgramPropertyEditor
                  embedded
                  root={program.tree}
                  selectedId={selectedNodeId}
                  catalogEntries={catalogEntries}
                  programParameters={program.parameters ?? []}
                  blackboardKeys={program.blackboard?.keys ?? []}
                  busy={authoringBusy}
                  onMutate={mutate}
                  onUpdateNodePolicies={updateNodePolicies}
                />
              ) : (
                <ProgramDeclarationsEditor
                  embedded
                  root={program.tree}
                  parameters={program.parameters ?? []}
                  namespaces={program.blackboard?.namespaces ?? []}
                  keys={program.blackboard?.keys ?? []}
                  busy={authoringBusy}
                  onMutate={mutate}
                />
              )}
            </div>
          </aside>
        )}
      </div>
    </section>
  );
}
