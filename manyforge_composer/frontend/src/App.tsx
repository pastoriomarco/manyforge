import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties } from 'react';

import { ApiError, api } from './api/client';
import { AssistantOverlay } from './components/AssistantOverlay';
import { CCLCPanel } from './components/CCLCPanel';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import {
  HeadlessRuntimeSurfaces,
  runtimeStatusView,
} from './components/HeadlessRuntimeSurfaces';
import { ProgramDocumentControls } from './components/ProgramDocumentControls';
import { PaneDivider } from './components/PaneDivider';
import { ProgramRuntimePanel } from './components/ProgramRuntimePanel';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { RobotControlPanelView } from './components/RobotControlPanel';
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
const AUTHORING_WIDTH_MIN = 632;
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
  const [workbenchWidth, setWorkbenchWidth] = useState(() => (
    typeof window === 'undefined' ? 1440 : window.innerWidth
  ));
  const [sceneEditorTarget, setSceneEditorTarget] = useState<HTMLElement | null>(null);
  const [runtimeDetailsOpen, setRuntimeDetailsOpen] = useState(false);
  const [programInvalidEdit, setProgramInvalidEdit] = useState(false);
  const [programSettingsRequest, setProgramSettingsRequest] = useState(0);
  const [programRequirementCreated, setProgramRequirementCreated] = useState<{
    sequence: number;
    id: string;
    kind: 'component_role' | 'target_role';
  } | null>(null);
  const workbench = useRef<HTMLDivElement>(null);
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
  const runtimeView = useMemo(
    () => headlessSnapshot === null ? null : runtimeStatusView(headlessSnapshot.runtime),
    [headlessSnapshot],
  );
  const viewerWidthMax = Math.max(
    VIEWER_WIDTH_MIN,
    Math.min(
      Math.floor(workbenchWidth * VIEWER_WIDTH_MAX_FRACTION),
      workbenchWidth - AUTHORING_WIDTH_MIN - WORKBENCH_DIVIDER_WIDTH,
    ),
  );
  const viewerWidthMin = Math.min(VIEWER_WIDTH_MIN, viewerWidthMax);
  const layoutStyle = {
    '--viewer-width': `${viewerWidth}px`,
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
  const openRuntimeDetails = useCallback(() => {
    setRuntimeDetailsOpen(true);
    window.requestAnimationFrame(() => {
      document.getElementById('runtime-observability')?.scrollIntoView({
        block: 'nearest',
      });
    });
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
        <RuntimeStatusOverlay
          status={runtimeView}
          backendConnected={connectedProjection}
          error={headlessState.error}
          connectionLabel={connectionLabel}
          connectionDetail={connectionDetail}
          unacknowledgedAlarms={headlessSnapshot?.alarms.unacknowledgedCount ?? 0}
          activeAcknowledgedAlarms={headlessSnapshot?.alarms.activeAcknowledgedCount ?? 0}
          onRetry={retryHealth}
          onOpenDetails={openRuntimeDetails}
        />
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
              <DeploymentCompositionPanel enabled={headlessEnabled} />
            </div>
          )}
        </section>

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
                robotControl={robotControl}
                sourceModel={sourceModel}
                viewerActive
              />
            </div>
            <div className="live-controls-scroll">
              <ProgramRuntimePanel
                enabled={headlessEnabled}
                controller={programRuntime}
              />
              <details className="workbench-disclosure">
                <summary>Robot control</summary>
                <RobotControlPanelView
                  enabled={headlessEnabled}
                  controller={robotControl}
                />
              </details>
              <details className="workbench-disclosure">
                <summary>Component lifecycle / CCLC</summary>
                <CCLCPanel enabled={headlessEnabled} />
              </details>
              <details
                id="runtime-observability"
                className="workbench-disclosure"
                open={runtimeDetailsOpen}
                onToggle={(event) => setRuntimeDetailsOpen(event.currentTarget.open)}
              >
                <summary>Runtime, alarms, and history</summary>
                <HeadlessRuntimeSurfaces
                  enabled={headlessEnabled}
                  headlessStateSource={headlessStateSource}
                />
              </details>
              <details className="workbench-disclosure">
                <summary>Assistant</summary>
                <AssistantOverlay
                  enabled={assistantEnabled}
                  headlessConnected={headlessEnabled}
                  programController={programController}
                />
              </details>
            </div>
          </div>
        </aside>
      </div>
    </main>
  );
}
