import { describe, expect, it } from 'vitest';
import {
  composePose,
  poseFromRecord,
  relativePose,
  type PoseTuple,
} from './poseTransforms';

function expectPoseClose(actual: PoseTuple, expected: PoseTuple): void {
  actual.position.forEach((value, index) => {
    expect(value).toBeCloseTo(expected.position[index], 9);
  });
  actual.quaternion.forEach((value, index) => {
    expect(value).toBeCloseTo(expected.quaternion[index], 9);
  });
}

describe('pose transform helpers', () => {
  it('parses and normalizes canonical xyzw poses', () => {
    expect(
      poseFromRecord({
        position_m: [1, 2, 3],
        orientation_xyzw: [0, 0, 0, 2],
      }),
    ).toEqual({ position: [1, 2, 3], quaternion: [0, 0, 0, 1] });
  });

  it('rejects compatibility aliases and invalid quaternions', () => {
    expect(
      poseFromRecord({ positionM: [1, 2, 3], orientationXyzw: [0, 0, 0, 1] }),
    ).toBeNull();
    expect(
      poseFromRecord({ position_m: [1, 2, 3], orientation_xyzw: [0, 0, 0, 0] }),
    ).toBeNull();
  });

  it('round-trips child poses between parent and world frames', () => {
    const halfSqrt = Math.SQRT1_2;
    const parentWorld: PoseTuple = {
      position: [-0.21814000606536865, -0.571590006351471, 0.13],
      quaternion: [0, 0, -halfSqrt, halfSqrt],
    };
    const childInParent: PoseTuple = {
      position: [-0.6944924933552961, 0.24622926206090415, 0.28870366325322006],
      quaternion: [
        0.6338908675052229,
        0.7730568401636817,
        -0.01407355189677022,
        -0.019168336042257924,
      ],
    };

    const childWorld = composePose(parentWorld, childInParent);
    expect(childWorld.position[0]).toBeCloseTo(0.02808923222352791, 7);
    expect(childWorld.position[1]).toBeCloseTo(0.12290249543208553, 7);
    expect(childWorld.position[2]).toBeCloseTo(0.41870366325322006, 9);
    expectPoseClose(relativePose(childWorld, parentWorld), childInParent);
  });
});
