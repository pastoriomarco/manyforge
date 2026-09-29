/**
 * Opens a surface's own settings without sharing their state: each surface
 * listens for its own name and opens its panel. "program" is the Program
 * settings, which include how the tree editor draws the Program.
 */
export type SettingsSurface = 'viewer' | 'program';

const OPEN_SETTINGS_EVENT = 'manyforge:open-settings';

export function requestOpenSettings(surface: SettingsSurface): void {
  globalThis.dispatchEvent?.(new CustomEvent(OPEN_SETTINGS_EVENT, { detail: surface }));
}

/** Subscribe; returns the unsubscribe function (for a `useEffect`). */
export function onOpenSettingsRequest(surface: SettingsSurface, open: () => void): () => void {
  const handler = (event: Event) => {
    if ((event as CustomEvent<SettingsSurface>).detail === surface) open();
  };
  globalThis.addEventListener?.(OPEN_SETTINGS_EVENT, handler);
  return () => globalThis.removeEventListener?.(OPEN_SETTINGS_EVENT, handler);
}
