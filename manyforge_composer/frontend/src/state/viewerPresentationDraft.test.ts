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
      { type: 'set_frame_visibility', frame: { component_id: 'robot', frame_id: 'tool0' }, visible: false },
    );
    const object = viewerPresentationDraftReducer(frame, {
      type: 'set_resource_visibility', sceneResourceId: 'workpiece', visible: true,
    });
    const axes = viewerPresentationDraftReducer(object, {
      type: 'set_scene_resource_axes_visibility',
      sceneResourceId: 'part',
      visible: true,
    });
    expect(axes.frameVisibility).toEqual([{ frame: { component_id: 'robot', frame_id: 'tool0' }, visible: false }]);
    expect(axes.sceneResourceVisibility).toEqual({ workpiece: true });
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
      frame_visibility: [{ frame: { component_id: 'robot', frame_id: 'tcp' }, visible: false }],
      cell_frame_visible: false,
      scene_resource_visibility: { workpiece: true },
      scene_resource_axes_visibility: { part: true },
      frame_gizmo_size_m: 0.2,
      object_axes_gizmo_size_m: 0.3,
    });

    expect(programViewerMetadata(draft)).toEqual({
      frame_visibility: [{ frame: { component_id: 'robot', frame_id: 'tcp' }, visible: false }],
      cell_frame_visible: false,
      frame_gizmo_size_m: 0.2,
      scene_resource_visibility: { workpiece: true },
      scene_resource_axes_visibility: { part: true },
      object_axes_gizmo_size_m: 0.3,
    });
  });

  it('keeps equal frame IDs on different components independent and ordered', () => {
    let draft = viewerPresentationDraftReducer(DEFAULT_VIEWER_PRESENTATION_DRAFT,
      { type: 'set_frame_visibility', frame: { component_id: 'robot_b', frame_id: 'base' }, visible: false });
    draft = viewerPresentationDraftReducer(draft,
      { type: 'set_frame_visibility', frame: { component_id: 'robot_a', frame_id: 'base' }, visible: true });
    draft = viewerPresentationDraftReducer(draft,
      { type: 'set_frame_visibility', frame: { component_id: 'robot_b', frame_id: 'base' }, visible: true });
    expect(programViewerMetadata(draft).frame_visibility).toEqual([
      { frame: { component_id: 'robot_a', frame_id: 'base' }, visible: true },
      { frame: { component_id: 'robot_b', frame_id: 'base' }, visible: true },
    ]);
    expect(draft.cellFrameVisible).toBe(true);
    expect(viewerPresentationDraftReducer(draft, { type: 'set_cell_frame_visibility', visible: false }).cellFrameVisible).toBe(false);
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
