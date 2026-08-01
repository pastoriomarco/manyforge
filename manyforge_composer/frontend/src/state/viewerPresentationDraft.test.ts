import { describe, expect, it } from 'vitest';
import {
  DEFAULT_VIEWER_PRESENTATION_DRAFT,
  viewerPresentationDraftReducer,
} from './viewerPresentationDraft';

describe('viewer presentation draft reducer', () => {
  it('retains independent frame and object-axis visibility edits', () => {
    const frame = viewerPresentationDraftReducer(
      DEFAULT_VIEWER_PRESENTATION_DRAFT,
      { type: 'set_frame_visibility', frameId: 'tool0', visible: false },
    );
    const object = viewerPresentationDraftReducer(frame, {
      type: 'set_object_frame_visibility', objectId: 'part-1', visible: true,
    });
    expect(object.frameVisibility).toEqual({ tool0: false });
    expect(object.objectFrameVisibility).toEqual({ 'part-1': true });
  });

  it('bounds gizmo sizes as local presentation values', () => {
    const resized = viewerPresentationDraftReducer(
      DEFAULT_VIEWER_PRESENTATION_DRAFT,
      { type: 'set_frame_gizmo_size', sizeM: 0.25 },
    );
    expect(resized.frameGizmoSizeM).toBe(0.25);
    expect(viewerPresentationDraftReducer(resized, {
      type: 'set_frame_gizmo_size', sizeM: 10,
    })).toBe(resized);
  });
});
