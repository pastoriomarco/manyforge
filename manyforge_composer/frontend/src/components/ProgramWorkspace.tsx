import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties } from 'react';
import { createPortal } from 'react-dom';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  CatalogResponse,
  NodeEnvironmentResponse,
  DeploymentWorkspaceResponse,
  JsonValue,
  SourceModelProjection,
} from '../types/api';
import type { ManualControlMotionDescriptor } from '../types/robotControl';
import { globalRuleIds, interruptConditionKeyUses, type TargetStopChain } from '../utils/interruptAuthoring';
import type { CarriedObjectLostSignalChoice } from '../utils/systemSignalAuthoring';
import {
  deriveProgramMotionConfiguration,
  flattenProgramForest,
  programDiagnosticNodeId,
  programMotionDefaultRoles,
  type ProgramMotionDefaultRole,
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
import { ProgramSettingsEditor } from './ProgramSettingsEditor';
import { TreeEditorPreferences } from './TreeEditorPreferences';
import { onOpenSettingsRequest } from '../state/settingsNavigation';
import { useCardPanelLayout } from '../hooks/useCardPanelLayout';
import { useComposerSettings } from '../hooks/useComposerSettings';
import { PROGRAM_TOOLS_WIDTH_PX } from '../state/composerSettings';

const PROGRAM_TOOL_MIN = PROGRAM_TOOLS_WIDTH_PX.min;
const PROGRAM_TOOL_MAX = PROGRAM_TOOLS_WIDTH_PX.max;
const PROGRAM_CANVAS_MIN = 216;
const PROGRAM_DIVIDER_WIDTH = 7;
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
  /** The advisory preview of a held failed node's Draft edits: one status
   * line in that node's Properties editor (the viewer shows its ghost). */
  heldNodePreview?: { nodeInstanceId: string | null; statusText: string | null } | null;
  catalog: StructuralResourceState<CatalogResponse>;
  environment: StructuralResourceState<NodeEnvironmentResponse>;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  deploymentWorkspace?: DeploymentWorkspaceResponse | null;
  systemSignalChoices?: readonly CarriedObjectLostSignalChoice[];
  systemSignalsNeedApply?: boolean;
  /** The scenario's Program role assignment (authoring workspace project facts). */
  programRoleAssignment?: Record<string, JsonValue> | null;
  motionDescriptors?: ManualControlMotionDescriptor[];
  /** Published motion targets' qualified Stop chains (SX-2167). */
  stopChains?: TargetStopChain[];
  onRetryCatalog: () => void;
  onRetryEnvironment: () => void;
  onInvalidEditChange?: (invalid: boolean) => void;
  onOpenScene?: () => void;
  inspectorTarget?: HTMLElement | null;
  inspectorMinimized?: boolean;
  onInspectorMinimizedChange?: (minimized: boolean) => void;
  onInspectorMaximize?: () => void;
  onInspectorStateChange?: (state: {
    active: boolean;
    title: string;
    readOnlyReason: string | null;
  }) => void;
  programRequirementCreated?: {
    sequence: number;
    id: string;
    kind: 'component_role' | 'target_role';
  } | null;
  onProgramRequirementCreated?: (kind: 'component_role' | 'target_role', id: string) => void;
}

export function ProgramWorkspace({
  controller,
  runtime,
  heldNodePreview = null,
  catalog,
  environment,
  sourceModel,
  deploymentWorkspace = null,
  systemSignalChoices = [],
  systemSignalsNeedApply = false,
  programRoleAssignment = null,
  motionDescriptors = [],
  stopChains,
  onRetryCatalog,
  onRetryEnvironment,
  onInvalidEditChange,
  onOpenScene,
  inspectorTarget = null,
  inspectorMinimized = false,
  onInspectorMinimizedChange,
  onInspectorMaximize,
  onInspectorStateChange,
  programRequirementCreated = null,
  onProgramRequirementCreated,
}: ProgramWorkspaceProps) {
  const {
    state,
    mutationAvailable,
    mutationUnavailableReason,
    refresh,
    mutate,
    updateNodePolicies,
    updateNodePresentation,
  } = controller;
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const deploymentDraftDocument = deploymentWorkspace?.draft?.document ?? null;
  const draftGlobalRuleIds = useMemo(
    () => globalRuleIds(deploymentDraftDocument as Record<string, JsonValue> | null),
    [deploymentDraftDocument],
  );
  // Program settings show in the Program inspector, opened from the tree
  // editor gear or the Composer settings; closing returns to the properties.
  const [settingsOpen, setSettingsOpen] = useState(false);
  const focusGroupsOnOpen = useRef(false);
  const [settingsInvalid, setSettingsInvalid] = useState(false);
  useEffect(() => onOpenSettingsRequest('program', () => setSettingsOpen(true)), []);
  // Selecting another node shows its properties (unless a setting is invalid).
  const settingsSelection = useRef(selectedNodeId);
  useEffect(() => {
    if (settingsSelection.current === selectedNodeId) return;
    settingsSelection.current = selectedNodeId;
    if (selectedNodeId !== null && !settingsInvalid) setSettingsOpen(false);
  }, [selectedNodeId, settingsInvalid]);
  const [paletteCollapsed, setPaletteCollapsed] = useState(false);
  const collapsePalette = useCallback(() => setPaletteCollapsed(true), []);
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
  // The pane opens at the installation default; dragging the divider
  // changes only this session. Moving the settings slider previews it live.
  const defaultToolsWidth = useComposerSettings().settings.treeEditor.toolsWidthPx;
  const [paletteWidth, setPaletteWidth] = useState(defaultToolsWidth);
  const [authoringWidth, setAuthoringWidth] = useState(856);
  const [notices, setNotices] = useState<ProgramNotice[]>([]);
  const noticeSequence = useRef(0);
  const noticeIdsByKey = useRef(new Map<string, string>());
  const noticeTimeouts = useRef(new Map<string, number>());
  const observedRequirementSequence = useRef(0);
  const authoringGrid = useRef<HTMLDivElement>(null);
  const settingsPanel = useCardPanelLayout();
  useEffect(() => {
    if (!settingsOpen || !focusGroupsOnOpen.current) return;
    focusGroupsOnOpen.current = false;
    document.getElementById('program-settings-execution-groups')?.focus();
  }, [settingsOpen]);
  const paletteCollapseButton = useRef<HTMLButtonElement>(null);
  const paletteRail = useRef<HTMLButtonElement>(null);
  const previousPaletteCollapsed = useRef(false);
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
  }, []);
  useEffect(() => () => {
    for (const timeout of noticeTimeouts.current.values()) window.clearTimeout(timeout);
    noticeTimeouts.current.clear();
  }, []);
  const workspace = state.workspace;
  const authoring = workspace?.draft.authoring ?? null;
  // Contract 87: keys read by node or global interrupt rules must declare `initial:`.
  const interruptKeyUses = useMemo(() => interruptConditionKeyUses(
    authoring?.forest ?? [], deploymentDraftDocument as Record<string, JsonValue> | null,
  ), [authoring?.forest, deploymentDraftDocument]);
  const diagnostics = useMemo(
    () => workspace?.draft.diagnostics ?? [],
    [workspace?.draft.diagnostics],
  );
  const busy = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const catalogEntries = useMemo(
    () => catalog.status === 'ready' ? catalog.data.entries : [],
    [catalog],
  );
  const motionDefaults = useMemo(
    () => authoring === null
      ? new Map<string, ProgramMotionDefaultRole>()
      : programMotionDefaultRoles(deriveProgramMotionConfiguration(authoring, catalogEntries)),
    [authoring, catalogEntries],
  );
  const catalogIdentity = useMemo(() => catalog.status === 'ready' ? {
    catalogId: catalog.data.catalogId,
    contentVersion: catalog.data.contentVersion,
    catalogDigest: catalog.data.catalogDigest,
    environmentDigest: catalog.data.environmentDigest,
  } : null, [catalog]);
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
    if (instanceId !== null) onInspectorMinimizedChange?.(false);
  }, [invalidLocalEdit, onInspectorMinimizedChange, selectedNodeId]);
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
    onInspectorMinimizedChange?.(false);
  }, [declarationSelection, invalidLocalEdit, onInspectorMinimizedChange]);
  useEffect(() => {
    onInvalidEditChange?.(invalidLocalEdit);
    return () => onInvalidEditChange?.(false);
  }, [invalidLocalEdit, onInvalidEditChange]);

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
        onInspectorMinimizedChange?.(false);
      }
      setPendingReference(null);
      setDeclarationPrefill(null);
    });
  }, [authoring, mutate, mutationAvailable, onInspectorMinimizedChange, pendingReference]);
  useEffect(() => {
    if (paletteCollapsed) paletteRail.current?.focus();
    else if (previousPaletteCollapsed.current) paletteCollapseButton.current?.focus();
    previousPaletteCollapsed.current = paletteCollapsed;
  }, [paletteCollapsed]);
  useEffect(() => {
    const element = authoringGrid.current;
    if (element === null) return undefined;
    const update = () => setAuthoringWidth(element.clientWidth);
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const paletteWidthMax = Math.max(
    PROGRAM_TOOL_MIN,
    Math.min(
      PROGRAM_TOOL_MAX,
      authoringWidth - PROGRAM_CANVAS_MIN - PROGRAM_DIVIDER_WIDTH,
    ),
  );
  // The requested width is kept as set; the pane shows it within the space
  // the tree canvas leaves free.
  useEffect(() => {
    setPaletteWidth(defaultToolsWidth);
  }, [defaultToolsWidth]);
  const shownPaletteWidth = clamp(paletteWidth, PROGRAM_TOOL_MIN, paletteWidthMax);

  // Workspace command results and failures belong to the workbench
  // notification rows. Authoring availability is a status of this editor, so
  // it stays beside the canvas as a fixed, non-dismissible line.
  const canvasNotices = useMemo(() => {
    const statusNotices: ProgramNotice[] = [];
    if (runtime.active) {
      statusNotices.push({
        id: 'program-runtime-active',
        key: 'program-runtime-active',
        kind: 'info',
        message: mutationAvailable
          ? 'Run held with all motion stopped. The Draft is editable; Apply Resets the run first, and Retry, Resume and Execute wait until the edits are Applied or Discarded.'
          : 'Run active. Draft inspection remains available; editing waits until all Program motion is stopped or the cycle is Idle.',
        dismissible: false,
      });
    } else if (workspace !== null && !mutationAvailable) {
      statusNotices.push({
        id: 'program-authoring-unavailable',
        key: 'program-authoring-unavailable',
        kind: 'info',
        message: `Authoring unavailable. ${mutationUnavailableReason}`,
        dismissible: false,
      });
    }
    return [...statusNotices, ...notices];
  }, [mutationAvailable, mutationUnavailableReason, notices, runtime.active, workspace]);

  const gridClassName = [
    'program-authoring-grid',
    authoring ? '' : 'program-authoring-empty',
    paletteCollapsed ? 'palette-collapsed' : '',
  ].filter(Boolean).join(' ');
  const gridStyle = {
    '--program-tool-width': `${shownPaletteWidth}px`,
  } as CSSProperties;
  let inspectorTitle = '';
  if (settingsOpen) inspectorTitle = 'Program settings';
  else if (toolView === 'params') inspectorTitle = 'Declaration';
  else if (selectedNodeId !== null) inspectorTitle = 'Properties';
  const inspectorActive = settingsOpen || (toolView === 'nodes'
    ? selectedNodeId !== null
    : declarationSelection !== null);
  const inspectorReadOnlyReason = authoringUnavailable
    ? 'Portable Program authoring is unavailable for the current environment.'
    : busy
      ? 'A Program operation is in progress.'
      : !mutationAvailable
        ? mutationUnavailableReason
        : null;
  useEffect(() => {
    onInspectorStateChange?.({
      active: inspectorActive,
      title: inspectorTitle || 'Program inspector',
      readOnlyReason: inspectorReadOnlyReason,
    });
    return () => onInspectorStateChange?.({
      active: false,
      title: 'Program inspector',
      readOnlyReason: null,
    });
  }, [inspectorActive, inspectorReadOnlyReason, inspectorTitle, onInspectorStateChange]);

  return (
    <section className="panel program-workspace" aria-label="Program authoring workspace">
      <div className="program-workspace-notices">
        {state.status === 'loading' && workspace === null && (
          <div className="program-workspace-notice notice severity-info notice-floating" role="status">
            Reading the Program workspace…
          </div>
        )}
        {state.status === 'error' && workspace === null && (
          <div className="program-workspace-notice error-state notice severity-error notice-floating" role="alert">
            <span><strong>Program workspace unavailable.</strong> {state.error}</span>
            <button type="button" onClick={refresh}>Retry workspace</button>
          </div>
        )}
        {catalog.status === 'loading' && (
          <div className="program-workspace-notice notice severity-info notice-floating" role="status">
            Reading the node catalog…
          </div>
        )}
        {(catalog.status === 'error' || catalog.status === 'absent') && (
          <div className="program-workspace-notice error-state notice severity-error notice-floating" role="alert">
            <span><strong>Node catalog unavailable.</strong> {catalog.message}</span>
            <button type="button" onClick={onRetryCatalog}>Retry catalog</button>
          </div>
        )}
        {environment.status === 'loading' && (
          <div className="program-workspace-notice notice severity-info notice-floating" role="status">
            Reading authoring availability…
          </div>
        )}
        {(environment.status === 'error' || environment.status === 'absent') && (
          <div className="program-workspace-notice error-state notice severity-error notice-floating" role="alert">
            <span><strong>Authoring availability unavailable.</strong> {environment.message}</span>
            <button type="button" onClick={onRetryEnvironment}>Retry environment</button>
          </div>
        )}
        {catalog.status === 'ready' && environment.status === 'ready' && !environmentAligned && (
          <div className="program-workspace-notice error-state notice severity-error notice-floating" role="alert">
            <span><strong>Catalog identity mismatch.</strong> Editing remains disabled.</span>
            <button type="button" onClick={onRetryCatalog}>Retry catalog</button>
            <button type="button" onClick={onRetryEnvironment}>Retry environment</button>
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
            program={authoring}
            catalogEntries={catalogEntries}
            catalogIdentity={catalogIdentity}
            environment={nodeEnvironment}
            busy={authoringBusy}
            onMutate={mutate}
            onSelect={selectNode}
            onCollapse={collapsePalette}
            collapseButtonRef={paletteCollapseButton}
            onViewChange={changeToolView}
          />
        ) : authoring ? (
          <section className="program-palette" aria-label="Program declaration navigation">
            <header className="program-subpanel-header">
              <div className="program-tool-tabs dseg" aria-label="Program tools">
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
              formulaReferences={workspace!.draft.formulaReferences}
              selected={declarationSelection}
              onSelect={selectDeclaration}
              motionDefaults={motionDefaults}
              globalRuleIds={draftGlobalRuleIds}
            />
          </section>
        ) : (
          <section className="program-palette" aria-label="Node palette">
            <header className="program-subpanel-header">
              <div className="program-tool-tabs dseg" aria-label="Program tools">
                <button type="button" className={toolView === 'nodes' ? 'active' : ''} aria-pressed={toolView === 'nodes'} onClick={() => changeToolView('nodes')}>Nodes</button>
                <button type="button" className={toolView === 'params' ? 'active' : ''} aria-pressed={toolView === 'params'} onClick={() => changeToolView('params')}>Params</button>
              </div>
              <button ref={paletteCollapseButton} type="button" className="icon-button" aria-label="Collapse node palette" onClick={() => setPaletteCollapsed(true)}>‹</button>
            </header>
            <div className="program-empty-guidance">
              <strong>Open a Draft to use the palette</strong>
              <span>Use New or Load Scenario in the header.</span>
            </div>
          </section>
        )}

        {!paletteCollapsed && (
          <PaneDivider
            label="Program tools"
            className="program-pane-divider"
            value={shownPaletteWidth}
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
            nodeStatuses={runtime.cycleState === 'idle_after_reset'
              ? {}
              : runtime.state.run?.nodeStatuses ?? {}}
            busy={authoringBusy}
            onSelect={selectNode}
            onMutate={mutate}
            notices={canvasNotices}
            onNotice={reportNotice}
            onDismissNotice={dismissNotice}
            onDismissAllNotices={dismissAllNotices}
          />
        ) : (
          <section className="program-tree-surface program-tree-empty" aria-label="Program tree canvas">
            <strong>No Program Draft is open</strong>
            <span>Use New or Load Scenario in the header.</span>
          </section>
        )}

        {inspectorTarget !== null && createPortal(
          <aside
            className={`program-inspector central-inspector-section${
              inspectorActive ? ' active' : ''
            }${inspectorMinimized || !inspectorActive ? ' minimized' : ''}${
              invalidLocalEdit ? ' invalid' : ''
            }`}
            aria-label="Program inspector"
          >
            <header className="central-inspector-header">
              <strong>{inspectorTitle || 'Program inspector'}</strong>
              {inspectorReadOnlyReason !== null && inspectorActive && (
                <span className="central-inspector-state" title={inspectorReadOnlyReason}>
                  Read-only
                </span>
              )}
              {invalidLocalEdit && (
                <span className="central-inspector-warning" title="An invalid field edit needs attention">
                  Invalid edit
                </span>
              )}
              <div className="central-inspector-actions">
                {settingsOpen && (
                  <button
                    type="button"
                    aria-label="Close Program settings"
                    title={settingsInvalid
                      ? 'Fix or discard the invalid setting first.'
                      : 'Close Program settings'}
                    disabled={settingsInvalid}
                    onClick={() => setSettingsOpen(false)}
                  >×</button>
                )}
                <button
                  type="button"
                  aria-label="Maximize Program inspector"
                  title="Maximize Program inspector"
                  disabled={!inspectorActive}
                  onClick={onInspectorMaximize}
                >□</button>
                <button
                  type="button"
                  aria-label={inspectorMinimized ? 'Restore Program inspector' : 'Minimize Program inspector'}
                  title={inspectorMinimized ? 'Restore Program inspector' : 'Minimize Program inspector'}
                  disabled={!inspectorActive}
                  onClick={() => onInspectorMinimizedChange?.(!inspectorMinimized)}
                >
                  {inspectorMinimized ? '⌃' : '⌄'}
                </button>
              </div>
            </header>
            {inspectorActive && !inspectorMinimized && inspectorReadOnlyReason !== null && (
              <p className="central-inspector-notice notice severity-warning notice-strip" role="status">
                <span><strong>Editing is unavailable.</strong> {inspectorReadOnlyReason}</span>
              </p>
            )}
            {inspectorActive && !inspectorMinimized && (
              <div className="program-inspector-scroll central-inspector-scroll">
              {settingsOpen ? (
                <div ref={settingsPanel} className="program-settings-inspector dpanel">
                  {authoring ? (
                    <ProgramSettingsEditor
                      program={authoring}
                      catalogEntries={catalogEntries}
                      candidateRevision={workspace?.draft.programRevision ?? null}
                      busy={authoringBusy}
                      runtime={runtime}
                      onMutate={mutate}
                      onInvalidChange={setSettingsInvalid}
                      onRoleCreated={onProgramRequirementCreated}
                      diagnostics={workspace?.draft.diagnostics ?? []}
                    />
                  ) : (
                    <p className="hint">No Program is loaded.</p>
                  )}
                  <TreeEditorPreferences />
                </div>
              ) : !authoring ? (
                <div className="program-empty-guidance">
                  <strong>Select a node to inspect properties</strong>
                  <span>Properties and declarations appear after a Draft is open.</span>
                </div>
              ) : toolView === 'nodes' && selectedNodeId === null ? (
                <div className="program-inspector-empty" aria-label="No tree node selected" />
              ) : toolView === 'nodes' ? (
                <>
                {heldNodePreview?.statusText != null
                  && heldNodePreview.nodeInstanceId === selectedNodeId && (
                  <p className="held-node-preview hint" role="status" aria-label="Edited motion preview">
                    {heldNodePreview.statusText}
                  </p>
                )}
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
                  deploymentWorkspace={deploymentWorkspace}
                  systemSignalChoices={systemSignalChoices}
                  systemSignalsNeedApply={systemSignalsNeedApply}
                  programRoleAssignment={programRoleAssignment}
                  motionDescriptors={motionDescriptors}
                  stopChains={stopChains}
                  busy={authoringBusy}
                  onMutate={mutate}
                  onCreateGroup={() => {
                    focusGroupsOnOpen.current = true;
                    setSettingsOpen(true);
                  }}
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
                      setSettingsOpen(true);
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
                      onInspectorMinimizedChange?.(false);
                    }
                  }}
                  onRenamed={(previousId, renamedId) => {
                    if (selectedNodeId === previousId) setSelectedNodeId(renamedId);
                  }}
                  onUpdateNodePolicies={updateNodePolicies}
                  onUpdateNodePresentation={updateNodePresentation}
                  onInvalidEditChange={setInvalidLocalEdit}
                  diagnostics={diagnostics}
                />
                </>
              ) : (
                <ProgramDeclarationsEditor
                  embedded
                  selection={declarationSelection}
                  showNavigation={false}
                  roots={authoring.forest}
                  parameters={authoring.parameters ?? []}
                  namespaces={authoring.blackboard?.namespaces ?? []}
                  keys={authoring.blackboard?.keys ?? []}
                  formulaReferences={workspace!.draft.formulaReferences}
                  catalogEntries={catalogEntries}
                  motionDefaults={motionDefaults}
                  interruptKeyUses={interruptKeyUses}
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
                  frameProgram={authoring}
                />
              )}
              </div>
            )}
          </aside>
        , inspectorTarget)}
      </div>
    </section>
  );
}
