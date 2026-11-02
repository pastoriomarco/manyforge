import type { WorkbenchNotice } from './workbenchNotices';

export function initialSceneNotice(
  failure: { state: 'failed' | 'unknown'; reason: { code: string; message: string } } | null,
  openResetReload: () => void,
): WorkbenchNotice | null {
  if (failure === null) return null;
  return {
    id: 'initial-scene-not-loaded', severity: 'error', domain: 'activation',
    title: failure.state === 'unknown' ? 'Initial scene may be incomplete' : 'Initial scene not loaded',
    message: failure.reason.message,
    actions: [{ id: 'reset-reload-scene', label: 'Reset → reload initial scene', run: openResetReload }],
  };
}
