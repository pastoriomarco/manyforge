import { useEffect } from 'react';
import { Layout } from './components/Layout';
import { useCycleStore } from './store/cycleStore';
import { useCatalogStore } from './store/catalogStore';
import { useProgramStore } from './store/programStore';
import { initComposerStore } from './store/composerStore';

let composerUnsubscribe: (() => void) | null = null;

export default function App() {
  const connectWs = useCycleStore(s => s.connectWs);
  const disconnectWs = useCycleStore(s => s.disconnectWs);
  const fetchCatalog = useCatalogStore(s => s.fetchCatalog);
  const hydrateLoadedProgram = useProgramStore(s => s.hydrateLoadedProgram);

  useEffect(() => {
    connectWs();
    void fetchCatalog();
    void hydrateLoadedProgram({
      retries: 6,
      retryDelayMs: 500,
    });

    // Initialize composer store subscription
    composerUnsubscribe = initComposerStore();

    return () => {
      disconnectWs();
      composerUnsubscribe?.();
    };
  }, [connectWs, disconnectWs, fetchCatalog, hydrateLoadedProgram]);

  return <Layout />;
}
