import { Euler, Quaternion, Vector3 } from 'three';
import type { PoseTuple } from './poseTransforms';

export type MoveToOrientationMode = 'keep-current' | 'face-down' | 'use-entity';

export interface CartesianMoveDraftOptions {
  orientationMode: MoveToOrientationMode;
  offsetXyzM: [number, number, number];
  offsetRpyRad: [number, number, number];
}

function finitePose(pose: PoseTuple | null): pose is PoseTuple {
  return pose !== null
    && pose.position.every(Number.isFinite)
    && pose.quaternion.every(Number.isFinite)
    && new Quaternion(...pose.quaternion).lengthSq() > 1e-12;
}

function selectedOrientation(
  mode: MoveToOrientationMode,
  entityPose: PoseTuple,
  currentTcpPose: PoseTuple | null,
): Quaternion | null {
  if (mode === 'use-entity') return new Quaternion(...entityPose.quaternion).normalize();
  if (mode === 'face-down') return new Quaternion(0, 0, 0, 1);
  return finitePose(currentTcpPose)
    ? new Quaternion(...currentTcpPose.quaternion).normalize()
    : null;
}

/**
 * Compute a local Cartesian authoring draft from a resolved entity pose. I6
 * still owns entity/target resolution and command-schema binding; mixed joint
 * targets are deliberately excluded until their unit-tagged contract lands.
 */
export function computeCartesianMoveDraft(
  entityPose: PoseTuple,
  currentTcpPose: PoseTuple | null,
  options: CartesianMoveDraftOptions,
): PoseTuple | null {
  if (!finitePose(entityPose)) return null;
  if (
    !options.offsetXyzM.every(Number.isFinite)
    || !options.offsetRpyRad.every(Number.isFinite)
  ) {
    return null;
  }
  const orientation = selectedOrientation(
    options.orientationMode,
    entityPose,
    currentTcpPose,
  );
  if (orientation === null) return null;

  const [roll, pitch, yaw] = options.offsetRpyRad;
  const delta = new Quaternion().setFromEuler(
    new Euler(roll, pitch, yaw, 'XYZ'),
  );
  orientation.multiply(delta).normalize();
  const position = new Vector3(...entityPose.position).add(
    new Vector3(...options.offsetXyzM).applyQuaternion(orientation),
  );
  return {
    position: [position.x, position.y, position.z],
    quaternion: [orientation.x, orientation.y, orientation.z, orientation.w],
  };
}
