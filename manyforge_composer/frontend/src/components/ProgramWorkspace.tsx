import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  CatalogResponse,
  NodeEnvironmentResponse,
  SourceModelProjection,
} from '../types/api';
import {
  flattenProgramForest,
  programDiagnosticNodeId,
} from '../utils/programAuthoring';
import { PaneDivider } from './PaneDivider';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import {
  ProgramDeclarationsNavigator,
  type ProgramDeclarationSelection,
} from './ProgramDeclarationsNavigator';
import { ProgramNodePalette } from './ProgramNodePalette';
import type { ProgramNotice, ProgramNoticeInput } from './ProgramNoticeRail';
import {
  buildProgramParameterMutation,
  ProgramPropertyEditor,
} from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';

const PROGRAM_TOOL_MIN = 160;
const PROGRAM_TOOL_MAX = 360;
const PROGRAM_INSPECTOR_MIN = 240;
const PROGRAM_INSPECTOR_MAX = 480;
const PROGRAM_CANVAS_MIN = 216;
const PROGRAM_DIVIDER_WIDTH = 7;
const PROGRAM_COLLAPSED_RAIL_WIDTH = 38;
export const PROGRAM_INFO_NOTICE_TIMEOUT_MS = 4_000;

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

export function mergeProgramNotice(
  current: ProgramNotice[],
  notice: ProgramNotice,
  limit = 8,
): ProgramNotice[] {
  const existing = current.some((row) => row.key === notice.key);
  const next = existing
    ? current.map((row) => row.key === notice.key ? notice : row)
    : [...current, notice];
  return next.slice(-limit);
}

interface ProgramWorkspaceProps {
  controller: ProgramWorkspaceController;
  runtime: ProgramRuntimeController;
  catalog: StructuralResourceState<CatalogResponse>;
  environment: StructuralResourceState<NodeEnvironmentResponse>;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  onRetryCatalog: () => void;
  onRetryEnvironment: () => void;
  onInvalidEditChange?: (invalid: boolean) => void;
  onOpenScene?: () => void;
  onOpenProgramSettings?: () => void;
  programRequirementCreated?: {
    sequence: number;
    id: string;
    kind: 'component_role' | 'target_role';
  } | null;
}

export function ProgramWorkspace({
  controller,
  runtime,
  catalog,
  environment,
  sourceModel,
  onRetryCatalog,
  onRetryEnvironment,
  onInvalidEditChange,
  onOpenScene,
  onOpenProgramSettings,
  programRequirementCreated = null,
}: ProgramWorkspaceProps) {
  const {
    state,
    mutationAvailable,
    mutationUnavailableReason,
    refresh,
    mutate,
    updateNodePolicies,
    clearFeedback,
  } = controller;
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [paletteCollapsed, setPaletteCollapsed] = useState(false);
  const [inspectorCollapsed, setInspectorCollapsed] = useState(false);
  const [toolView, setToolView] = useState<'nodes' | 'params'>('nodes');
  const [declarationSelection, setDeclarationSelection] = useState<ProgramDeclarationSelection | null>(null);
  const [declarationPrefill, setDeclarationPrefill] = useState<{
    expectedType: string | null;
    expectedAccess: 'read' | 'write' | 'read_write' | null;
  } | null>(null);
  const [pendingReference, setPendingReference] = useState<{
    nodeId: string;
    parameterId: string;
    semanticType: string;
    target: 'blackboard_key' | 'program_parameter' | 'program_requirements';
    createdId: string | null;
    applying: boolean;
  } | null>(null);
  const [invalidLocalEdit, setInvalidLocalEdit] = useState(false);
  const [paletteWidth, setPaletteWidth] = useState(216);
  const [inspectorWidth, setInspectorWidth] = useState(288);
  const [authoringWidth, setAuthoringWidth] = useState(856);
  const [notices, setNotices] = useState<ProgramNotice[]>([]);
  const noticeSequence = useRef(0);
  const noticeIdsByKey = useRef(new Map<string, string>());
  const noticeTimeouts = useRef(new Map<string, number>());
  const observedRequirementSequence = useRef(0);
  const authoringGrid = useRef<HTMLDivElement>(null);
  const paletteCollapseButton = useRef<HTMLButtonElement>(null);
  const paletteRail = useRef<HTMLButtonElement>(null);
  const inspectorCollapseButton = useRef<HTMLButtonElement>(null);
  const inspectorRail = useRef<HTMLButtonElement>(null);
  const previousPaletteCollapsed = useRef(false);
  const previousInspectorCollapsed = useRef(false);
  const dismissNotice = useCallback((id: string) => {
    const timeout = noticeTimeouts.current.get(id);
    if (timeout !== undefined) window.clearTimeout(timeout);
    noticeTimeouts.current.delete(id);
    setNotices((current) => {
      const removed = current.find((notice) => notice.id === id);
      if (removed) noticeIdsByKey.current.delete(removed.key);
      return current.filter((notice) => notice.id !== id);
    });
  }, []);
  const reportNotice = useCallback((input: ProgramNoticeInput) => {
    const kind = input.kind ?? 'info';
    const noticeId = noticeIdsByKey.current.get(input.key)
      ?? `program-notice-${++noticeSequence.current}`;
    noticeIdsByKey.current.set(input.key, noticeId);
    setNotices((current) => {
      const notice: ProgramNotice = {
        id: noticeId,
        key: input.key,
        kind,
        message: input.message,
        dismissible: input.dismissible ?? true,
      };
      const bounded = mergeProgramNotice(current, notice);
      const retainedIds = new Set(bounded.map((row) => row.id));
      for (const [key, id] of noticeIdsByKey.current) {
        if (!retainedIds.has(id)) noticeIdsByKey.current.delete(key);
      }
      return bounded;
    });
    const previousTimeout = noticeTimeouts.current.get(noticeId);
    if (previousTimeout !== undefined) window.clearTimeout(previousTimeout);
    if (kind === 'info') {
      const timeout = window.setTimeout(
        () => dismissNotice(noticeId),
        PROGRAM_INFO_NOTICE_TIMEOUT_MS,
      );
      noticeTimeouts.current.set(noticeId, timeout);
    }
  }, [dismissNotice]);
  const dismissAllNotices = useCallback(() => {
    for (const timeout of noticeTimeouts.current.values()) window.clearTimeout(timeout);
    noticeTimeouts.current.clear();
    setNotices((current) => {
      const retained = current.filter((notice) => !notice.dismissible);
      noticeIdsByKey.current = new Map(retained.map((notice) => [notice.key, notice.id]));
      return retained;
    });
    clearFeedback();
  }, [clearFeedback]);
  useEffect(() => () => {
    for (const timeout of noticeTimeouts.current.values()) window.clearTimeout(timeout);
    noticeTimeouts.current.clear();
  }, []);
  const workspace = state.workspace;
  const authoring = workspace?.draft.authoring ?? null;
  const diagnostics = useMemo(
    () => workspace?.draft.diagnostics ?? [],
    [workspace?.draft.diagnostics],
  );
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
  const selectNode = useCallback((instanceId: string | null) => {
    if (invalidLocalEdit && instanceId !== selectedNodeId) return;
    setSelectedNodeId(instanceId);
  }, [invalidLocalEdit, selectedNodeId]);
  const changeToolView = useCallback((view: 'nodes' | 'params') => {
    if (invalidLocalEdit && view !== toolView) return;
    setToolView(view);
  }, [invalidLocalEdit, toolView]);
  const selectDeclaration = useCallback((selection: ProgramDeclarationSelection) => {
    if (
      invalidLocalEdit
      && (selection.kind !== declarationSelection?.kind || selection.id !== declarationSelection.id)
    ) return;
    setDeclarationSelection(selection);
  }, [declarationSelection, invalidLocalEdit]);
  useEffect(() => {
    onInvalidEditChange?.(invalidLocalEdit);
    return () => onInvalidEditChange?.(false);
  }, [invalidLocalEdit, onInvalidEditChange]);
  useEffect(() => {
    const message = state.error ?? state.message;
    if (!message || workspace === null) return;
    reportNotice({
      key: state.error ? `workspace-error:${message}` : `workspace-message:${message}`,
      message,
      kind: state.error ? 'error' : 'info',
    });
  }, [reportNotice, state.error, state.message, workspace]);

  const validNodeIds = useMemo(
    () => new Set(authoring ? flattenProgramForest(authoring.forest)
      .map(({ node }) => node.instance_id) : []),
    [authoring],
  );
  const invalidNodeIds = useMemo(() => new Set(
    authoring === null
      ? []
      : diagnostics.map((diagnostic) =>
        programDiagnosticNodeId(authoring.forest, diagnostic.path))
        .filter((instanceId): instanceId is string => instanceId !== null),
  ), [authoring, diagnostics]);
  useEffect(() => {
    if (!authoring) {
      setSelectedNodeId(null);
      return;
    }
    if (selectedNodeId && !validNodeIds.has(selectedNodeId)) {
      setSelectedNodeId(null);
    }
  }, [authoring, selectedNodeId, validNodeIds]);
  useEffect(() => {
    if (!authoring || declarationSelection === null) return;
    const exists = declarationSelection.kind === 'parameter'
      ? (authoring.parameters ?? []).some(
        (row) => row.parameter_id === declarationSelection.id,
      )
      : declarationSelection.kind === 'namespace'
        ? (authoring.blackboard?.namespaces ?? []).some(
          (row) => row.namespace_id === declarationSelection.id,
        )
        : (authoring.blackboard?.keys ?? []).some(
          (row) => row.key === declarationSelection.id,
        );
    if (declarationSelection.id && !exists) setDeclarationSelection(null);
  }, [authoring, declarationSelection]);
  useEffect(() => {
    if (
      programRequirementCreated === null
      || programRequirementCreated.sequence === observedRequirementSequence.current
    ) return;
    observedRequirementSequence.current = programRequirementCreated.sequence;
    setPendingReference((current) => current?.target === 'program_requirements'
      && current.semanticType === programRequirementCreated.kind
      ? { ...current, createdId: programRequirementCreated.id, applying: false }
      : current);
  }, [programRequirementCreated]);
  useEffect(() => {
    if (
      pendingReference === null
      || pendingReference.createdId === null
      || pendingReference.applying
      || authoring === null
      || !mutationAvailable
    ) return;
    const node = flattenProgramForest(authoring.forest).find(
      (row) => row.node.instance_id === pendingReference.nodeId,
    )?.node;
    const declarationExists = pendingReference.target === 'blackboard_key'
      ? (authoring.blackboard?.keys ?? []).some((row) => row.key === pendingReference.createdId)
      : pendingReference.target === 'program_parameter'
        ? (authoring.parameters ?? []).some(
          (row) => row.parameter_id === pendingReference.createdId,
        )
        : pendingReference.semanticType === 'component_role'
          ? authoring.component_roles[pendingReference.createdId] !== undefined
          : authoring.target_roles[pendingReference.createdId] !== undefined;
    if (!node || !declarationExists) return;
    const request = buildProgramParameterMutation(node, pendingReference.parameterId, {
      kind: 'literal',
      value: pendingReference.createdId,
    });
    setPendingReference((current) => current === null ? null : { ...current, applying: true });
    void mutate(request.mutation, request.args).then((response) => {
      if (response !== null) {
        setToolView('nodes');
        setSelectedNodeId(pendingReference.nodeId);
      }
      setPendingReference(null);
      setDeclarationPrefill(null);
    });
  }, [authoring, mutate, mutationAvailable, pendingReference]);
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
    authoring ? '' : 'program-authoring-empty',
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
        {runtime.active && (
          <div className="program-workspace-notice" role="status">
            <strong>Run active.</strong>
            <span>Draft inspection remains available; backend-owned authoring actions stay disabled until Idle.</span>
          </div>
        )}
        {!runtime.active && workspace !== null && !mutationAvailable && (
          <div className="program-workspace-notice" role="status">
            <strong>Authoring unavailable.</strong>
            <span>{mutationUnavailableReason}</span>
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
        ) : authoring && toolView === 'nodes' ? (
          <ProgramNodePalette
            roots={authoring.forest}
            catalogEntries={catalogEntries}
            catalogIdentity={catalogIdentity}
            environment={nodeEnvironment}
            busy={authoringBusy}
            onMutate={mutate}
            onCollapse={() => setPaletteCollapsed(true)}
            collapseButtonRef={paletteCollapseButton}
            onViewChange={changeToolView}
          />
        ) : authoring ? (
          <section className="program-palette" aria-label="Program declaration navigation">
            <header className="program-subpanel-header">
              <div className="program-tool-tabs" aria-label="Program tools">
                <button type="button" aria-pressed="false" onClick={() => changeToolView('nodes')}>Nodes</button>
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
              roots={authoring.forest}
              parameters={authoring.parameters ?? []}
              namespaces={authoring.blackboard?.namespaces ?? []}
              keys={authoring.blackboard?.keys ?? []}
              selected={declarationSelection}
              onSelect={selectDeclaration}
            />
          </section>
        ) : (
          <section className="program-palette" aria-label="Node palette">
            <header className="program-subpanel-header">
              <div className="program-tool-tabs" aria-label="Program tools">
                <button type="button" className={toolView === 'nodes' ? 'active' : ''} aria-pressed={toolView === 'nodes'} onClick={() => changeToolView('nodes')}>Nodes</button>
                <button type="button" className={toolView === 'params' ? 'active' : ''} aria-pressed={toolView === 'params'} onClick={() => changeToolView('params')}>Params</button>
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

        {authoring ? (
          <ProgramTreeCanvas
            key={workspace?.draft.draftId}
            roots={authoring.forest}
            retainedPositions={authoring.positions}
            catalogEntries={catalogEntries}
            environment={nodeEnvironment}
            selectedId={selectedNodeId}
            invalidNodeIds={invalidNodeIds}
            busy={authoringBusy}
            onSelect={selectNode}
            onMutate={mutate}
            notices={notices}
            onNotice={reportNotice}
            onDismissNotice={(id) => {
              const ownerFeedback = notices.find((notice) => notice.id === id)
                ?.key.startsWith('workspace-') === true;
              dismissNotice(id);
              if (ownerFeedback) clearFeedback();
            }}
            onDismissAllNotices={dismissAllNotices}
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
                disabled={invalidLocalEdit}
                onClick={() => setInspectorCollapsed(true)}
              >
                ›
              </button>
            </div>
            <div className="program-inspector-scroll">
              {!authoring ? (
                <div className="program-empty-guidance">
                  <strong>Select a node to inspect properties</strong>
                  <span>Properties and declarations appear after a Draft is open.</span>
                </div>
              ) : toolView === 'nodes' && selectedNodeId === null ? (
                <div className="program-inspector-empty" aria-label="No tree node selected" />
              ) : toolView === 'nodes' ? (
                <ProgramPropertyEditor
                  embedded
                  roots={authoring.forest}
                  selectedId={selectedNodeId}
                  catalogEntries={catalogEntries}
                  program={authoring}
                  appliedProgram={runtime.state.applied}
                  sourceModel={sourceModel.status === 'ready' ? sourceModel.data : null}
                  sourceModelUnavailableReason={sourceModel.status === 'loading'
                    ? 'Reading commandable joints from the active source model.'
                    : sourceModel.status === 'ready'
                      ? null
                      : sourceModel.message}
                  busy={authoringBusy}
                  onMutate={mutate}
                  onCreateReference={(target, expectedType, expectedAccess, source) => {
                    if (target === 'scene_resource') {
                      onOpenScene?.();
                    } else if (target === 'program_requirements') {
                      setPendingReference({
                        ...source,
                        target,
                        createdId: null,
                        applying: false,
                      });
                      onOpenProgramSettings?.();
                    } else {
                      setPendingReference({
                        ...source,
                        target,
                        createdId: null,
                        applying: false,
                      });
                      setDeclarationPrefill({ expectedType, expectedAccess });
                      setToolView('params');
                      setDeclarationSelection({
                        kind: target === 'blackboard_key' ? 'key' : 'parameter',
                        id: '',
                      });
                    }
                  }}
                  onRenamed={(previousId, renamedId) => {
                    if (selectedNodeId === previousId) setSelectedNodeId(renamedId);
                  }}
                  onUpdateNodePolicies={updateNodePolicies}
                  onInvalidEditChange={setInvalidLocalEdit}
                  diagnostics={diagnostics}
                />
              ) : (
                <ProgramDeclarationsEditor
                  embedded
                  selection={declarationSelection}
                  showNavigation={false}
                  roots={authoring.forest}
                  parameters={authoring.parameters ?? []}
                  namespaces={authoring.blackboard?.namespaces ?? []}
                  keys={authoring.blackboard?.keys ?? []}
                  catalogEntries={catalogEntries}
                  creationExpectedType={declarationPrefill?.expectedType ?? null}
                  creationExpectedAccess={declarationPrefill?.expectedAccess ?? null}
                  busy={authoringBusy}
                  onMutate={mutate}
                  onSelectNode={selectNode}
                  onCreated={(selection) => {
                    setPendingReference((current) => current === null ? null : {
                      ...current,
                      createdId: selection.id,
                      applying: false,
                    });
                  }}
                  onInvalidEditChange={setInvalidLocalEdit}
                  diagnostics={diagnostics}
                />
              )}
            </div>
          </aside>
        )}
      </div>
    </section>
  );
}
