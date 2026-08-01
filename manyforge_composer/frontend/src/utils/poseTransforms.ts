import { Matrix4, Quaternion, Vector3 } from 'three';

export interface PoseTuple {
  position: [number, number, number];
  quaternion: [number, number, number, number];
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
