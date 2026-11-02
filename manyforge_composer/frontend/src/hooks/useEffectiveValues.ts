import { useEffect, useState } from 'react';

import { api } from '../api/client';
import type { EffectiveField, EffectiveValues } from '../types/components';
import type { DeploymentWorkspaceController } from './useDeploymentWorkspace';

/** The key of one Deployment field in `GET /api/deployment/effective`: its Draft path, dot-joined. */
export function effectivePathKey(path: ReadonlyArray<string | number>): string {
  return path.join('.');
}

export function effectiveField(
  effective: EffectiveValues | null,
  path: ReadonlyArray<string | number>,
): EffectiveField | null {
  return effective?.fields[effectivePathKey(path)] ?? null;
}

/**
 * Each Deployment field's effective value and default (CONTRACTS §11), read
 * again whenever the Draft changes. The backend is the only owner of defaults.
 */
export function useEffectiveValues(controller: DeploymentWorkspaceController): EffectiveValues | null {
  const draft = controller.workspace?.draft ?? null;
  const key = draft === null ? null : `${draft.id}:${draft.revision}`;
  const [effective, setEffective] = useState<EffectiveValues | null>(null);
  useEffect(() => {
    if (key === null) return;
    const abort = new AbortController();
    api.cellApply.effective(abort.signal)
      .then((values) => { if (!abort.signal.aborted) setEffective(values); })
      .catch(() => { if (!abort.signal.aborted) setEffective(null); });
    return () => abort.abort();
  }, [key]);
  return key === null ? null : effective;
}
