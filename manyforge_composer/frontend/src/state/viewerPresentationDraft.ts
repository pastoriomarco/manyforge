import type { ProgramViewerMetadata } from '../types/api';

export interface ViewerPresentationDraft {
  frameRoleVisibility: Record<string, boolean>;
  resourceRoleVisibility: Record<string, boolean>;
  sceneResourceAxesVisibility: Record<string, boolean>;
  frameGizmoSizeM: number;
  objectAxesGizmoSizeM: number;
  /** Execution groups the Motion panel hides; null keeps the default. */
  hiddenExecutionGroups: string[] | null;
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
  hiddenExecutionGroups: null,
};

/**
 * The viewer presents the Draft Scene while it is editable and differs from
 * Applied; with no difference the runtime Scene is shown (and edited through
 * the identical Draft copy). An explicit choice is a pure view override. An
 * unknown owner scene never flashes the Draft presentation.
 */
export function sceneSourceIsDraft(
  choice: 'auto' | 'draft' | 'applied',
  ownerSceneKnown: boolean,
  draftAvailable: boolean,
  draftEditable: boolean,
  draftDiffersFromApplied: boolean,
): boolean {
  return ownerSceneKnown && draftAvailable && draftEditable
    && (choice === 'draft' || (choice === 'auto' && draftDiffersFromApplied));
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
    hiddenExecutionGroups: viewer?.hidden_execution_groups === undefined
      ? null
      : [...viewer.hidden_execution_groups],
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
    ...(draft.hiddenExecutionGroups === null ? {} : {
      hidden_execution_groups: [...new Set(draft.hiddenExecutionGroups)].sort(),
    }),
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
