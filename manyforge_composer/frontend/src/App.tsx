import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties } from 'react';

import { ApiError, api } from './api/client';
import { AssistantOverlay } from './components/AssistantOverlay';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import { DeploymentDocumentControls } from './components/DeploymentDocumentControls';
import {
  HeadlessRuntimeSurfaces,
  attentionAlarmsNewestFirst,
} from './components/HeadlessRuntimeSurfaces';
import {
  RuntimeAttentionAutoOpen,
  shouldCloseAutomaticRuntimeAttention,
} from './components/RuntimeAttentionAutoOpen';
import { ProgramDocumentControls } from './components/ProgramDocumentControls';
import { OverlayPanel } from './components/OverlayPanel';
import { PaneDivider } from './components/PaneDivider';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { runtimeStatusPresentation } from './components/runtimeStatus';
import { SceneAuthoringWorkspace } from './components/SceneAuthoringWorkspace';
import {
  useHeadlessStateSource,
  useShellHeadlessState,
} from './hooks/useHeadlessState';
import { useAuthoringWorkspace } from './hooks/useAuthoringWorkspace';
import { useDeploymentWorkspace } from './hooks/useDeploymentWorkspace';
import { useProgramRuntime } from './hooks/useProgramRuntime';
import { useProgramWorkspace } from './hooks/useProgramWorkspace';
import { useRobotControl } from './hooks/useRobotControl';
import { nodeEnvironmentRefreshKey } from './state/nodeEnvironmentRefresh';
import { useStructuralResource } from './state/structuralResource';
import { deriveProgramMotionConfiguration } from './utils/programAuthoring';
import {
  useOperationalErrorMirror,
  useOperationalErrors,
} from './state/operationalErrors';
import {
  INSPECTOR_WIDTH_MIN,
  WORKBENCH_DIVIDER_WIDTH,
  workbenchWidthLimits,
} from './state/workbenchLayout';

function isNotFound(error: unknown): boolean {
  return error instanceof ApiError && error.status === 404;
}

const INSPECTOR_SECTION_MIN_HEIGHT = 140;

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

export default function App() {
  const [activeWorkspace, setActiveWorkspace] = useState<'program' | 'scene' | 'deployment'>('program');
  const [viewerWidth, setViewerWidth] = useState(() => (
    typeof window === 'undefined' ? 640 : Math.round(window.innerWidth * 0.45)
  ));
  const [inspectorWidth, setInspectorWidth] = useState(320);
  const [workbenchWidth, setWorkbenchWidth] = useState(() => (
    typeof window === 'undefined' ? 1440 : window.innerWidth
  ));
  const [sceneEditorTarget, setSceneEditorTarget] = useState<HTMLElement | null>(null);
  const [programInspectorTarget, setProgramInspectorTarget] = useState<HTMLElement | null>(null);
  const [viewerInspectorTarget, setViewerInspectorTarget] = useState<HTMLElement | null>(null);
  const [programInspectorMinimized, setProgramInspectorMinimized] = useState(false);
  const [viewerInspectorMinimized, setViewerInspectorMinimized] = useState(false);
  const [programInspectorState, setProgramInspectorState] = useState<{
    active: boolean;
    title: string;
    readOnlyReason: string | null;
  }>({ active: false, title: 'Program inspector', readOnlyReason: null });
  const [viewerInspectorPanel, setViewerInspectorPanel] = useState<
    'tools' | 'robot' | 'object' | null
  >(null);
  const [inspectorHeight, setInspectorHeight] = useState(() => (
    typeof window === 'undefined' ? 720 : Math.max(320, window.innerHeight - 52)
  ));
  const [programInspectorHeight, setProgramInspectorHeight] = useState(() => (
    typeof window === 'undefined' ? 360 : Math.max(160, Math.floor((window.innerHeight - 52) / 2))
  ));
  const [globalPanel, setGlobalPanel] = useState<'runtime' | 'assistant' | null>(null);
  const [runtimePanelExpanded, setRuntimePanelExpanded] = useState(false);
  const [runtimeAttentionRequest, setRuntimeAttentionRequest] = useState<string | null>(null);
  const [runtimePanelAutomatic, setRuntimePanelAutomatic] = useState(false);
  const [programInvalidEdit, setProgramInvalidEdit] = useState(false);
  const [programSettingsRequest, setProgramSettingsRequest] = useState(0);
  const [componentOverviewRequest, setComponentOverviewRequest] = useState(0);
  const [programRequirementCreated, setProgramRequirementCreated] = useState<{
    sequence: number;
    id: string;
    kind: 'component_role' | 'target_role';
  } | null>(null);
  const workbench = useRef<HTMLDivElement>(null);
  const centralInspector = useRef<HTMLElement>(null);
  const bothInspectorSectionsExpanded = programInspectorState.active
    && !programInspectorMinimized
    && viewerInspectorPanel !== null
    && !viewerInspectorMinimized;
  const previousBothInspectorSectionsExpanded = useRef(false);
  const runtimeButton = useRef<HTMLButtonElement>(null);
  const assistantButton = useRef<HTMLButtonElement>(null);
  const [health, retryHealth] = useStructuralResource(api.health.get, {
    failureMessage: 'Could not read the Composer structural boundary.',
  });
  const [structuralMode, setStructuralMode] = useState<
    'headless_client' | 'structural_only' | null
  >(null);
  const [assistantConfigured, setAssistantConfigured] = useState(false);
  const structuralRetryAttempt = useRef(0);
  const headlessEnabled = structuralMode === 'headless_client';
  const [catalog, retryCatalog] = useStructuralResource(api.catalog.list, {
    failureMessage: 'Could not read the immutable v2 node catalog.',
  });
  const [manifest, retryManifest] = useStructuralResource(api.urdf.manifest, {
    absentWhen: isNotFound,
    absentMessage: 'No complete URDF is available for the active retained deployment.',
    failureMessage: 'Could not read the retained-neutral URDF manifest.',
  });
  const [nodeEnvironment, retryNodeEnvironment] = useStructuralResource(
    api.nodes.environment,
    {
      enabled: headlessEnabled,
      disabledMessage: 'Node-environment facts require a connected headless owner.',
      failureMessage: 'Could not read the active v2 node environment.',
    },
  );
  const nodeEnvironmentWakeKey = useRef<string | null>(null);
  useEffect(() => {
    if (health.status === 'ready') {
      structuralRetryAttempt.current = 0;
      setStructuralMode(health.data.mode);
      setAssistantConfigured(
        health.data.components.assistant_bridge === 'configured',
      );
      return undefined;
    }
    if (health.status === 'loading' || structuralMode !== null) return undefined;
    const attempt = structuralRetryAttempt.current;
    structuralRetryAttempt.current += 1;
    const timer = window.setTimeout(
      retryHealth,
      Math.min(1_000 * 2 ** attempt, 8_000),
    );
    return () => window.clearTimeout(timer);
  }, [health, retryHealth, structuralMode]);
  const [sourceModel, retrySourceModel] = useStructuralResource(api.sourceModel.get, {
    enabled: headlessEnabled,
    disabledMessage: 'Source-model facts require a connected headless owner.',
    failureMessage: 'Could not read the verified active source-model projection.',
  });
  const assistantEnabled = headlessEnabled && assistantConfigured;
  const headlessStateSource = useHeadlessStateSource(headlessEnabled);
  const headlessState = useShellHeadlessState(headlessStateSource);
  const headlessSnapshot = headlessState.snapshot;
  // One shared authoring publication for the whole workbench: Program,
  // Deployment and Scenario authoring all read this single snapshot, so no
  // consumer establishes an independent authoring identity.
  const authoringWorkspace = useAuthoringWorkspace(headlessSnapshot);
  const deploymentController = useDeploymentWorkspace(
    headlessEnabled,
    headlessSnapshot,
    authoringWorkspace,
  );
  useEffect(() => {
    if (!headlessEnabled) {
      nodeEnvironmentWakeKey.current = null;
      return;
    }
    if (headlessSnapshot === null) return;
    const key = nodeEnvironmentRefreshKey(headlessSnapshot);
    if (nodeEnvironmentWakeKey.current === null) {
      nodeEnvironmentWakeKey.current = key;
      return;
    }
    if (nodeEnvironmentWakeKey.current === key) return;
    nodeEnvironmentWakeKey.current = key;
    retryNodeEnvironment();
  }, [headlessEnabled, headlessSnapshot, retryNodeEnvironment]);
  const programController = useProgramWorkspace(
    headlessSnapshot,
    authoringWorkspace,
  );
  const programRuntime = useProgramRuntime(
    headlessEnabled,
    headlessSnapshot,
    programController.state.workspace,
    headlessStateSource,
  );
  const programCartesianDefaults = useMemo(() => {
    const authoring = programController.state.workspace?.draft.authoring ?? null;
    if (authoring === null || catalog.status !== 'ready') return null;
    const configuration = deriveProgramMotionConfiguration(authoring, catalog.data.entries);
    return configuration.available ? {
      translationToleranceM: configuration.configuration.values.translation_tol_m,
      maxBlendDeviationRad: configuration.configuration.values.max_blend_deviation_rad,
    } : null;
  }, [catalog, programController.state.workspace?.draft.authoring]);
  const robotControl = useRobotControl(
    headlessEnabled,
    programRuntime.cycleState !== null
      && programRuntime.cycleState !== 'idle_after_reset',
    ['motion-stop', 'full-stop'].includes(programRuntime.state.pending ?? '')
      || ['stopping', 'full_stopping'].includes(
        programRuntime.state.run?.state ?? '',
      ),
    headlessSnapshot,
    headlessStateSource,
    programCartesianDefaults,
  );
  const operationalErrors = useOperationalErrors();
  useOperationalErrorMirror({
    source: 'composer.control_transport',
    title: 'Control connection became unavailable',
    message: headlessState.transportError,
    onDismiss: headlessState.reconnect,
  });
  useOperationalErrorMirror({
    source: 'composer.deployment_workspace',
    title: 'Deployment command or response failed',
    message: deploymentController.error,
    onDismiss: deploymentController.dismissError,
  });
  useOperationalErrorMirror({
    source: 'composer.program_workspace',
    title: 'Program authoring command or response failed',
    message: programController.state.error,
    onDismiss: programController.clearFeedback,
  });
  useOperationalErrorMirror({
    source: 'composer.program_runtime',
    title: 'Program Run, Stop, or Reset failed',
    message: programRuntime.state.error,
    onDismiss: programRuntime.dismissError,
  });
  useOperationalErrorMirror({
    source: 'composer.full_cell_stop',
    title: 'Cell Full Stop command failed',
    message: robotControl.fullStopError,
    onDismiss: robotControl.dismissFullStopError,
    attentionPolicy: 'acknowledgement_required',
  });
  useOperationalErrorMirror({
    source: 'composer.runtime_scale',
    title: 'Runtime velocity update failed',
    message: robotControl.runtimeScaleError,
    onDismiss: robotControl.dismissRuntimeScaleError,
  });
  const robotOperationalError = robotControl.state.error !== null
    && !robotControl.state.error.startsWith('The authored ')
    && !robotControl.state.error.startsWith('The stored ')
    ? robotControl.state.error
    : null;
  useOperationalErrorMirror({
    source: 'composer.robot_control',
    title: 'Robot control request failed',
    message: robotOperationalError,
    onDismiss: robotControl.dismissError,
  });
  useOperationalErrorMirror({
    source: 'composer.structural_manifest',
    title: 'Viewer model response failed',
    message: manifest.status === 'error' ? manifest.message : null,
    onDismiss: retryManifest,
  });
  useOperationalErrorMirror({
    source: 'composer.source_model',
    title: 'Viewer source-model response failed',
    message: sourceModel.status === 'error' ? sourceModel.message : null,
    onDismiss: retrySourceModel,
  });
  const clientErrors = operationalErrors.errors;
  const runtimeAttentionItems = useMemo(() => [
    ...attentionAlarmsNewestFirst(headlessSnapshot?.alarms.alarms ?? []).map((alarm) => ({
      identity: `backend:${alarm.activationId}`,
    })),
    ...clientErrors.map((error) => ({
      identity: `composer:${error.id}`,
    })),
  ], [clientErrors, headlessSnapshot]);
  const openRuntimeAttention = useCallback((identity: string) => {
    setRuntimeAttentionRequest(identity);
    // Automatic attention may choose the initial compact presentation, but it
    // must never override an already-open panel that the operator controls.
    if (globalPanel !== 'runtime') {
      setRuntimePanelExpanded(false);
      setRuntimePanelAutomatic(true);
    }
    setGlobalPanel('runtime');
  }, [globalPanel]);
  useEffect(() => {
    if (shouldCloseAutomaticRuntimeAttention(
      runtimePanelAutomatic,
      globalPanel,
      runtimeAttentionItems.length,
    )) {
      setGlobalPanel(null);
      setRuntimePanelExpanded(false);
      setRuntimeAttentionRequest(null);
      setRuntimePanelAutomatic(false);
    }
  }, [globalPanel, runtimeAttentionItems.length, runtimePanelAutomatic]);
  const runtimeView = headlessSnapshot?.componentOverview ?? null;
  const {
    viewerWidthMin,
    viewerWidthMax,
    inspectorWidthMax,
  } = workbenchWidthLimits(workbenchWidth, inspectorWidth);
  const programInspectorHeightMax = Math.max(
    INSPECTOR_SECTION_MIN_HEIGHT,
    inspectorHeight - INSPECTOR_SECTION_MIN_HEIGHT - WORKBENCH_DIVIDER_WIDTH,
  );
  const layoutStyle = {
    '--viewer-width': `${viewerWidth}px`,
    '--inspector-width': `${inspectorWidth}px`,
    '--program-inspector-height': `${programInspectorHeight}px`,
  } as CSSProperties;
  useEffect(() => {
    const element = workbench.current;
    if (element === null) return undefined;
    const update = () => setWorkbenchWidth(element.clientWidth);
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    setViewerWidth((current) => clamp(current, viewerWidthMin, viewerWidthMax));
  }, [viewerWidthMax, viewerWidthMin]);
  useEffect(() => {
    setInspectorWidth((current) => clamp(
      current,
      INSPECTOR_WIDTH_MIN,
      inspectorWidthMax,
    ));
  }, [inspectorWidthMax]);
  useEffect(() => {
    const element = centralInspector.current;
    if (element === null) return undefined;
    const update = () => setInspectorHeight(element.clientHeight);
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    setProgramInspectorHeight((current) => clamp(
      current,
      INSPECTOR_SECTION_MIN_HEIGHT,
      programInspectorHeightMax,
    ));
  }, [programInspectorHeightMax]);
  useEffect(() => {
    if (
      bothInspectorSectionsExpanded
      && !previousBothInspectorSectionsExpanded.current
    ) {
      setProgramInspectorHeight(Math.floor(inspectorHeight / 2));
    }
    previousBothInspectorSectionsExpanded.current = bothInspectorSectionsExpanded;
  }, [bothInspectorSectionsExpanded, inspectorHeight]);
  const handleProgramInspectorState = useCallback((next: {
    active: boolean;
    title: string;
    readOnlyReason: string | null;
  }) => {
    setProgramInspectorState((current) => (
      current.active === next.active
      && current.title === next.title
      && current.readOnlyReason === next.readOnlyReason
        ? current
        : next
    ));
  }, []);
  const maximizeProgramInspector = useCallback(() => {
    const alreadyMaximized = viewerInspectorPanel !== null
      && viewerInspectorMinimized
      && !programInspectorMinimized;
    setProgramInspectorMinimized(false);
    if (viewerInspectorPanel !== null) {
      setViewerInspectorMinimized(!alreadyMaximized);
    }
  }, [programInspectorMinimized, viewerInspectorMinimized, viewerInspectorPanel]);
  const maximizeViewerInspector = useCallback(() => {
    const alreadyMaximized = programInspectorState.active
      && programInspectorMinimized
      && !viewerInspectorMinimized;
    setViewerInspectorMinimized(false);
    if (programInspectorState.active) {
      setProgramInspectorMinimized(!alreadyMaximized);
    }
  }, [programInspectorMinimized, programInspectorState.active, viewerInspectorMinimized]);
  const openRuntimeDetails = useCallback(() => {
    setRuntimePanelExpanded(true);
    setRuntimePanelAutomatic(false);
    setRuntimeAttentionRequest(null);
    setGlobalPanel('runtime');
  }, []);
  const closeRuntimePanel = useCallback(() => {
    setGlobalPanel(null);
    setRuntimePanelExpanded(false);
    setRuntimeAttentionRequest(null);
    setRuntimePanelAutomatic(false);
  }, []);
  const setRuntimePanelExpansion = useCallback((expanded: boolean) => {
    setRuntimePanelExpanded(expanded);
    if (expanded) setRuntimePanelAutomatic(false);
  }, []);

  const connectedProjection = headlessEnabled
    && headlessSnapshot !== null
    && headlessState.transportStatus === 'connected'
    && headlessState.semanticStatus === 'current';
  const connectionLabel = connectedProjection
    ? 'Headless owner connected'
    : !headlessEnabled
      ? 'Runtime unavailable'
      : headlessState.transportStatus === 'delayed'
        ? 'Control connection delayed'
        : headlessState.transportStatus === 'stale'
          ? 'Control connection stale'
          : headlessState.transportStatus === 'connected'
            ? `Headless owner connected · control state ${headlessState.semanticStatus}`
            : headlessState.transportStatus === 'connecting'
              ? 'Connecting to Headless owner'
              : 'Headless owner disconnected';
  const connectionDetail = connectedProjection
    ? `Receiver ${headlessSnapshot.receiverEpoch} · snapshot ${headlessSnapshot.snapshotSequence}`
    : headlessState.transportError
      ?? (headlessState.semanticStatus === 'current'
        ? null
        : `Control state ${headlessState.semanticStatus}: ${headlessState.semanticReason
          ?? 'owner projection unavailable'}`)
      ?? headlessState.error
      ?? (health.status === 'error' || health.status === 'absent'
        ? health.message
        : health.status === 'ready'
          ? health.data.runtimeAvailability?.message ?? 'Structural Composer mode'
          : 'Reading backend status…');
  const deploymentStatus = runtimeStatusPresentation({
    status: runtimeView,
    backendConnected: connectedProjection,
    error: headlessState.error,
    connectionDetail,
    unacknowledgedAlarms: headlessSnapshot?.alarms.unacknowledgedCount ?? 0,
    clientErrorCount: clientErrors.length,
  });
  const deploymentTitle = programInvalidEdit
    ? `Review or discard the invalid Program field edit first. Component status: ${deploymentStatus.accessibleLabel}. ${deploymentStatus.detail}`
    : `${deploymentStatus.accessibleLabel}. ${deploymentStatus.detail}`;

  return (
    <main className="app-shell">
      <header className="app-header">
        <div className="app-identity" aria-label="ManyForge Composer">
          <span className="brand-mark" aria-hidden="true">MF</span>
          <strong>ManyForge</strong>
        </div>
        <nav className="workspace-switcher" aria-label="Workbench sections">
          <button
            type="button"
            data-workspace="program"
            className={activeWorkspace === 'program' ? 'active' : ''}
            aria-pressed={activeWorkspace === 'program'}
            onClick={() => setActiveWorkspace('program')}
          >
            Program
          </button>
          <button
            type="button"
            data-workspace="scene"
            className={activeWorkspace === 'scene' ? 'active' : ''}
            aria-pressed={activeWorkspace === 'scene'}
            disabled={programInvalidEdit}
            title={programInvalidEdit ? 'Review or discard the invalid Program field edit first.' : undefined}
            onClick={() => setActiveWorkspace('scene')}
          >
            Scene
          </button>
          <button
            type="button"
            data-workspace="deployment"
            className={`deployment-status-button readiness-${deploymentStatus.readiness}${
              activeWorkspace === 'deployment' ? ' active' : ''
            }`}
            aria-pressed={activeWorkspace === 'deployment'}
            aria-label={`Deployment — ${deploymentStatus.accessibleLabel}. Live status feed ${
              deploymentStatus.backendConnected ? 'connected' : 'unavailable'
            }. ${deploymentStatus.attentionCount > 0
              ? `${deploymentStatus.attentionCount} attention ${
                  deploymentStatus.attentionCount === 1 ? 'item' : 'items'
                }. `
              : ''}${deploymentStatus.detail}`}
            data-backend-connected={deploymentStatus.backendConnected}
            disabled={programInvalidEdit}
            title={deploymentTitle}
            onClick={() => {
              setActiveWorkspace('deployment');
              setComponentOverviewRequest((current) => current + 1);
            }}
          >
            <span>Deployment</span>
            <span
              className={`deployment-status-indicator${
                deploymentStatus.attentionCount > 0 ? ' has-attention' : ''
              }`}
              aria-hidden="true"
              title={`${deploymentStatus.backendConnected
                ? 'Live status feed connected'
                : 'Live status feed unavailable'}${deploymentStatus.attentionCount > 0
                ? ` · ${deploymentStatus.attentionCount} attention ${
                    deploymentStatus.attentionCount === 1 ? 'item' : 'items'
                  }`
                : ''}`}
            >
              {deploymentStatus.attentionCount > 0
                ? deploymentStatus.attentionCount
                : ''}
            </span>
          </button>
        </nav>
        {activeWorkspace === 'deployment' ? (
          <DeploymentDocumentControls controller={deploymentController} />
        ) : (
          <ProgramDocumentControls
            controller={programController}
            runtime={programRuntime}
            catalogEntries={catalog.status === 'ready' ? catalog.data.entries : []}
            localInvalidEdit={programInvalidEdit}
            settingsRequest={programSettingsRequest}
            onProgramRequirementCreated={(kind, id) => setProgramRequirementCreated((current) => ({
              sequence: (current?.sequence ?? 0) + 1,
              kind,
              id,
            }))}
          />
        )}
        <div className="global-surface-launchers" aria-label="Application tools">
          <button
            ref={runtimeButton}
            type="button"
            aria-haspopup="dialog"
            aria-expanded={globalPanel === 'runtime'}
            onClick={() => {
              setRuntimePanelExpanded(false);
              setRuntimePanelAutomatic(false);
              setRuntimeAttentionRequest(null);
              setGlobalPanel('runtime');
            }}
          >
            Runtime / history
            {clientErrors.length + (headlessSnapshot?.alarms.unacknowledgedCount ?? 0) > 0 && (
              <span className="global-attention-count">
                {clientErrors.length + (headlessSnapshot?.alarms.unacknowledgedCount ?? 0)}
              </span>
            )}
          </button>
          <button
            ref={assistantButton}
            type="button"
            aria-haspopup="dialog"
            aria-expanded={globalPanel === 'assistant'}
            disabled={!assistantEnabled}
            title={assistantEnabled ? 'Open the AI assistant.' : 'The assistant bridge is unavailable.'}
            onClick={() => setGlobalPanel('assistant')}
          >AI</button>
        </div>
      </header>

      <div
        ref={workbench}
        className="workbench"
        style={layoutStyle}
      >
        <section className="workbench-workspace">
          {activeWorkspace === 'program' ? (
            <ProgramWorkspace
              controller={programController}
              runtime={programRuntime}
              catalog={catalog}
              environment={nodeEnvironment}
              sourceModel={sourceModel}
              deploymentWorkspace={deploymentController.workspace}
              motionDescriptors={robotControl.motionTargets.map(
                (target) => target.targetDescriptor,
              )}
              onRetryCatalog={retryCatalog}
              onRetryEnvironment={retryNodeEnvironment}
              onInvalidEditChange={setProgramInvalidEdit}
              onOpenScene={() => setActiveWorkspace('scene')}
              onOpenProgramSettings={() => setProgramSettingsRequest((current) => current + 1)}
              inspectorTarget={programInspectorTarget}
              inspectorMinimized={programInspectorMinimized}
              onInspectorMinimizedChange={setProgramInspectorMinimized}
              onInspectorMaximize={maximizeProgramInspector}
              onInspectorStateChange={handleProgramInspectorState}
              programRequirementCreated={programRequirementCreated}
            />
          ) : activeWorkspace === 'scene' ? (
            <div
              ref={setSceneEditorTarget}
              className="scene-workspace-host"
              aria-label="Scene authoring workspace"
            />
          ) : (
            <div className="deployment-workspace">
              <DeploymentCompositionPanel
                controller={deploymentController}
                enabled={headlessEnabled}
                snapshot={headlessSnapshot}
                sourceModel={sourceModel}
                motionDescriptors={robotControl.motionTargets.map(
                  (target) => target.targetDescriptor,
                )}
                gripperDescriptors={robotControl.gripperTargets.map(
                  (target) => target.targetDescriptor,
                )}
                componentOverview={{
                  request: componentOverviewRequest,
                  status: runtimeView,
                  backendConnected: connectedProjection,
                  connectionLabel,
                  connectionDetail,
                  contextFacts: programRuntime.state.currentScenario?.currentScenario === null
                    || programRuntime.state.currentScenario === null
                    ? []
                    : [
                        `Scenario ${programRuntime.state.currentScenario.currentScenario.scenarioId}`,
                        `Deployment ${programRuntime.state.currentScenario.currentScenario.deploymentRevision}`,
                        `Program ${programRuntime.state.currentScenario.currentScenario.programRevision}`,
                      ],
                  unacknowledgedAlarms:
                    headlessSnapshot?.alarms.unacknowledgedCount ?? 0,
                  activeAcknowledgedAlarms:
                    headlessSnapshot?.alarms.activeAcknowledgedCount ?? 0,
                  clientErrorCount: clientErrors.length,
                  error: headlessState.error,
                  onReconnect: () => {
                    headlessState.reconnect();
                    if (structuralMode === null) retryHealth();
                  },
                  onOpenRuntimeHistory: openRuntimeDetails,
                }}
              />
            </div>
          )}
        </section>

        <PaneDivider
          label="central inspector"
          value={inspectorWidth}
          min={INSPECTOR_WIDTH_MIN}
          max={inspectorWidthMax}
          direction={-1}
          onChange={setInspectorWidth}
        />

        <aside
          ref={centralInspector}
          className={`workbench-inspector${bothInspectorSectionsExpanded ? ' split' : ''}`}
          aria-label="Program and viewer inspectors"
        >
          <div
            ref={setProgramInspectorTarget}
            className={`central-inspector-slot program-slot${
              programInspectorState.active && !programInspectorMinimized ? ' expanded' : ' minimized'
            }${programInspectorState.active ? ' active' : ' inactive'}`}
          >
            {activeWorkspace !== 'program' && (
              <div className="central-inspector-placeholder">
                <strong>Program inspector</strong>
                <span>No Program node is selected.</span>
              </div>
            )}
          </div>
          {bothInspectorSectionsExpanded && (
            <PaneDivider
              axis="y"
              className="central-inspector-divider"
              label="Program and viewer inspector height"
              value={programInspectorHeight}
              min={INSPECTOR_SECTION_MIN_HEIGHT}
              max={programInspectorHeightMax}
              onChange={setProgramInspectorHeight}
            />
          )}
          <div
            ref={setViewerInspectorTarget}
            className={`central-inspector-slot viewer-inspector-slot${
              viewerInspectorPanel !== null && !viewerInspectorMinimized ? ' expanded' : ' minimized'
            }${viewerInspectorPanel !== null ? ' active' : ' inactive'}`}
          />
        </aside>

        <PaneDivider
          label="viewer and live controls"
          value={viewerWidth}
          min={viewerWidthMin}
          max={viewerWidthMax}
          direction={-1}
          onChange={setViewerWidth}
        />

        <aside className="workbench-commands" aria-label="3D viewer and live controls">
          <div className="workbench-pane">
            <div className="viewer-slot">
              <SceneAuthoringWorkspace
                editorTarget={sceneEditorTarget}
                environment={nodeEnvironment}
                headlessStateSource={headlessStateSource}
                manifest={manifest}
                onRetryManifest={retryManifest}
                onRetrySourceModel={retrySourceModel}
                programController={programController}
                programRuntime={programRuntime}
                robotControl={robotControl}
                runtimeEnabled={connectedProjection}
                sourceModel={sourceModel}
                viewerActive
                viewerInspectorTarget={viewerInspectorTarget}
                viewerInspectorMinimized={viewerInspectorMinimized}
                onViewerInspectorMinimizedChange={setViewerInspectorMinimized}
                onViewerInspectorMaximize={maximizeViewerInspector}
                onViewerInspectorPanelChange={setViewerInspectorPanel}
                onOpenRuntimeHistory={openRuntimeDetails}
                runtimeOverlay={globalPanel === 'runtime' ? (
                  <HeadlessRuntimeSurfaces
                    enabled={headlessEnabled}
                    headlessStateSource={headlessStateSource}
                    clientErrors={clientErrors}
                    onAcknowledgeClientError={operationalErrors.dismiss}
                    attentionRequest={runtimeAttentionRequest}
                    expanded={runtimePanelExpanded}
                    onExpandedChange={setRuntimePanelExpansion}
                    onClose={closeRuntimePanel}
                  />
                ) : undefined}
              />
            </div>
          </div>
        </aside>
      </div>
      <RuntimeAttentionAutoOpen
        items={runtimeAttentionItems}
        onOpen={openRuntimeAttention}
      />
      <OverlayPanel
        open={globalPanel === 'assistant'}
        title="AI assistant"
        titleId="assistant-overlay-title"
        onClose={() => setGlobalPanel(null)}
        returnFocusRef={assistantButton}
      >
        <AssistantOverlay
          enabled={assistantEnabled}
          headlessConnected={headlessEnabled}
          programController={programController}
        />
      </OverlayPanel>
    </main>
  );
}
