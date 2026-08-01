export interface ViewerPresentationDraft {
  frameVisibility: Record<string, boolean>;
  objectFrameVisibility: Record<string, boolean>;
  frameGizmoSizeM: number;
  objectAxesGizmoSizeM: number;
}

export type ViewerPresentationDraftAction =
  | { type: 'set_frame_visibility'; frameId: string; visible: boolean }
  | { type: 'set_object_frame_visibility'; objectId: string; visible: boolean }
  | { type: 'set_frame_gizmo_size'; sizeM: number }
  | { type: 'set_object_axes_gizmo_size'; sizeM: number };

export const DEFAULT_VIEWER_PRESENTATION_DRAFT: ViewerPresentationDraft = {
  frameVisibility: {},
  objectFrameVisibility: {},
  frameGizmoSizeM: 0.1,
  objectAxesGizmoSizeM: 0.1,
};

function validGizmoSize(value: number): boolean {
  return Number.isFinite(value) && value >= 0.02 && value <= 1;
}

/**
 * Local viewer-authoring reducer extracted from the old mixed Scene controls.
 * I6 owns persistence in the qualified Program artifact; live state is never
 * mutated by this reducer.
 */
export function viewerPresentationDraftReducer(
  state: ViewerPresentationDraft,
  action: ViewerPresentationDraftAction,
): ViewerPresentationDraft {
  switch (action.type) {
    case 'set_frame_visibility':
      return {
        ...state,
        frameVisibility: {
          ...state.frameVisibility,
          [action.frameId]: action.visible,
        },
      };
    case 'set_object_frame_visibility':
      return {
        ...state,
        objectFrameVisibility: {
          ...state.objectFrameVisibility,
          [action.objectId]: action.visible,
        },
      };
    case 'set_frame_gizmo_size':
      return validGizmoSize(action.sizeM)
        ? { ...state, frameGizmoSizeM: action.sizeM }
        : state;
    case 'set_object_axes_gizmo_size':
      return validGizmoSize(action.sizeM)
        ? { ...state, objectAxesGizmoSizeM: action.sizeM }
        : state;
  }
}
