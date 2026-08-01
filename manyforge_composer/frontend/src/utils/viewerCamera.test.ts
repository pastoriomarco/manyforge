import { describe, expect, it } from 'vitest';
import { DEFAULT_VIEWER_CAMERA, parseViewerCameraQuery } from './viewerCamera';

describe('viewer camera query', () => {
  it('returns null when no camera pin was requested', () => {
    expect(parseViewerCameraQuery('?tab=structure')).toBeNull();
  });

  it('parses an explicit camera and target', () => {
    expect(parseViewerCameraQuery('?cam=1,2,3&target=0.1,0.2,0.3')).toEqual({
      position: [1, 2, 3],
      target: [0.1, 0.2, 0.3],
    });
  });

  it('uses presentation defaults for a missing or malformed half', () => {
    expect(parseViewerCameraQuery('?cam=1,2,3')).toEqual({
      position: [1, 2, 3],
      target: DEFAULT_VIEWER_CAMERA.target,
    });
    expect(parseViewerCameraQuery('?cam=bad&target=0,0,1')).toEqual({
      position: DEFAULT_VIEWER_CAMERA.position,
      target: [0, 0, 1],
    });
  });
});
