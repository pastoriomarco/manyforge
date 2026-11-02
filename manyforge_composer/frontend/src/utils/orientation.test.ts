import { describe, expect, it } from 'vitest';

import { normalizeXyzw, rpyDegToXyzw, xyzwToRpyDeg, type Xyzw } from './orientation';

function expectSameRotation(actual: Xyzw | undefined, expected: readonly number[]) {
  expect(actual).toBeDefined();
  const target = normalizeXyzw(expected) as Xyzw;
  const dot = (actual as Xyzw).reduce((sum, component, index) => sum + component * target[index], 0);
  expect(Math.abs(dot)).toBeCloseTo(1, 12);
}

function expectCloseTuple(actual: readonly number[] | undefined, expected: readonly number[], digits = 9) {
  expect(actual).toHaveLength(expected.length);
  expected.forEach((component, index) => expect(actual?.[index]).toBeCloseTo(component, digits));
}

describe('pose orientation conversion (intrinsic ZYX roll/pitch/yaw degrees)', () => {
  it('converts identity without negative zeros', () => {
    expect(rpyDegToXyzw([0, 0, 0])).toEqual([0, 0, 0, 1]);
    expect(xyzwToRpyDeg([0, 0, 0, 1])).toEqual([0, 0, 0]);
    expect(Object.is(xyzwToRpyDeg([0, 0, -0, -1])?.[2], -0)).toBe(false);
  });

  it('matches the backend rpy_deg_to_xyzw convention on single-axis rotations', () => {
    const half = Math.SQRT1_2;
    expectCloseTuple(rpyDegToXyzw([90, 0, 0]), [half, 0, 0, half], 15);
    expectCloseTuple(rpyDegToXyzw([0, 90, 0]), [0, half, 0, half], 15);
    expectCloseTuple(rpyDegToXyzw([0, 0, 90]), [0, 0, half, half], 15);
    // Yaw is applied last (extrinsic XYZ == intrinsic ZYX): Rz(90) Rx(90).
    expectCloseTuple(rpyDegToXyzw([90, 0, 90]), [0.5, 0.5, 0.5, 0.5], 15);
  });

  it('round-trips rpy_deg through xyzw on non-trivial angles', () => {
    for (const rpy of [[10, 20, 30], [-135.5, 42.25, 170], [179, -89.5, -179.75], [0.001, -0.002, 0.003]]) {
      const xyzw = rpyDegToXyzw(rpy);
      expectCloseTuple(xyzwToRpyDeg(xyzw ?? []), rpy);
      expect(Math.hypot(...(xyzw ?? []))).toBeCloseTo(1, 15);
    }
  });

  it('round-trips a measured, slightly non-unit quaternion through rpy_deg', () => {
    const measured = [0.0183412, -0.7071123, 0.0192817, 0.7066541];
    const rpy = xyzwToRpyDeg(measured);
    // Backend manyforge_behavior.geometry.xyzw_to_rpy_deg on the same quaternion.
    expectCloseTuple(rpy, [-114.450387380201, -89.91523304804716, 117.49915800287467], 6);
    expectSameRotation(rpyDegToXyzw(rpy ?? []), measured);
    // Sign-flipped (same rotation) quaternion reports the same angles.
    expectCloseTuple(xyzwToRpyDeg(measured.map((component) => -component)), rpy ?? []);
  });

  it('resolves gimbal lock deterministically with zero roll and exact rotation', () => {
    for (const pitch of [90, -90]) {
      const xyzw = rpyDegToXyzw([25, pitch, 70]);
      const rpy = xyzwToRpyDeg(xyzw ?? []);
      expect(rpy?.[0]).toBe(0);
      expect(rpy?.[1]).toBeCloseTo(pitch, 9);
      expect(rpy?.[2]).toBeCloseTo(pitch > 0 ? 45 : 95, 9);
      expectSameRotation(rpyDegToXyzw(rpy ?? []), xyzw ?? []);
    }
  });

  it('rejects malformed or rotation-free input', () => {
    expect(rpyDegToXyzw([1, 2])).toBeUndefined();
    expect(rpyDegToXyzw([1, Number.NaN, 3])).toBeUndefined();
    expect(xyzwToRpyDeg([0, 0, 0, 0])).toBeUndefined();
    expect(xyzwToRpyDeg([0, 0, 1])).toBeUndefined();
  });
});
