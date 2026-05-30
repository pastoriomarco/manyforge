import type { CandidateCreateRequest } from '../types/api';
import { relativePose } from './poseTransforms';
import type { PoseTuple } from './poseTransforms';

/**
 * Build the Cartesian candidate request for a gizmo drag release, expressed in
 * the robot BASE frame. The anchor mesh lives in WORLD space, so converting to
 * the base frame requires the base link's world pose. When that pose is unknown
 * we cannot produce a correct base-frame target, so return null and let the
 * caller fail closed rather than author a mis-framed candidate.
 */
export function gizmoCartesianRequest(
  anchorWorld: PoseTuple,
  baseWorldPose: PoseTuple | null,
  tcpFrame: string | null,
): CandidateCreateRequest | null {
  if (baseWorldPose === null) return null;
  const tcpInBase = relativePose(anchorWorld, baseWorldPose);
  return {
    sourceSurface: 'cartesian_gizmo',
    inputMode: 'cartesian',
    cartesianTarget: {
      positionM: [...tcpInBase.position],
      orientationXyzw: [...tcpInBase.quaternion],
      frameId: 'base',
      tcpFrame: tcpFrame ?? undefined,
    },
  };
}

export interface GizmoReleaseActions {
  author: (request: CandidateCreateRequest) => void;
  rejectDraft: (detail: string) => void;
}

/**
 * Decide what a gizmo drag release does. When the target frame can be resolved
 * it authors a validated candidate; otherwise it fails closed — it does NOT
 * author, and instead calls rejectDraft so any prior (e.g. ready) draft becomes
 * non-approvable. Never moves the robot either way.
 */
export function authorGizmoRelease(
  anchorWorld: PoseTuple,
  baseWorldPose: PoseTuple | null,
  tcpFrame: string | null,
  actions: GizmoReleaseActions,
): void {
  const request = gizmoCartesianRequest(anchorWorld, baseWorldPose, tcpFrame);
  if (request === null) {
    actions.rejectDraft(
      'Cannot author target: robot base frame pose is unavailable.',
    );
    return;
  }
  actions.author(request);
}
