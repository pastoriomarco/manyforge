/**
 * Exact conversion between the two stored Program pose orientation forms.
 *
 * Convention (matches manyforge_behavior/geometry.py): `rpy_deg` is intrinsic
 * ZYX roll/pitch/yaw in degrees (R = Rz(yaw) Ry(pitch) Rx(roll)); `xyzw` is a
 * unit quaternion. Results keep full float precision.
 */

export type RpyDeg = [number, number, number];
export type Xyzw = [number, number, number, number];

const RAD_PER_DEG = Math.PI / 180;

// Half-angle magnitude below which |pitch| is 90 deg to float precision. Roll
// and yaw are then coupled, so roll is fixed at zero and yaw carries the whole
// rotation about Z; the rotation error of that choice is below this value.
const GIMBAL_LOCK_EPSILON = 1e-9;

function finiteTuple(values: readonly unknown[], length: number): number[] | undefined {
  return values.length === length && values.every(Number.isFinite)
    ? values as number[]
    : undefined;
}

function withoutNegativeZero(value: number): number {
  return value === 0 ? 0 : value;
}

function wrapRadians(angle: number): number {
  if (angle > Math.PI) return angle - 2 * Math.PI;
  if (angle <= -Math.PI) return angle + 2 * Math.PI;
  return angle;
}

/** Normalize a finite non-zero quaternion; undefined when it has no rotation meaning. */
export function normalizeXyzw(xyzw: readonly unknown[]): Xyzw | undefined {
  const values = finiteTuple(xyzw, 4);
  if (!values) return undefined;
  const scale = Math.max(...values.map(Math.abs));
  if (scale === 0) return undefined;
  const scaled = values.map((component) => component / scale);
  const norm = Math.hypot(...scaled);
  return scaled.map((component) => withoutNegativeZero(component / norm)) as Xyzw;
}

export function rpyDegToXyzw(rpyDeg: readonly unknown[]): Xyzw | undefined {
  const values = finiteTuple(rpyDeg, 3);
  if (!values) return undefined;
  const [r, p, y] = values.map((degrees) => degrees * RAD_PER_DEG / 2);
  const cr = Math.cos(r);
  const sr = Math.sin(r);
  const cp = Math.cos(p);
  const sp = Math.sin(p);
  const cy = Math.cos(y);
  const sy = Math.sin(y);
  return normalizeXyzw([
    sr * cp * cy - cr * sp * sy,
    cr * sp * cy + sr * cp * sy,
    cr * cp * sy - sr * sp * cy,
    cr * cp * cy + sr * sp * sy,
  ]);
}

/**
 * Half-angle extraction: (w - y) + i(z + x) = (cos p/2 - sin p/2) e^{i(yaw+roll)/2}
 * and (w + y) + i(z - x) = (cos p/2 + sin p/2) e^{i(yaw-roll)/2}, which stays
 * well conditioned up to gimbal lock, unlike asin/atan2 of matrix entries.
 */
export function xyzwToRpyDeg(xyzw: readonly unknown[]): RpyDeg | undefined {
  const normalized = normalizeXyzw(xyzw);
  if (!normalized) return undefined;
  const [x, y, z, w] = normalized;
  const sumNorm = Math.hypot(w - y, z + x);
  const differenceNorm = Math.hypot(w + y, z - x);
  let halfSum = Math.atan2(z + x, w - y);
  let halfDifference = Math.atan2(z - x, w + y);
  if (sumNorm < GIMBAL_LOCK_EPSILON) halfSum = halfDifference;
  else if (differenceNorm < GIMBAL_LOCK_EPSILON) halfDifference = halfSum;
  const pitch = 2 * Math.atan2(differenceNorm, sumNorm) - Math.PI / 2;
  return [
    wrapRadians(halfSum - halfDifference),
    pitch,
    wrapRadians(halfSum + halfDifference),
  ].map((angle) => withoutNegativeZero(angle / RAD_PER_DEG)) as RpyDeg;
}
