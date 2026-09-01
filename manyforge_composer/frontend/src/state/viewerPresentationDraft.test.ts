import { describe, expect, it } from 'vitest';
import {
  DEFAULT_VIEWER_PRESENTATION_DRAFT,
  programDraftPreviewActive,
  programViewerMetadata,
  viewerPresentationDraftReducer,
  viewerPresentationDraftFromProgram,
} from './viewerPresentationDraft';

describe('viewer presentation draft reducer', () => {
  it('never treats an unknown owner scene as an empty owner scene', () => {
    expect(programDraftPreviewActive(false, 0, 3)).toBe(false);
    expect(programDraftPreviewActive(true, 0, 3)).toBe(true);
    expect(programDraftPreviewActive(true, 1, 3)).toBe(false);
  });

  it('retains independent frame and object-axis visibility edits', () => {
    const frame = viewerPresentationDraftReducer(
      DEFAULT_VIEWER_PRESENTATION_DRAFT,
      { type: 'set_frame_visibility', frameId: 'tool0', visible: false },
    );
    const object = viewerPresentationDraftReducer(frame, {
      type: 'set_resource_visibility', resourceRoleId: 'workpiece', visible: true,
    });
    const axes = viewerPresentationDraftReducer(object, {
      type: 'set_scene_resource_axes_visibility',
      sceneResourceId: 'part',
      visible: true,
    });
    expect(axes.frameRoleVisibility).toEqual({ tool0: false });
    expect(axes.resourceRoleVisibility).toEqual({ workpiece: true });
    expect(axes.sceneResourceAxesVisibility).toEqual({ part: true });
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
      scene_resource_axes_visibility: { part: true },
      frame_gizmo_size_m: 0.2,
      object_axes_gizmo_size_m: 0.3,
    });

    expect(programViewerMetadata(draft)).toEqual({
      frame_role_visibility: { tcp: false },
      frame_gizmo_size_m: 0.2,
      resource_role_visibility: { workpiece: true },
      scene_resource_axes_visibility: { part: true },
      object_axes_gizmo_size_m: 0.3,
    });
  });
});
