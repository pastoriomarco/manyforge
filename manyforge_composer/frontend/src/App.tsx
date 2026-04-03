import { useEffect } from 'react';
import { Layout } from './components/Layout';
import { useCycleStore } from './store/cycleStore';
import { useCatalogStore } from './store/catalogStore';
import { initComposerStore } from './store/composerStore';

let composerUnsubscribe: (() => void) | null = null;

export default function App() {
  const connectWs = useCycleStore(s => s.connectWs);
  const disconnectWs = useCycleStore(s => s.disconnectWs);
  const fetchCatalog = useCatalogStore(s => s.fetchCatalog);

  useEffect(() => {
    connectWs();
    fetchCatalog();
    
    // Initialize composer store subscription
    composerUnsubscribe = initComposerStore();
    
    return () => {
      disconnectWs();
      composerUnsubscribe?.();
    };
  }, [connectWs, disconnectWs, fetchCatalog]);

  return <Layout />;
}
