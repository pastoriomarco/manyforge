import { useCallback, useRef, useState } from 'react';
import type {
  CSSProperties,
  KeyboardEvent as ReactKeyboardEvent,
  PointerEvent as ReactPointerEvent,
  RefObject,
} from 'react';

import { ApiError, api } from './api/client';
import { ArtifactBrowser } from './components/ArtifactBrowser';
import { AssistantOverlay } from './components/AssistantOverlay';
import { CatalogPanel } from './components/CatalogPanel';
import { CCLCPanel } from './components/CCLCPanel';
import { CollapsedRail } from './components/CollapsedRail';
import { ProgramRuntimePanel } from './components/ProgramRuntimePanel';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { SceneAuthoringWorkspace } from './components/SceneAuthoringWorkspace';
import { UnavailableBanner } from './components/UnavailableBanner';
import { HeadlessRuntimeSurfaces } from './components/HeadlessRuntimeSurfaces';
import { RobotControlPanelView } from './components/RobotControlPanel';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import { useHeadlessStateSource } from './hooks/useHeadlessState';
import { useProgramWorkspace } from './hooks/useProgramWorkspace';
import { useProgramRuntime } from './hooks/useProgramRuntime';
import { useRobotControl } from './hooks/useRobotControl';
import { useStructuralResource } from './state/structuralResource';
import type { ArtifactKind, FileBrowserEntry } from './types/api';

function isNotFound(error: unknown): boolean {
  return error instanceof ApiError && error.status === 404;
}

const NAVIGATION_WIDTH_MIN = 220;
const NAVIGATION_WIDTH_MAX = 420;
const COMMANDS_WIDTH_MIN = 480;
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
  const [selectedProgramPath, setSelectedProgramPath] = useState<string | null>(null);
  const [navigationWidth, setNavigationWidth] = useState(320);
  const [commandsWidth, setCommandsWidth] = useState(520);
  const [navigationCollapsed, setNavigationCollapsed] = useState(false);
  const [commandsCollapsed, setCommandsCollapsed] = useState(false);
  const navigationCollapse = useRef<HTMLButtonElement>(null);
  const navigationRail = useRef<HTMLButtonElement>(null);
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
  // The workbench owns one persistent viewer. Its own observer throttling
  // still controls demand rendering and latest-only data publication.
  const viewerActive = !commandsCollapsed;
  const programController = useProgramWorkspace();
  const programRuntime = useProgramRuntime(headlessEnabled);
  const robotControl = useRobotControl(headlessEnabled);
  const handleArtifactSelection = useCallback((
    kind: ArtifactKind,
    entry: FileBrowserEntry | null,
  ) => {
    setSelectedProgramPath(kind === 'program' ? entry?.path ?? null : null);
  }, []);
  const focusAfterLayout = (target: RefObject<HTMLButtonElement | null>) => {
    window.requestAnimationFrame(() => target.current?.focus());
  };
  const layoutStyle = {
    '--navigation-width': `${navigationWidth}px`,
    '--commands-width': `${commandsWidth}px`,
  } as CSSProperties;

  return (
    <main className="app-shell">
      <header className="app-header">
        <div className="brand-mark" aria-hidden="true">MF</div>
        <div>
          <p className="eyebrow">ManyForge Composer</p>
          <p className="app-title">Composable cell workbench</p>
        </div>
        <div className="header-boundary">
          <span
            className={`boundary-dot${headlessEnabled ? ' boundary-dot-connected' : ''}`}
            aria-hidden="true"
          />
          {headlessEnabled ? 'Headless owner connected' : 'Structural mode'}
        </div>
      </header>

      <UnavailableBanner health={health} onRetry={retryHealth} />

      <div
        className={[
          'workbench',
          navigationCollapsed ? 'navigation-collapsed' : '',
          commandsCollapsed ? 'commands-collapsed' : '',
        ].filter(Boolean).join(' ')}
        style={layoutStyle}
      >
        <aside className="workbench-navigation" aria-label="Catalog and artifact library">
          <div className="collapsed-rail-slot" hidden={!navigationCollapsed}>
            <CollapsedRail
              label="Library"
              side="left"
              buttonRef={navigationRail}
              onExpand={() => {
                setNavigationCollapsed(false);
                focusAfterLayout(navigationCollapse);
              }}
            />
          </div>
          <div className="workbench-pane" hidden={navigationCollapsed}>
            <div className="pane-toolbar">
              <strong>Library</strong>
              <button
                type="button"
                ref={navigationCollapse}
                aria-label="Collapse library pane"
                onClick={() => {
                  setNavigationCollapsed(true);
                  focusAfterLayout(navigationRail);
                }}
              >
                ‹
              </button>
            </div>
            {/* Catalog is first so the node palette is useful at startup. */}
            <CatalogPanel catalog={catalog} onRetry={retryCatalog} />
            <details className="workbench-disclosure" open>
              <summary>Artifacts · Program / Deployment</summary>
              <ArtifactBrowser onSelectionChange={handleArtifactSelection} />
            </details>
            <details className="workbench-disclosure">
              <summary>Deployment composition</summary>
              <DeploymentCompositionPanel enabled={headlessEnabled} />
            </details>
          </div>
        </aside>

        {!navigationCollapsed && (
          <WorkbenchDivider
            label="Library"
            value={navigationWidth}
            min={NAVIGATION_WIDTH_MIN}
            max={NAVIGATION_WIDTH_MAX}
            direction={1}
            onChange={setNavigationWidth}
          />
        )}

        <section className="workbench-workspace" aria-label="Program authoring workspace">
          <ProgramWorkspace
            controller={programController}
            runtime={programRuntime}
            catalog={catalog}
            environment={nodeEnvironment}
            selectedProgramPath={selectedProgramPath}
            onRetryCatalog={retryCatalog}
            onRetryEnvironment={retryNodeEnvironment}
          />
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
              <details className="workbench-disclosure">
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

      <footer className="app-footer">
        Composer owns local drafts and presentation only. Runtime state,
        validation, execution, scene, lifecycle, and Stop remain headless-owner
        responsibilities.
      </footer>
    </main>
  );
}
