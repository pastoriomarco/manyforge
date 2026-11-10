import type { ProgramFrameReference, ProgramViewerMetadata } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

export interface ViewerPresentationDraft {
  frameVisibility: { frame: ProgramFrameReference; visible: boolean }[];
  cellFrameVisible: boolean;
  sceneResourceVisibility: Record<string, boolean>;
  sceneResourceAxesVisibility: Record<string, boolean>;
  frameGizmoSizeM: number;
  objectAxesGizmoSizeM: number;
  /** Execution groups the Motion panel hides; null keeps the default. */
  hiddenExecutionGroups: string[] | null;
}

export type ViewerPresentationDraftAction =
  | { type: 'set_frame_visibility'; frame: ProgramFrameReference; visible: boolean }
  | { type: 'set_cell_frame_visibility'; visible: boolean }
  | { type: 'set_resource_visibility'; sceneResourceId: string; visible: boolean }
  | { type: 'set_scene_resource_axes_visibility'; sceneResourceId: string; visible: boolean }
  | { type: 'set_frame_gizmo_size'; sizeM: number }
  | { type: 'set_object_axes_gizmo_size'; sizeM: number };

export const DEFAULT_VIEWER_PRESENTATION_DRAFT: ViewerPresentationDraft = {
  frameVisibility: [],
  cellFrameVisible: true,
  sceneResourceVisibility: {},
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

/**
 * Whether Program execution locks the viewer's object editing: any run,
 * except one whose Program motion the owners confirm stopped (the published
 * `heldRunDraftEditAllowed`, the rule every other Draft edit follows).
 */
export function executionLocksObjectEditing(
  snapshot: ComposerHeadlessState | null,
): boolean {
  return snapshot !== null
    && snapshot.programCycle.state !== 'idle_after_reset'
    && snapshot.heldRunDraftEditAllowed !== true;
}

function orderedVisibility(
  visibility: Record<string, boolean>,
): Record<string, boolean> {
  return Object.fromEntries(
    Object.entries(visibility).sort(([left], [right]) => left < right ? -1 : left > right ? 1 : 0),
  );
}

function compareFrames(left: ProgramFrameReference, right: ProgramFrameReference): number {
  if (left.component_id !== right.component_id) return left.component_id < right.component_id ? -1 : 1;
  if (left.frame_id !== right.frame_id) return left.frame_id < right.frame_id ? -1 : 1;
  return 0;
}

export function viewerPresentationDraftFromProgram(
  viewer: ProgramViewerMetadata | undefined,
): ViewerPresentationDraft {
  return {
    frameVisibility: (viewer?.frame_visibility ?? []).map(({ frame, visible }) => ({ frame: { ...frame }, visible })),
    cellFrameVisible: viewer?.cell_frame_visible ?? true,
    sceneResourceVisibility: { ...(viewer?.scene_resource_visibility ?? {}) },
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
    frame_visibility: draft.frameVisibility.map(({ frame, visible }) => ({ frame: { ...frame }, visible }))
      .sort((left, right) => compareFrames(left.frame, right.frame)),
    cell_frame_visible: draft.cellFrameVisible,
    frame_gizmo_size_m: draft.frameGizmoSizeM,
    scene_resource_visibility: orderedVisibility(draft.sceneResourceVisibility),
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
 * Persistence belongs to the qualified Program artifact; live state is never
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
        frameVisibility: [
          ...state.frameVisibility.filter(({ frame }) => compareFrames(frame, action.frame) !== 0),
          { frame: { ...action.frame }, visible: action.visible },
        ].sort((left, right) => compareFrames(left.frame, right.frame)),
      };
    case 'set_cell_frame_visibility':
      return { ...state, cellFrameVisible: action.visible };
    case 'set_resource_visibility':
      return {
        ...state,
        sceneResourceVisibility: {
          ...state.sceneResourceVisibility,
          [action.sceneResourceId]: action.visible,
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
