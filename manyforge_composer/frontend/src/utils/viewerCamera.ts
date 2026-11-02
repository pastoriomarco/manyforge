export interface ViewerCamera {
  position: [number, number, number];
  target: [number, number, number];
}

export const DEFAULT_VIEWER_CAMERA: ViewerCamera = {
  position: [2.3, -2.3, 1.7],
  target: [0, 0, 0.45],
};

function parseTriple(raw: string | null): [number, number, number] | null {
  if (raw === null) return null;
  const parts = raw.split(',').map((part) => Number(part.trim()));
  if (parts.length !== 3 || parts.some((part) => !Number.isFinite(part))) {
    return null;
  }
  return [parts[0], parts[1], parts[2]];
}

/** Parse a shareable local camera pin without creating a mixed viewer store. */
export function parseViewerCameraQuery(search: string): ViewerCamera | null {
  const params = new URLSearchParams(search);
  const positionRaw = params.get('cam');
  const targetRaw = params.get('target');
  if (positionRaw === null && targetRaw === null) return null;
  return {
    position: parseTriple(positionRaw) ?? [...DEFAULT_VIEWER_CAMERA.position],
    target: parseTriple(targetRaw) ?? [...DEFAULT_VIEWER_CAMERA.target],
  };
}
