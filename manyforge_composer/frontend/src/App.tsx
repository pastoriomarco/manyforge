import { useCallback, useRef, useState } from 'react';
import type { CSSProperties, RefObject } from 'react';

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
            <label className="pane-resize-control">
              Library width
              <input
                type="range"
                aria-label="Library width"
                min="220"
                max="420"
                step="10"
                value={navigationWidth}
                onChange={(event) => setNavigationWidth(event.currentTarget.valueAsNumber)}
              />
            </label>
            {/* Catalog is first so the node palette is useful at startup. */}
            <CatalogPanel catalog={catalog} onRetry={retryCatalog} />
            <details className="workbench-disclosure">
              <summary>Artifacts · Program / Deployment</summary>
              <ArtifactBrowser onSelectionChange={handleArtifactSelection} />
            </details>
            <details className="workbench-disclosure">
              <summary>Deployment composition</summary>
              <DeploymentCompositionPanel enabled={headlessEnabled} />
            </details>
          </div>
        </aside>

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
            <label className="pane-resize-control">
              Operations rail width
              <input
                type="range"
                aria-label="Operations rail width"
                min="480"
                max="720"
                step="10"
                value={commandsWidth}
                onChange={(event) => setCommandsWidth(event.currentTarget.valueAsNumber)}
              />
            </label>
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
