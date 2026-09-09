import { Matrix4, Quaternion, Vector3 } from 'three';
import type { Pose3Value } from '../types/robotControl';

export interface PoseTuple {
  position: [number, number, number];
  quaternion: [number, number, number, number];
}

export function poseTupleFromValue(pose: Pose3Value): PoseTuple {
  return {
    position: [pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m],
    quaternion: [pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w],
  };
}

export function poseValueFromTuple(pose: PoseTuple): Pose3Value {
  return {
    position_m: { x_m: pose.position[0], y_m: pose.position[1], z_m: pose.position[2] },
    orientation: { x: pose.quaternion[0], y: pose.quaternion[1], z: pose.quaternion[2], w: pose.quaternion[3] },
  };
}

/** Transform round-off and q/-q do not constitute an authoring edit. */
export function samePose(left: PoseTuple, right: PoseTuple): boolean {
  return new Vector3(...left.position).distanceToSquared(new Vector3(...right.position)) < 1e-18
    && 1 - Math.abs(new Quaternion(...left.quaternion).normalize()
      .dot(new Quaternion(...right.quaternion).normalize())) < 1e-14;
}

function finiteTuple(values: unknown, length: number): number[] | null {
  if (!Array.isArray(values) || values.length !== length) return null;
  const tuple = values.map((value) => Number(value));
  return tuple.every(Number.isFinite) ? tuple : null;
}

/** Parse the canonical metre/xyzw pose shape used by retained projections. */
export function poseFromRecord(
  pose: Record<string, unknown> | null | undefined,
): PoseTuple | null {
  const position = finiteTuple(pose?.position_m, 3);
  const quaternion = finiteTuple(pose?.orientation_xyzw, 4);
  if (position === null || quaternion === null) return null;

  const normalized = new Quaternion(
    quaternion[0],
    quaternion[1],
    quaternion[2],
    quaternion[3],
  );
  if (normalized.lengthSq() < 1e-12) return null;
  normalized.normalize();
  return {
    position: position as [number, number, number],
    quaternion: [normalized.x, normalized.y, normalized.z, normalized.w],
  };
}

function matrixFromPose(pose: PoseTuple): Matrix4 {
  return new Matrix4().compose(
    new Vector3(...pose.position),
    new Quaternion(...pose.quaternion),
    new Vector3(1, 1, 1),
  );
}

function poseFromMatrix(matrix: Matrix4): PoseTuple {
  const position = new Vector3();
  const quaternion = new Quaternion();
  const scale = new Vector3();
  matrix.decompose(position, quaternion, scale);
  quaternion.normalize();
  return {
    position: [position.x, position.y, position.z],
    quaternion: [quaternion.x, quaternion.y, quaternion.z, quaternion.w],
  };
}

export function composePose(
  parentWorld: PoseTuple,
  childInParent: PoseTuple,
): PoseTuple {
  return poseFromMatrix(
    matrixFromPose(parentWorld).multiply(matrixFromPose(childInParent)),
  );
}

export function relativePose(
  childWorld: PoseTuple,
  parentWorld: PoseTuple,
): PoseTuple {
  return poseFromMatrix(
    matrixFromPose(parentWorld).invert().multiply(matrixFromPose(childWorld)),
  );
}
