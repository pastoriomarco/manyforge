import {
  useCallback,
  useEffect,
  useRef,
  useState,
} from 'react';
import type { CSSProperties, KeyboardEvent, RefObject } from 'react';

import { ApiError, api } from './api/client';
import { ArtifactBrowser } from './components/ArtifactBrowser';
import { AssistantOverlay } from './components/AssistantOverlay';
import { CatalogPanel } from './components/CatalogPanel';
import { CCLCPanel } from './components/CCLCPanel';
import { CollapsedRail } from './components/CollapsedRail';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { ProgramRuntimePanel } from './components/ProgramRuntimePanel';
import { SceneAuthoringWorkspace } from './components/SceneAuthoringWorkspace';
import { UnavailableBanner } from './components/UnavailableBanner';
import { HeadlessRuntimeSurfaces } from './components/HeadlessRuntimeSurfaces';
import { RobotControlPanelView } from './components/RobotControlPanel';
import { useHeadlessStateSource } from './hooks/useHeadlessState';
import { useProgramWorkspace } from './hooks/useProgramWorkspace';
import { useProgramRuntime } from './hooks/useProgramRuntime';
import { useRobotControl } from './hooks/useRobotControl';
import { useStructuralResource } from './state/structuralResource';
import type { ArtifactKind, FileBrowserEntry } from './types/api';

type WorkspaceTab = 'program' | 'deployment' | 'scene';
type MobilePane = 'navigation' | 'workspace' | 'commands';
const MOBILE_WORKBENCH_QUERY = '(max-width: 76rem)';

function useMobileWorkbench(): boolean {
  const [mobile, setMobile] = useState(false);
  useEffect(() => {
    const query = window.matchMedia(MOBILE_WORKBENCH_QUERY);
    const update = () => setMobile(query.matches);
    update();
    query.addEventListener('change', update);
    return () => query.removeEventListener('change', update);
  }, []);
  return mobile;
}

function isNotFound(error: unknown): boolean {
  return error instanceof ApiError && error.status === 404;
}

export default function App() {
  const [selectedProgramPath, setSelectedProgramPath] = useState<string | null>(null);
  const [workspaceTab, setWorkspaceTab] = useState<WorkspaceTab>('program');
  const [mobilePane, setMobilePane] = useState<MobilePane>('workspace');
  const [navigationWidth, setNavigationWidth] = useState(320);
  const [commandsWidth, setCommandsWidth] = useState(360);
  const [navigationCollapsed, setNavigationCollapsed] = useState(false);
  const [commandsCollapsed, setCommandsCollapsed] = useState(false);
  const mobileWorkbench = useMobileWorkbench();
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
  const viewerActive = workspaceTab === 'scene'
    && (!mobileWorkbench || mobilePane === 'workspace');
  const programController = useProgramWorkspace();
  const programRuntime = useProgramRuntime(headlessEnabled);
  const robotControl = useRobotControl(headlessEnabled);
  const handleArtifactSelection = useCallback((
    kind: ArtifactKind,
    entry: FileBrowserEntry | null,
  ) => {
    setSelectedProgramPath(kind === 'program' ? entry?.path ?? null : null);
  }, []);
  const selectAdjacentWorkspace = (
    event: KeyboardEvent<HTMLButtonElement>,
    current: WorkspaceTab,
  ) => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    event.preventDefault();
    const tabs: WorkspaceTab[] = ['program', 'deployment', 'scene'];
    const index = tabs.indexOf(current);
    const next = event.key === 'Home'
      ? tabs[0]
      : event.key === 'End'
        ? tabs[tabs.length - 1]
        : tabs[(index + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length];
    setWorkspaceTab(next);
    document.getElementById(`workspace-tab-${next}`)?.focus();
  };
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
          <p className="app-title">Composable cell workspace</p>
        </div>
        <div className="header-boundary">
          <span className="boundary-dot" aria-hidden="true" />
          {headlessEnabled ? 'Headless owner connected' : 'Structural mode'}
        </div>
      </header>

      <UnavailableBanner health={health} onRetry={retryHealth} />

      <nav className="mobile-pane-tabs" aria-label="Workbench areas">
        {([
          ['navigation', 'Navigate'],
          ['workspace', 'Workspace'],
          ['commands', 'Commands / status'],
        ] as const).map(([id, label]) => (
          <button
            type="button"
            key={id}
            aria-pressed={mobilePane === id}
            onClick={() => setMobilePane(id)}
          >
            {label}
          </button>
        ))}
      </nav>

      <div
        className={[
          'workbench',
          navigationCollapsed ? 'navigation-collapsed' : '',
          commandsCollapsed ? 'commands-collapsed' : '',
        ].filter(Boolean).join(' ')}
        style={layoutStyle}
        data-mobile-pane={mobilePane}
      >
        <aside className="workbench-navigation" aria-label="Artifact and node navigation">
          <div className="collapsed-rail-slot" hidden={!navigationCollapsed}>
            <CollapsedRail
              label="Navigation"
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
              <strong>Navigate</strong>
              <button
                type="button"
                ref={navigationCollapse}
                aria-label="Collapse navigation pane"
                onClick={() => {
                  setNavigationCollapsed(true);
                  focusAfterLayout(navigationRail);
                }}
              >
                ‹
              </button>
            </div>
            <label className="pane-resize-control">
              Navigation width
              <input
                type="range"
                aria-label="Navigation width"
                min="240"
                max="520"
                step="10"
                value={navigationWidth}
                onChange={(event) => setNavigationWidth(event.currentTarget.valueAsNumber)}
              />
            </label>
            <ArtifactBrowser onSelectionChange={handleArtifactSelection} />
            <CatalogPanel catalog={catalog} onRetry={retryCatalog} />
          </div>
        </aside>

        <section className="workbench-workspace" aria-label="Authoring workspace">
          <div className="workspace-tablist" role="tablist" aria-label="Workspace">
            {([
              ['program', 'Program'],
              ['deployment', 'Deployment'],
              ['scene', 'Scene / viewer'],
            ] as const).map(([id, label]) => (
              <button
                type="button"
                role="tab"
                id={`workspace-tab-${id}`}
                key={id}
                aria-selected={workspaceTab === id}
                aria-controls={`workspace-panel-${id}`}
                tabIndex={workspaceTab === id ? 0 : -1}
                onClick={() => setWorkspaceTab(id)}
                onKeyDown={(event) => selectAdjacentWorkspace(event, id)}
              >
                {label}
              </button>
            ))}
          </div>
          <div
            role="tabpanel"
            id="workspace-panel-program"
            aria-labelledby="workspace-tab-program"
            hidden={workspaceTab !== 'program'}
          >
            <ProgramWorkspace
              controller={programController}
              runtime={programRuntime}
              catalog={catalog}
              environment={nodeEnvironment}
              selectedProgramPath={selectedProgramPath}
              onRetryCatalog={retryCatalog}
              onRetryEnvironment={retryNodeEnvironment}
            />
          </div>
          <div
            role="tabpanel"
            id="workspace-panel-deployment"
            aria-labelledby="workspace-tab-deployment"
            hidden={workspaceTab !== 'deployment'}
          >
            <DeploymentCompositionPanel enabled={headlessEnabled} />
          </div>
          <div
            role="tabpanel"
            id="workspace-panel-scene"
            aria-labelledby="workspace-tab-scene"
            hidden={workspaceTab !== 'scene'}
          >
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
          </div>
        </section>

        <aside className="workbench-commands" aria-label="Commands and owner status">
          <div className="collapsed-rail-slot" hidden={!commandsCollapsed}>
            <CollapsedRail
              label="Commands"
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
              <strong>Commands / owner status</strong>
              <button
                type="button"
                ref={commandsCollapse}
                aria-label="Collapse commands and status pane"
                onClick={() => {
                  setCommandsCollapsed(true);
                  focusAfterLayout(commandsRail);
                }}
              >
                ›
              </button>
            </div>
            <label className="pane-resize-control">
              Commands width
              <input
                type="range"
                aria-label="Commands width"
                min="300"
                max="560"
                step="10"
                value={commandsWidth}
                onChange={(event) => setCommandsWidth(event.currentTarget.valueAsNumber)}
              />
            </label>
            <ProgramRuntimePanel
              enabled={headlessEnabled}
              controller={programRuntime}
            />
            <RobotControlPanelView
              enabled={headlessEnabled}
              controller={robotControl}
            />
            <CCLCPanel enabled={headlessEnabled} />
            <HeadlessRuntimeSurfaces
              enabled={headlessEnabled}
              headlessStateSource={headlessStateSource}
            />
            <AssistantOverlay
              enabled={assistantEnabled}
              headlessConnected={headlessEnabled}
              programController={programController}
            />
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
