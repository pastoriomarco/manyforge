import { useCallback, useState } from 'react';

import { ApiError, api } from './api/client';
import { ArtifactBrowser } from './components/ArtifactBrowser';
import { AssistantOverlay } from './components/AssistantOverlay';
import { CatalogPanel } from './components/CatalogPanel';
import { CCLCPanel } from './components/CCLCPanel';
import { DeploymentCompositionPanel } from './components/DeploymentCompositionPanel';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { ProgramRuntimePanel } from './components/ProgramRuntimePanel';
import { SceneAuthoringWorkspace } from './components/SceneAuthoringWorkspace';
import { UnavailableBanner } from './components/UnavailableBanner';
import { HeadlessRuntimeSurfacesView } from './components/HeadlessRuntimeSurfaces';
import { RobotControlPanelView } from './components/RobotControlPanel';
import { useHeadlessState } from './hooks/useHeadlessState';
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
  const headlessState = useHeadlessState(headlessEnabled);
  const programController = useProgramWorkspace();
  const programRuntime = useProgramRuntime(headlessEnabled);
  const robotControl = useRobotControl(headlessEnabled);
  const handleArtifactSelection = useCallback((
    kind: ArtifactKind,
    entry: FileBrowserEntry | null,
  ) => {
    setSelectedProgramPath(kind === 'program' ? entry?.path ?? null : null);
  }, []);

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

      <HeadlessRuntimeSurfacesView
        enabled={headlessEnabled}
        headlessState={headlessState}
      />

      <DeploymentCompositionPanel enabled={headlessEnabled} />

      <RobotControlPanelView
        enabled={headlessEnabled}
        controller={robotControl}
      />

      <CCLCPanel enabled={headlessEnabled} />

      <ProgramWorkspace
        controller={programController}
        runtime={programRuntime}
        catalog={catalog}
        environment={nodeEnvironment}
        selectedProgramPath={selectedProgramPath}
        onRetryCatalog={retryCatalog}
        onRetryEnvironment={retryNodeEnvironment}
      />

      <ProgramRuntimePanel
        enabled={headlessEnabled}
        controller={programRuntime}
      />

      <AssistantOverlay
        enabled={assistantEnabled}
        headlessConnected={headlessEnabled}
        programController={programController}
      />

      <SceneAuthoringWorkspace
        enabled={headlessEnabled}
        environment={nodeEnvironment}
        headlessState={headlessState}
        manifest={manifest}
        onRetryManifest={retryManifest}
        programController={programController}
        robotControl={robotControl}
      />

      <div className="lower-grid">
        <CatalogPanel catalog={catalog} onRetry={retryCatalog} />
        <ArtifactBrowser onSelectionChange={handleArtifactSelection} />
      </div>

      <footer className="app-footer">
        Composer owns local drafts and presentation only. Runtime state,
        validation, execution, scene, lifecycle, and Stop remain headless-owner
        responsibilities.
      </footer>
    </main>
  );
}
