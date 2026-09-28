import type { ProgramViewerMetadata } from '../types/api';

export interface ViewerPresentationDraft {
  frameRoleVisibility: Record<string, boolean>;
  resourceRoleVisibility: Record<string, boolean>;
  sceneResourceAxesVisibility: Record<string, boolean>;
  frameGizmoSizeM: number;
  objectAxesGizmoSizeM: number;
}

export type ViewerPresentationDraftAction =
  | { type: 'set_frame_visibility'; frameId: string; visible: boolean }
  | { type: 'set_resource_visibility'; resourceRoleId: string; visible: boolean }
  | { type: 'set_scene_resource_axes_visibility'; sceneResourceId: string; visible: boolean }
  | { type: 'set_frame_gizmo_size'; sizeM: number }
  | { type: 'set_object_axes_gizmo_size'; sizeM: number };

export const DEFAULT_VIEWER_PRESENTATION_DRAFT: ViewerPresentationDraft = {
  frameRoleVisibility: {},
  resourceRoleVisibility: {},
  sceneResourceAxesVisibility: {},
  frameGizmoSizeM: 0.1,
  objectAxesGizmoSizeM: 0.1,
};

/**
 * Draft geometry is a deliberate preview only when a current owner projection
 * explicitly reports an empty scene. Startup/transport unknown is not an empty
 * owner scene and must never flash the Draft presentation.
 */
export function programDraftPreviewActive(
  ownerSceneKnown: boolean,
  ownerObjectCount: number,
  draftObjectCount: number,
): boolean {
  return ownerSceneKnown && ownerObjectCount === 0 && draftObjectCount > 0;
}

export function sceneSourceIsDraft(
  choice: 'auto' | 'draft' | 'applied',
  ownerSceneKnown: boolean,
  draftAvailable: boolean,
  draftDiffersFromApplied: boolean,
  emptyOwnerPreview: boolean,
): boolean {
  // An explicit Draft choice applies only while Draft and Applied differ,
  // which is also the only time its toggle is shown.
  return ownerSceneKnown && draftAvailable
    && ((choice === 'draft' && draftDiffersFromApplied) || (choice === 'auto'
      && (draftDiffersFromApplied || emptyOwnerPreview)));
}

function orderedVisibility(
  visibility: Record<string, boolean>,
): Record<string, boolean> {
  return Object.fromEntries(
    Object.entries(visibility).sort(([left], [right]) => left.localeCompare(right)),
  );
}

export function viewerPresentationDraftFromProgram(
  viewer: ProgramViewerMetadata | undefined,
): ViewerPresentationDraft {
  return {
    frameRoleVisibility: { ...(viewer?.frame_role_visibility ?? {}) },
    resourceRoleVisibility: { ...(viewer?.resource_role_visibility ?? {}) },
    sceneResourceAxesVisibility: {
      ...(viewer?.scene_resource_axes_visibility ?? {}),
    },
    frameGizmoSizeM: viewer?.frame_gizmo_size_m ?? 0.1,
    objectAxesGizmoSizeM: viewer?.object_axes_gizmo_size_m ?? 0.1,
  };
}

export function programViewerMetadata(
  draft: ViewerPresentationDraft,
): ProgramViewerMetadata {
  return {
    frame_role_visibility: orderedVisibility(draft.frameRoleVisibility),
    frame_gizmo_size_m: draft.frameGizmoSizeM,
    resource_role_visibility: orderedVisibility(draft.resourceRoleVisibility),
    scene_resource_axes_visibility: orderedVisibility(
      draft.sceneResourceAxesVisibility,
    ),
    object_axes_gizmo_size_m: draft.objectAxesGizmoSizeM,
  };
}

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
        frameRoleVisibility: {
          ...state.frameRoleVisibility,
          [action.frameId]: action.visible,
        },
      };
    case 'set_resource_visibility':
      return {
        ...state,
        resourceRoleVisibility: {
          ...state.resourceRoleVisibility,
          [action.resourceRoleId]: action.visible,
        },
      };
    case 'set_scene_resource_axes_visibility':
      return {
        ...state,
        sceneResourceAxesVisibility: {
          ...state.sceneResourceAxesVisibility,
          [action.sceneResourceId]: action.visible,
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
