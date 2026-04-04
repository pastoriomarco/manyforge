import { useEffect } from 'react';
import { Layout } from './components/Layout';
import { useCycleStore } from './store/cycleStore';
import { useCatalogStore } from './store/catalogStore';
import { useProgramStore } from './store/programStore';
import { initComposerStore } from './store/composerStore';
import { api } from './api/client';

let composerUnsubscribe: (() => void) | null = null;

export default function App() {
  const connectWs = useCycleStore(s => s.connectWs);
  const disconnectWs = useCycleStore(s => s.disconnectWs);
  const fetchCatalog = useCatalogStore(s => s.fetchCatalog);
  const fetchTree = useProgramStore(s => s.fetchTree);

  useEffect(() => {
    connectWs();
    fetchCatalog();

    // If a program was already loaded (e.g. via curl/demo script),
    // fetch the tree so the canvas renders it.
    // Retry once after 2s in case the program load hasn't completed yet.
    const tryFetchProgram = () => {
      api.program.get().then((info) => {
        useProgramStore.setState({ programInfo: info });
        fetchTree();
      }).catch(() => {
        // No program loaded yet — retry once after a short delay
        setTimeout(() => {
          api.program.get().then((info) => {
            useProgramStore.setState({ programInfo: info });
            fetchTree();
          }).catch(() => {});
        }, 2000);
      });
    };
    tryFetchProgram();

    // Initialize composer store subscription
    composerUnsubscribe = initComposerStore();

    return () => {
      disconnectWs();
      composerUnsubscribe?.();
    };
  }, [connectWs, disconnectWs, fetchCatalog, fetchTree]);

  return <Layout />;
}
