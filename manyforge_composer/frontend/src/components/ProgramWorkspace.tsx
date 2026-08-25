import { useEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  CatalogResponse,
  NodeEnvironmentResponse,
} from '../types/api';
import { flattenProgramTree } from '../utils/programAuthoring';
import type { ProgramInsertionRequest } from '../utils/programCanvasInteraction';
import { PaneDivider } from './PaneDivider';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import {
  ProgramDeclarationsNavigator,
  type ProgramDeclarationSelection,
} from './ProgramDeclarationsNavigator';
import { ProgramNodePalette } from './ProgramNodePalette';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';

const PROGRAM_TOOL_MIN = 160;
const PROGRAM_TOOL_MAX = 360;
const PROGRAM_INSPECTOR_MIN = 240;
const PROGRAM_INSPECTOR_MAX = 480;
const PROGRAM_CANVAS_MIN = 216;
const PROGRAM_DIVIDER_WIDTH = 7;
const PROGRAM_COLLAPSED_RAIL_WIDTH = 38;

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

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
    admissionObserved,
    refresh,
    mutate,
    updateNodePolicies,
    clearFeedback,
  } = controller;
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [insertionRequest, setInsertionRequest] = useState<ProgramInsertionRequest | null>(null);
  const [paletteCollapsed, setPaletteCollapsed] = useState(false);
  const [inspectorCollapsed, setInspectorCollapsed] = useState(false);
  const [toolView, setToolView] = useState<'nodes' | 'params'>('nodes');
  const [declarationSelection, setDeclarationSelection] = useState<ProgramDeclarationSelection | null>(null);
  const [paletteWidth, setPaletteWidth] = useState(216);
  const [inspectorWidth, setInspectorWidth] = useState(288);
  const [authoringWidth, setAuthoringWidth] = useState(856);
  const authoringGrid = useRef<HTMLDivElement>(null);
  const paletteCollapseButton = useRef<HTMLButtonElement>(null);
  const paletteRail = useRef<HTMLButtonElement>(null);
  const inspectorCollapseButton = useRef<HTMLButtonElement>(null);
  const inspectorRail = useRef<HTMLButtonElement>(null);
  const previousPaletteCollapsed = useRef(false);
  const previousInspectorCollapsed = useRef(false);
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
    if (selectedNodeId && !validNodeIds.has(selectedNodeId)) {
      setSelectedNodeId(null);
    }
  }, [program, selectedNodeId, validNodeIds]);
  useEffect(() => {
    if (paletteCollapsed) paletteRail.current?.focus();
    else if (previousPaletteCollapsed.current) paletteCollapseButton.current?.focus();
    previousPaletteCollapsed.current = paletteCollapsed;
  }, [paletteCollapsed]);
  useEffect(() => {
    if (inspectorCollapsed) inspectorRail.current?.focus();
    else if (previousInspectorCollapsed.current) inspectorCollapseButton.current?.focus();
    previousInspectorCollapsed.current = inspectorCollapsed;
  }, [inspectorCollapsed]);
  useEffect(() => {
    const element = authoringGrid.current;
    if (element === null) return undefined;
    const update = () => setAuthoringWidth(element.clientWidth);
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const inspectorReservation = inspectorCollapsed
    ? PROGRAM_COLLAPSED_RAIL_WIDTH
    : inspectorWidth + PROGRAM_DIVIDER_WIDTH;
  const paletteReservation = paletteCollapsed
    ? PROGRAM_COLLAPSED_RAIL_WIDTH
    : paletteWidth + PROGRAM_DIVIDER_WIDTH;
  const paletteWidthMax = Math.max(
    PROGRAM_TOOL_MIN,
    Math.min(
      PROGRAM_TOOL_MAX,
      authoringWidth - inspectorReservation - PROGRAM_CANVAS_MIN - PROGRAM_DIVIDER_WIDTH,
    ),
  );
  const inspectorWidthMax = Math.max(
    PROGRAM_INSPECTOR_MIN,
    Math.min(
      PROGRAM_INSPECTOR_MAX,
      authoringWidth - paletteReservation - PROGRAM_CANVAS_MIN - PROGRAM_DIVIDER_WIDTH,
    ),
  );
  useEffect(() => {
    setPaletteWidth((current) => clamp(current, PROGRAM_TOOL_MIN, paletteWidthMax));
  }, [paletteWidthMax]);
  useEffect(() => {
    setInspectorWidth((current) => clamp(current, PROGRAM_INSPECTOR_MIN, inspectorWidthMax));
  }, [inspectorWidthMax]);

  const gridClassName = [
    'program-authoring-grid',
    program ? '' : 'program-authoring-empty',
    paletteCollapsed ? 'palette-collapsed' : '',
    inspectorCollapsed ? 'inspector-collapsed' : '',
  ].filter(Boolean).join(' ');
  const gridStyle = {
    '--program-tool-width': `${paletteWidth}px`,
    '--program-inspector-width': `${inspectorWidth}px`,
  } as CSSProperties;
  let inspectorTitle = '';
  if (toolView === 'params') inspectorTitle = 'Declaration';
  else if (selectedNodeId !== null) inspectorTitle = 'Properties';

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
        {!runtime.active && workspace !== null && admissionObserved && !mutationAvailable && (
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

      <div ref={authoringGrid} className={gridClassName} style={gridStyle}>
        {paletteCollapsed ? (
          <button
            ref={paletteRail}
            type="button"
            className="program-side-rail program-side-rail-left"
            aria-label="Expand node palette"
            onClick={() => setPaletteCollapsed(false)}
          >
            <span aria-hidden="true">›</span><span>{toolView === 'nodes' ? 'Nodes' : 'Params'}</span>
          </button>
        ) : program && toolView === 'nodes' ? (
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
            collapseButtonRef={paletteCollapseButton}
            onViewChange={setToolView}
          />
        ) : program ? (
          <section className="program-palette" aria-label="Program declaration navigation">
            <header className="program-subpanel-header">
              <div className="program-tool-tabs" aria-label="Program tools">
                <button type="button" aria-pressed="false" onClick={() => setToolView('nodes')}>Nodes</button>
                <button type="button" className="active" aria-pressed="true">Params</button>
              </div>
              <button
                ref={paletteCollapseButton}
                type="button"
                className="icon-button"
                aria-label="Collapse Program tools"
                onClick={() => setPaletteCollapsed(true)}
              >
                ‹
              </button>
            </header>
            <ProgramDeclarationsNavigator
              root={program.tree}
              parameters={program.parameters ?? []}
              namespaces={program.blackboard?.namespaces ?? []}
              keys={program.blackboard?.keys ?? []}
              selected={declarationSelection}
              onSelect={setDeclarationSelection}
            />
          </section>
        ) : (
          <section className="program-palette" aria-label="Node palette">
            <header className="program-subpanel-header">
              <div className="program-tool-tabs" aria-label="Program tools">
                <button type="button" className={toolView === 'nodes' ? 'active' : ''} aria-pressed={toolView === 'nodes'} onClick={() => setToolView('nodes')}>Nodes</button>
                <button type="button" className={toolView === 'params' ? 'active' : ''} aria-pressed={toolView === 'params'} onClick={() => setToolView('params')}>Params</button>
              </div>
              <button ref={paletteCollapseButton} type="button" className="icon-button" aria-label="Collapse node palette" onClick={() => setPaletteCollapsed(true)}>‹</button>
            </header>
            <div className="program-empty-guidance">
              <strong>Open a Draft to use the palette</strong>
              <span>Use Open in the persistent header.</span>
            </div>
          </section>
        )}

        {!paletteCollapsed && (
          <PaneDivider
            label="Program tools"
            className="program-pane-divider"
            value={paletteWidth}
            min={PROGRAM_TOOL_MIN}
            max={paletteWidthMax}
            onChange={setPaletteWidth}
          />
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

        {!inspectorCollapsed && (
          <PaneDivider
            label="Program inspector"
            className="program-pane-divider"
            value={inspectorWidth}
            min={PROGRAM_INSPECTOR_MIN}
            max={inspectorWidthMax}
            direction={-1}
            onChange={setInspectorWidth}
          />
        )}

        {inspectorCollapsed ? (
          <button
            ref={inspectorRail}
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
              <strong>{inspectorTitle}</strong>
              <button
                ref={inspectorCollapseButton}
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
              ) : toolView === 'nodes' && selectedNodeId === null ? (
                <div className="program-inspector-empty" aria-label="No tree node selected" />
              ) : toolView === 'nodes' ? (
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
                  selection={declarationSelection}
                  showNavigation={false}
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
