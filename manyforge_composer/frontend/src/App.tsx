import { useEffect } from 'react';
import { Layout } from './components/Layout';
import { useCycleStore } from './store/cycleStore';
import { useCatalogStore } from './store/catalogStore';
import { useProgramStore } from './store/programStore';
import { useDeploymentStore } from './store/deploymentStore';
import { initComposerStore } from './store/composerStore';

let composerUnsubscribe: (() => void) | null = null;

export default function App() {
  const connectWs = useCycleStore(s => s.connectWs);
  const disconnectWs = useCycleStore(s => s.disconnectWs);
  const fetchCatalog = useCatalogStore(s => s.fetchCatalog);
  const hydrateLoadedProgram = useProgramStore(s => s.hydrateLoadedProgram);
  const fetchDeployment = useDeploymentStore(s => s.fetchDeployment);

  useEffect(() => {
    connectWs();
    void fetchCatalog();
    void hydrateLoadedProgram({
      retries: 6,
      retryDelayMs: 500,
    });
    void fetchDeployment();

    // Initialize composer store subscription
    composerUnsubscribe = initComposerStore();

    return () => {
      disconnectWs();
      composerUnsubscribe?.();
    };
  }, [connectWs, disconnectWs, fetchCatalog, hydrateLoadedProgram, fetchDeployment]);

  useEffect(() => {
    const handleBeforeUnload = (event: BeforeUnloadEvent) => {
      const programStore = useProgramStore.getState();
      const deploymentStore = useDeploymentStore.getState();

      const hasUnsavedProgram =
        JSON.stringify(programStore.flowToTree()) !== JSON.stringify(programStore.savedTree);
      const hasUnsavedDeployment =
        JSON.stringify(deploymentStore.deployment) !== JSON.stringify(deploymentStore.savedDeployment);

      if (!hasUnsavedProgram && !hasUnsavedDeployment) {
        return;
      }

      event.preventDefault();
      event.returnValue = '';
    };

    window.addEventListener('beforeunload', handleBeforeUnload);
    return () => window.removeEventListener('beforeunload', handleBeforeUnload);
  }, []);

  return <Layout />;
}
