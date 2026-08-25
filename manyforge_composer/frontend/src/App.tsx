import { useCallback, useMemo, useRef, useState } from 'react';
import type {
  CSSProperties,
  KeyboardEvent as ReactKeyboardEvent,
  PointerEvent as ReactPointerEvent,
  RefObject,
} from 'react';

import { ApiError, api } from './api/client';
import { AssistantOverlay } from './components/AssistantOverlay';
import { CCLCPanel } from './components/CCLCPanel';
import { CollapsedRail } from './components/CollapsedRail';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import {
  HeadlessRuntimeSurfaces,
  runtimeStatusView,
} from './components/HeadlessRuntimeSurfaces';
import { ProgramDocumentControls } from './components/ProgramDocumentControls';
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

const COMMANDS_WIDTH_MIN = 420;
const COMMANDS_WIDTH_MAX = 720;

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

interface WorkbenchDividerProps {
  label: string;
  value: number;
  min: number;
  max: number;
  direction: 1 | -1;
  onChange: (value: number) => void;
}

function WorkbenchDivider({
  label,
  value,
  min,
  max,
  direction,
  onChange,
}: WorkbenchDividerProps) {
  const pointer = useRef<{ id: number; startX: number; startValue: number } | null>(null);

  const handlePointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    pointer.current = {
      id: event.pointerId,
      startX: event.clientX,
      startValue: value,
    };
  };
  const handlePointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    const active = pointer.current;
    if (active === null || active.id !== event.pointerId) return;
    onChange(clamp(
      active.startValue + direction * (event.clientX - active.startX),
      min,
      max,
    ));
  };
  const finishPointer = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (pointer.current?.id !== event.pointerId) return;
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    pointer.current = null;
  };
  const handleKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    const step = event.shiftKey ? 50 : 10;
    const next = event.key === 'ArrowLeft'
      ? value - direction * step
      : event.key === 'ArrowRight'
        ? value + direction * step
        : event.key === 'Home'
          ? min
          : event.key === 'End'
            ? max
            : null;
    if (next === null) return;
    event.preventDefault();
    onChange(clamp(next, min, max));
  };

  return (
    <div
      className="pane-divider"
      role="separator"
      tabIndex={0}
      aria-label={`Resize ${label} pane`}
      aria-orientation="vertical"
      aria-valuemin={min}
      aria-valuemax={max}
      aria-valuenow={value}
      aria-valuetext={`${value} pixels`}
      title={`Resize ${label} pane`}
      onPointerDown={handlePointerDown}
      onPointerMove={handlePointerMove}
      onPointerUp={finishPointer}
      onPointerCancel={finishPointer}
      onLostPointerCapture={() => {
        pointer.current = null;
      }}
      onKeyDown={handleKeyDown}
    />
  );
}

export default function App() {
  const [activeWorkspace, setActiveWorkspace] = useState<'program' | 'deployment'>('program');
  const [commandsWidth, setCommandsWidth] = useState(520);
  const [commandsCollapsed, setCommandsCollapsed] = useState(false);
  const [runtimeDetailsOpen, setRuntimeDetailsOpen] = useState(false);
  const commandsCollapse = useRef<HTMLButtonElement>(null);
  const commandsRail = useRef<HTMLButtonElement>(null);
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
  const focusAfterLayout = (target: RefObject<HTMLButtonElement | null>) => {
    window.requestAnimationFrame(() => target.current?.focus());
  };
  const layoutStyle = {
    '--commands-width': `${commandsWidth}px`,
  } as CSSProperties;
  const viewerActive = !commandsCollapsed;
  const openRuntimeDetails = useCallback(() => {
    setCommandsCollapsed(false);
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
            className={activeWorkspace === 'program' ? 'active' : ''}
            aria-pressed={activeWorkspace === 'program'}
            onClick={() => setActiveWorkspace('program')}
          >
            Program
          </button>
          <button
            type="button"
            className={activeWorkspace === 'deployment' ? 'active' : ''}
            aria-pressed={activeWorkspace === 'deployment'}
            onClick={() => setActiveWorkspace('deployment')}
          >
            Deployment
          </button>
        </nav>
        {activeWorkspace === 'program' ? (
          <ProgramDocumentControls
            controller={programController}
            runtime={programRuntime}
          />
        ) : (
          <div className="active-workspace-title">
            <strong>Deployment composition</strong>
            <span>Manager-owned desired and active graph</span>
          </div>
        )}
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
        className={`workbench${commandsCollapsed ? ' commands-collapsed' : ''}`}
        style={layoutStyle}
      >
        <section className="workbench-workspace">
          {activeWorkspace === 'program' ? (
            <ProgramWorkspace
              controller={programController}
              runtime={programRuntime}
              catalog={catalog}
              environment={nodeEnvironment}
              onRetryCatalog={retryCatalog}
              onRetryEnvironment={retryNodeEnvironment}
            />
          ) : (
            <div className="deployment-workspace">
              <DeploymentCompositionPanel enabled={headlessEnabled} />
            </div>
          )}
        </section>

        {!commandsCollapsed && (
          <WorkbenchDivider
            label="Operations"
            value={commandsWidth}
            min={COMMANDS_WIDTH_MIN}
            max={COMMANDS_WIDTH_MAX}
            direction={-1}
            onChange={setCommandsWidth}
          />
        )}

        <aside className="workbench-commands" aria-label="Viewer and operations rail">
          <div className="collapsed-rail-slot" hidden={!commandsCollapsed}>
            <CollapsedRail
              label="Operations"
              side="right"
              buttonRef={commandsRail}
              onExpand={() => {
                setCommandsCollapsed(false);
                focusAfterLayout(commandsCollapse);
              }}
            />
          </div>
          <div className="workbench-pane" hidden={commandsCollapsed}>
            <div className="pane-toolbar">
              <strong>Viewer / operations</strong>
              <button
                type="button"
                ref={commandsCollapse}
                aria-label="Collapse viewer and operations pane"
                onClick={() => {
                  setCommandsCollapsed(true);
                  focusAfterLayout(commandsRail);
                }}
              >
                ›
              </button>
            </div>
            <div className="visual-operations-stack">
              <SceneAuthoringWorkspace
                enabled={headlessEnabled}
                environment={nodeEnvironment}
                headlessStateSource={headlessStateSource}
                manifest={manifest}
                onRetryManifest={retryManifest}
                programController={programController}
                robotControl={robotControl}
                viewerActive={viewerActive}
              />
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
