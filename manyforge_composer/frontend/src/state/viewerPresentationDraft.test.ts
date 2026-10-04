import { describe, expect, it } from 'vitest';
import {
  DEFAULT_VIEWER_PRESENTATION_DRAFT,
  sceneSourceIsDraft,
  programViewerMetadata,
  viewerPresentationDraftReducer,
  viewerPresentationDraftFromProgram,
} from './viewerPresentationDraft';

describe('viewer presentation draft reducer', () => {
  it('shows the Draft Scene only while it differs from Applied, with a pure view override', () => {
    // Unapplied changes: the Draft Scene; no difference: the runtime Scene.
    expect(sceneSourceIsDraft('auto', true, true, true, true)).toBe(true);
    expect(sceneSourceIsDraft('auto', true, true, true, false)).toBe(false);
    // The manual override is a pure view choice either way.
    expect(sceneSourceIsDraft('applied', true, true, true, true)).toBe(false);
    expect(sceneSourceIsDraft('draft', true, true, true, false)).toBe(true);
    // A Draft that cannot be edited (for example while running) is not shown.
    expect(sceneSourceIsDraft('draft', true, true, false, true)).toBe(false);
    // No Draft, or an unknown owner scene, never flashes the Draft presentation.
    expect(sceneSourceIsDraft('auto', true, false, true, true)).toBe(false);
    expect(sceneSourceIsDraft('auto', false, true, true, true)).toBe(false);
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

describe('Program viewer settings round trip', () => {
  it('keeps the hidden execution groups when other viewer settings change', () => {
    const draft = viewerPresentationDraftFromProgram({
      frame_gizmo_size_m: 0.1,
      hidden_execution_groups: ['all', 'robot_b'],
    });
    expect(programViewerMetadata({ ...draft, frameGizmoSizeM: 0.2 })).toMatchObject({
      frame_gizmo_size_m: 0.2,
      hidden_execution_groups: ['all', 'robot_b'],
    });
    // No choice stays no choice (the default rule), not an empty list.
    expect(programViewerMetadata(viewerPresentationDraftFromProgram({})))
      .not.toHaveProperty('hidden_execution_groups');
    // Written canonical: sorted and unique, as the Program requires.
    expect(programViewerMetadata({ ...draft, hiddenExecutionGroups: ['robot_b', 'all', 'robot_b'] })
      .hidden_execution_groups).toEqual(['all', 'robot_b']);
  });
});
