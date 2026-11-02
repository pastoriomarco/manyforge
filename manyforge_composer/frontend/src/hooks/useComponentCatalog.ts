import { useCallback, useEffect, useState } from 'react';

import { api } from '../api/client';
import type { ComponentCatalog } from '../types/componentCatalog';

function errorMessage(error: unknown): string {
  return error instanceof Error && error.message.trim()
    ? error.message
    : 'Could not read the component catalog.';
}

/**
 * The curated component catalog, read once for the workbench (Components and
 * Deployment share it). Its model paths are relative to the Draft's project:
 * it is read again for every Draft opened (a new Draft id) and on `reload`.
 */
export function useComponentCatalog(enabled: boolean, draftId: string | null) {
  const [catalog, setCatalog] = useState<ComponentCatalog | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const reload = useCallback(() => setReloadToken((current) => current + 1), []);
  useEffect(() => {
    if (!enabled || draftId === null) return undefined;
    const controller = new AbortController();
    setError(null);
    void api.deploymentWorkspace.componentCatalog(controller.signal)
      .then((next) => { if (!controller.signal.aborted) setCatalog(next); })
      .catch((reason: unknown) => {
        if (!controller.signal.aborted) setError(errorMessage(reason));
      });
    return () => controller.abort();
  }, [enabled, draftId, reloadToken]);
  return { catalog, error, reload };
}

export type ComponentCatalogState = ReturnType<typeof useComponentCatalog>;
