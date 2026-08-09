import { describe, expect, it } from 'vitest';
import {
  DEFAULT_VIEWER_PRESENTATION_DRAFT,
  programViewerMetadata,
  viewerPresentationDraftReducer,
  viewerPresentationDraftFromProgram,
} from './viewerPresentationDraft';

describe('viewer presentation draft reducer', () => {
  it('retains independent frame and object-axis visibility edits', () => {
    const frame = viewerPresentationDraftReducer(
      DEFAULT_VIEWER_PRESENTATION_DRAFT,
      { type: 'set_frame_visibility', frameId: 'tool0', visible: false },
    );
    const object = viewerPresentationDraftReducer(frame, {
      type: 'set_resource_visibility', resourceRoleId: 'workpiece', visible: true,
    });
    expect(object.frameRoleVisibility).toEqual({ tool0: false });
    expect(object.resourceRoleVisibility).toEqual({ workpiece: true });
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

  it('maps exactly to the Program viewer contract', () => {
    const draft = viewerPresentationDraftFromProgram({
      frame_role_visibility: { tcp: false },
      resource_role_visibility: { workpiece: true },
      frame_gizmo_size_m: 0.2,
      object_axes_gizmo_size_m: 0.3,
    });

    expect(programViewerMetadata(draft)).toEqual({
      frame_role_visibility: { tcp: false },
      frame_gizmo_size_m: 0.2,
      resource_role_visibility: { workpiece: true },
      object_axes_gizmo_size_m: 0.3,
    });
  });
});
