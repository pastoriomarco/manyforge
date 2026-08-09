import { useCallback, useState } from 'react';

import { ApiError, api } from './api/client';
import { ArtifactBrowser } from './components/ArtifactBrowser';
import { CatalogPanel } from './components/CatalogPanel';
import { ProgramWorkspace } from './components/ProgramWorkspace';
import { StructuralViewer } from './components/StructuralViewer';
import { UnavailableBanner } from './components/UnavailableBanner';
import { HeadlessRuntimeSurfaces } from './components/HeadlessRuntimeSurfaces';
import { RobotControlPanel } from './components/RobotControlPanel';
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
    absentMessage: 'No complete URDF is available for the authored deployment.',
    failureMessage: 'Could not read the authored-neutral URDF manifest.',
  });
  const [nodeEnvironment, retryNodeEnvironment] = useStructuralResource(
    api.nodes.environment,
    { failureMessage: 'Could not read the active v2 node environment.' },
  );
  const headlessEnabled = health.status === 'ready'
    && health.data.mode === 'headless_client';
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

      <HeadlessRuntimeSurfaces enabled={headlessEnabled} />

      <RobotControlPanel enabled={headlessEnabled} />

      <ProgramWorkspace
        catalog={catalog}
        environment={nodeEnvironment}
        selectedProgramPath={selectedProgramPath}
        onRetryCatalog={retryCatalog}
        onRetryEnvironment={retryNodeEnvironment}
      />

      <StructuralViewer manifest={manifest} onRetry={retryManifest} />

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
