import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties } from 'react';

import { ApiError, api } from './api/client';
import { AssistantOverlay } from './components/AssistantOverlay';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import {
  HeadlessRuntimeSurfaces,
  runtimeStatusView,
} from './components/HeadlessRuntimeSurfaces';
import { ProgramDocumentControls } from './components/ProgramDocumentControls';
import { OverlayPanel } from './components/OverlayPanel';
import { PaneDivider } from './components/PaneDivider';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { RuntimeStatusOverlay } from './components/RuntimeStatusOverlay';
import { SceneAuthoringWorkspace } from './components/SceneAuthoringWorkspace';
import {
  useHeadlessStateSource,
  useShellHeadlessState,
} from './hooks/useHeadlessState';
import { useProgramRuntime } from './hooks/useProgramRuntime';
import { useProgramWorkspace } from './hooks/useProgramWorkspace';
import { useRobotControl } from './hooks/useRobotControl';
import { useStructuralResource } from './state/structuralResource';

function isNotFound(error: unknown): boolean {
  return error instanceof ApiError && error.status === 404;
}

const VIEWER_WIDTH_MIN = 420;
const AUTHORING_WIDTH_MIN = 400;
const INSPECTOR_WIDTH_MIN = 260;
const INSPECTOR_WIDTH_MAX = 480;
const INSPECTOR_SECTION_MIN_HEIGHT = 140;
const WORKBENCH_DIVIDER_WIDTH = 8;
const VIEWER_WIDTH_MAX_FRACTION = 2 / 3;

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

export default function App() {
  const [activeWorkspace, setActiveWorkspace] = useState<'program' | 'scene' | 'deployment'>('program');
  const [viewerWidth, setViewerWidth] = useState(() => (
    typeof window === 'undefined' ? 640 : Math.round(window.innerWidth * 0.4)
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
  const [programInvalidEdit, setProgramInvalidEdit] = useState(false);
  const [programSettingsRequest, setProgramSettingsRequest] = useState(0);
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
    { failureMessage: 'Could not read the active v2 node environment.' },
  );
  const headlessEnabled = health.status === 'ready'
    && health.data.mode === 'headless_client';
  const [sourceModel, retrySourceModel] = useStructuralResource(api.sourceModel.get, {
    enabled: headlessEnabled,
    disabledMessage: 'Source-model facts require a connected headless owner.',
    failureMessage: 'Could not read the verified active source-model projection.',
  });
  const assistantEnabled = headlessEnabled
    && health.data.components.assistant_bridge === 'configured';
  const headlessStateSource = useHeadlessStateSource(headlessEnabled);
  const headlessState = useShellHeadlessState(headlessStateSource);
  const headlessSnapshot = headlessState.snapshot;
  const programController = useProgramWorkspace(headlessSnapshot);
  const programRuntime = useProgramRuntime(
    headlessEnabled,
    headlessSnapshot,
    programController.state.workspace,
  );
  const robotControl = useRobotControl(headlessEnabled);
  const clientErrors = useMemo(() => [
    ...(programRuntime.state.error === null ? [] : [{
      id: 'composer.program_runtime',
      title: 'Program command or response failed',
      message: programRuntime.state.error,
    }]),
    ...(robotControl.fullStopError === null ? [] : [{
      id: 'composer.full_cell_stop',
      title: 'Full-cell Stop command failed',
      message: robotControl.fullStopError,
    }]),
  ], [programRuntime.state.error, robotControl.fullStopError]);
  const runtimeView = useMemo(
    () => headlessSnapshot === null ? null : runtimeStatusView(
      headlessSnapshot.runtime,
      {
        unacknowledged: headlessSnapshot.alarms.unacknowledgedCount,
        activeAcknowledged: headlessSnapshot.alarms.activeAcknowledgedCount,
        programCycleState: headlessSnapshot.programCycle.state,
        executionOwnerAvailable: robotControl.state.status !== null,
        clientErrorCount: clientErrors.length,
      },
    ),
    [clientErrors.length, headlessSnapshot, robotControl.state.status],
  );
  const viewerWidthMax = Math.max(
    VIEWER_WIDTH_MIN,
    Math.min(
      Math.floor(workbenchWidth * VIEWER_WIDTH_MAX_FRACTION),
      workbenchWidth - AUTHORING_WIDTH_MIN - inspectorWidth
        - 2 * WORKBENCH_DIVIDER_WIDTH,
    ),
  );
  const viewerWidthMin = Math.min(VIEWER_WIDTH_MIN, viewerWidthMax);
  const inspectorWidthMax = Math.max(
    INSPECTOR_WIDTH_MIN,
    Math.min(
      INSPECTOR_WIDTH_MAX,
      workbenchWidth - AUTHORING_WIDTH_MIN - viewerWidthMin
        - 2 * WORKBENCH_DIVIDER_WIDTH,
    ),
  );
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
    setGlobalPanel('runtime');
  }, []);

  const connectedProjection = headlessEnabled
    && headlessSnapshot !== null
    && headlessState.error === null;
  const connectionLabel = connectedProjection
    ? 'Headless owner connected'
    : headlessEnabled ? 'Headless projection unavailable' : 'Runtime unavailable';
  const connectionDetail = connectedProjection
    ? `Receiver ${headlessSnapshot.receiverEpoch} · snapshot ${headlessSnapshot.snapshotSequence}`
    : headlessState.error
      ?? (health.status === 'error' || health.status === 'absent'
        ? health.message
        : health.status === 'ready'
          ? health.data.runtimeAvailability?.message ?? 'Structural Composer mode'
          : 'Reading backend status…');

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
            className={activeWorkspace === 'deployment' ? 'active' : ''}
            aria-pressed={activeWorkspace === 'deployment'}
            disabled={programInvalidEdit}
            title={programInvalidEdit ? 'Review or discard the invalid Program field edit first.' : undefined}
            onClick={() => setActiveWorkspace('deployment')}
          >
            Deployment
          </button>
        </nav>
        <ProgramDocumentControls
          controller={programController}
          runtime={programRuntime}
          localInvalidEdit={programInvalidEdit}
          settingsRequest={programSettingsRequest}
          onProgramRequirementCreated={(kind, id) => setProgramRequirementCreated((current) => ({
            sequence: (current?.sequence ?? 0) + 1,
            kind,
            id,
          }))}
        />
        <div className="global-surface-launchers" aria-label="Application tools">
          <button
            ref={runtimeButton}
            type="button"
            aria-haspopup="dialog"
            aria-expanded={globalPanel === 'runtime'}
            onClick={() => setGlobalPanel('runtime')}
          >Runtime / history</button>
          <button
            ref={assistantButton}
            type="button"
            aria-haspopup="dialog"
            aria-expanded={globalPanel === 'assistant'}
            disabled={!assistantEnabled}
            title={assistantEnabled ? 'Open the AI assistant.' : 'The assistant bridge is unavailable.'}
            onClick={() => setGlobalPanel('assistant')}
          >AI</button>
          <RuntimeStatusOverlay
            status={runtimeView}
            backendConnected={connectedProjection}
            error={headlessState.error}
            connectionLabel={connectionLabel}
            connectionDetail={connectionDetail}
            unacknowledgedAlarms={headlessSnapshot?.alarms.unacknowledgedCount ?? 0}
            activeAcknowledgedAlarms={headlessSnapshot?.alarms.activeAcknowledgedCount ?? 0}
            clientErrorCount={clientErrors.length}
            contextFacts={programRuntime.state.currentScenario?.currentScenario === null
              || programRuntime.state.currentScenario === null
              ? []
              : [
                  `Scenario ${programRuntime.state.currentScenario.currentScenario.scenarioId}`,
                  `Deployment ${programRuntime.state.currentScenario.currentScenario.deploymentRevision}`,
                  `Program ${programRuntime.state.currentScenario.currentScenario.programRevision}`,
                ]}
            onRetry={retryHealth}
            onOpenDetails={openRuntimeDetails}
          />
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
                enabled={headlessEnabled}
                snapshot={headlessSnapshot}
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
                runtimeEnabled={headlessEnabled}
                sourceModel={sourceModel}
                viewerActive
                viewerInspectorTarget={viewerInspectorTarget}
                viewerInspectorMinimized={viewerInspectorMinimized}
                onViewerInspectorMinimizedChange={setViewerInspectorMinimized}
                onViewerInspectorMaximize={maximizeViewerInspector}
                onViewerInspectorPanelChange={setViewerInspectorPanel}
                onOpenRuntimeHistory={() => setGlobalPanel('runtime')}
              />
            </div>
          </div>
        </aside>
      </div>
      <OverlayPanel
        open={globalPanel === 'runtime'}
        title="Runtime, alarms, and history"
        titleId="runtime-observability-title"
        kind="status"
        onClose={() => setGlobalPanel(null)}
        returnFocusRef={runtimeButton}
      >
        <HeadlessRuntimeSurfaces
          enabled={headlessEnabled}
          headlessStateSource={headlessStateSource}
          clientErrors={clientErrors}
          onAcknowledgeClientError={(id) => {
            if (id === 'composer.full_cell_stop') {
              robotControl.dismissFullStopError();
            } else {
              programRuntime.dismissError();
            }
          }}
        />
      </OverlayPanel>
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
