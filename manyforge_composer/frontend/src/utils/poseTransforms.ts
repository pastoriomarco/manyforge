import { Euler, Matrix4, Quaternion, Vector3 } from 'three';

export interface PoseTuple {
  position: [number, number, number];
  quaternion: [number, number, number, number];
}

function finiteTuple(values: unknown, length: number): number[] | null {
  if (!Array.isArray(values) || values.length !== length) return null;
  const out = values.map((value) => Number(value));
  return out.every(Number.isFinite) ? out : null;
}

export function poseFromRecord(pose: Record<string, unknown> | null | undefined): PoseTuple | null {
  const position = finiteTuple(pose?.position_m, 3);
  const quaternion = finiteTuple(pose?.orientation_xyzw, 4);
  if (position === null || quaternion === null) return null;
  const q = new Quaternion(
    quaternion[0],
    quaternion[1],
    quaternion[2],
    quaternion[3],
  );
  if (q.lengthSq() < 1e-12) return null;
  q.normalize();
  return {
    position: position as [number, number, number],
    quaternion: [q.x, q.y, q.z, q.w],
  };
}

export function poseFromBasePoseRecord(
  pose: Record<string, unknown> | null | undefined,
): PoseTuple | null {
  const direct = poseFromRecord(pose);
  if (direct !== null) return direct;
  const position = finiteTuple(pose?.position_m, 3);
  if (position === null) return null;
  const rpyDeg =
    finiteTuple(pose?.orientation_rpy_deg, 3)
    ?? finiteTuple(pose?.orientationRPYDeg, 3);
  const rpyRad =
    finiteTuple(pose?.orientation_rpy_rad, 3)
    ?? finiteTuple(pose?.orientationRPYRad, 3)
    ?? (rpyDeg === null ? null : rpyDeg.map((v) => (v * Math.PI) / 180));
  const q = new Quaternion();
  if (rpyRad !== null) {
    q.setFromEuler(new Euler(rpyRad[0], rpyRad[1], rpyRad[2], 'XYZ'));
  }
  return {
    position: position as [number, number, number],
    quaternion: [q.x, q.y, q.z, q.w],
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

export function composePose(parentWorld: PoseTuple, childInParent: PoseTuple): PoseTuple {
  return poseFromMatrix(matrixFromPose(parentWorld).multiply(matrixFromPose(childInParent)));
}

export function relativePose(childWorld: PoseTuple, parentWorld: PoseTuple): PoseTuple {
  return poseFromMatrix(
    matrixFromPose(parentWorld).invert().multiply(matrixFromPose(childWorld)),
  );
}
