import { ApiError, api } from './api/client';
import { ArtifactBrowser } from './components/ArtifactBrowser';
import { CatalogPanel } from './components/CatalogPanel';
import { StructuralViewer } from './components/StructuralViewer';
import { UnavailableBanner } from './components/UnavailableBanner';
import { useStructuralResource } from './state/structuralResource';

function isNotFound(error: unknown): boolean {
  return error instanceof ApiError && error.status === 404;
}

export default function App() {
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

  return (
    <main className="app-shell">
      <header className="app-header">
        <div className="brand-mark" aria-hidden="true">MF</div>
        <div>
          <p className="eyebrow">ManyForge Composer</p>
          <p className="app-title">Structural inspection shell</p>
        </div>
        <div className="header-boundary">
          <span className="boundary-dot" aria-hidden="true" />
          Read-only structural mode
        </div>
      </header>

      <UnavailableBanner health={health} onRetry={retryHealth} />

      <StructuralViewer manifest={manifest} onRetry={retryManifest} />

      <div className="lower-grid">
        <CatalogPanel catalog={catalog} onRetry={retryCatalog} />
        <ArtifactBrowser />
      </div>

      <footer className="app-footer">
        This shell is a replaceable client of retained structural APIs. It owns no
        runtime, scene, lifecycle, alarm, assistant, or command semantics.
      </footer>
    </main>
  );
}
