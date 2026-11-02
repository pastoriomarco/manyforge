export const SIDE_PANE_WIDTH_MIN = 360;
export const INSPECTOR_WIDTH_MIN = 260;
export const INSPECTOR_WIDTH_MAX = 480;
export const WORKBENCH_DIVIDER_WIDTH = 8;

/**
 * The central inspector stays centered on screen, so the editor and the viewer
 * always share the remaining width equally; only the inspector is resizable.
 */
export function inspectorWidthMax(workbenchWidth: number): number {
  return Math.max(
    INSPECTOR_WIDTH_MIN,
    Math.min(
      INSPECTOR_WIDTH_MAX,
      workbenchWidth - 2 * SIDE_PANE_WIDTH_MIN - 2 * WORKBENCH_DIVIDER_WIDTH,
    ),
  );
}
