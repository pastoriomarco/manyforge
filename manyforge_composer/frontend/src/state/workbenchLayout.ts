export const VIEWER_WIDTH_MIN = 420;
export const AUTHORING_WIDTH_MIN = 360;
export const INSPECTOR_WIDTH_MIN = 260;
export const INSPECTOR_WIDTH_MAX = 480;
export const WORKBENCH_DIVIDER_WIDTH = 8;
export const VIEWER_WIDTH_MAX_FRACTION = 2 / 3;

export interface WorkbenchWidthLimits {
  viewerWidthMin: number;
  viewerWidthMax: number;
  inspectorWidthMax: number;
}

/** Preserve usable authoring/inspector widths while exposing the accepted viewer range. */
export function workbenchWidthLimits(
  workbenchWidth: number,
  inspectorWidth: number,
): WorkbenchWidthLimits {
  const viewerWidthMax = Math.max(
    VIEWER_WIDTH_MIN,
    Math.min(
      Math.floor(workbenchWidth * VIEWER_WIDTH_MAX_FRACTION),
      workbenchWidth - AUTHORING_WIDTH_MIN - inspectorWidth
        - 2 * WORKBENCH_DIVIDER_WIDTH,
    ),
  );
  const viewerWidthMin = Math.min(VIEWER_WIDTH_MIN, viewerWidthMax);
  const inspectorWidthMax = Math.max(
    INSPECTOR_WIDTH_MIN,
    Math.min(
      INSPECTOR_WIDTH_MAX,
      workbenchWidth - AUTHORING_WIDTH_MIN - viewerWidthMin
        - 2 * WORKBENCH_DIVIDER_WIDTH,
    ),
  );
  return { viewerWidthMin, viewerWidthMax, inspectorWidthMax };
}
