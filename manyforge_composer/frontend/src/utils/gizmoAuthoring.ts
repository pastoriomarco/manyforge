import { relativePose, type PoseTuple } from './poseTransforms';

export interface GizmoTargetAddress {
  componentId: string;
  targetId: string;
  referenceFrame: string;
}

export interface GizmoPoseDraft {
  source: 'cartesian_gizmo';
  address: GizmoTargetAddress;
  pose: {
    relative_to: string;
    position_m: [number, number, number];
    orientation_xyzw: [number, number, number, number];
  };
}

function completeAddress(address: GizmoTargetAddress): boolean {
  return address.componentId.trim().length > 0
    && address.targetId.trim().length > 0
    && address.referenceFrame.trim().length > 0;
}

/**
 * Convert a world-space gizmo anchor into a local, addressed authoring draft.
 * This is deliberately not an API command or candidate DTO: I6 must bind the
 * draft to its qualified Program-Binding and command-schema contracts.
 */
export function gizmoPoseDraft(
  anchorWorld: PoseTuple,
  referenceWorld: PoseTuple | null,
  address: GizmoTargetAddress,
): GizmoPoseDraft | null {
  if (referenceWorld === null || !completeAddress(address)) return null;
  const targetInReference = relativePose(anchorWorld, referenceWorld);
  return {
    source: 'cartesian_gizmo',
    address: {
      componentId: address.componentId.trim(),
      targetId: address.targetId.trim(),
      referenceFrame: address.referenceFrame.trim(),
    },
    pose: {
      relative_to: address.referenceFrame.trim(),
      position_m: [...targetInReference.position],
      orientation_xyzw: [...targetInReference.quaternion],
    },
  };
}

export interface GizmoReleaseActions {
  retainDraft: (draft: GizmoPoseDraft) => void;
  rejectDraft: (detail: string) => void;
}

export function retainGizmoRelease(
  anchorWorld: PoseTuple,
  referenceWorld: PoseTuple | null,
  address: GizmoTargetAddress,
  actions: GizmoReleaseActions,
): void {
  const draft = gizmoPoseDraft(anchorWorld, referenceWorld, address);
  if (draft === null) {
    actions.rejectDraft(
      'Cannot retain target: its component, target, or reference frame is unavailable.',
    );
    return;
  }
  actions.retainDraft(draft);
}
