import { describe, expect, it } from 'vitest';

import {
  composePose,
  poseFromBasePoseRecord,
  poseFromRecord,
  relativePose,
  type PoseTuple,
} from './poseTransforms';

function expectPoseClose(actual: PoseTuple, expected: PoseTuple, tolerance = 1e-9): void {
  actual.position.forEach((value, index) => {
    expect(value).toBeCloseTo(expected.position[index], 9);
  });
  actual.quaternion.forEach((value, index) => {
    expect(value).toBeCloseTo(expected.quaternion[index], tolerance === 1e-9 ? 9 : 6);
  });
}

describe('pose transform helpers', () => {
  it('parses and normalizes xyzw poses', () => {
    const pose = poseFromRecord({
      position_m: [1, 2, 3],
      orientation_xyzw: [0, 0, 0, 2],
    });

    expect(pose).not.toBeNull();
    expectPoseClose(pose!, {
      position: [1, 2, 3],
      quaternion: [0, 0, 0, 1],
    });
  });

  it('parses base poses with degree rpy orientation', () => {
    const pose = poseFromBasePoseRecord({
      position_m: [0.25, -0.5, 0.75],
      orientation_rpy_deg: [0, 0, 90],
    });

    expect(pose).not.toBeNull();
    expect(pose!.position).toEqual([0.25, -0.5, 0.75]);
    expect(pose!.quaternion[2]).toBeCloseTo(Math.SQRT1_2, 9);
    expect(pose!.quaternion[3]).toBeCloseTo(Math.SQRT1_2, 9);
  });

  it('round-trips child poses between parent and world frames', () => {
    const baseWorld = poseFromBasePoseRecord({
      position_m: [-0.21814000606536865, -0.571590006351471, 0.13],
      orientation_rpy_deg: [0, 0, -90],
    });
    expect(baseWorld).not.toBeNull();
    const tcpInBase: PoseTuple = {
      position: [-0.6944924933552961, 0.24622926206090415, 0.28870366325322006],
      quaternion: [
        0.6338908675052229,
        0.7730568401636817,
        -0.01407355189677022,
        -0.019168336042257924,
      ],
    };

    const tcpWorld = composePose(baseWorld!, tcpInBase);
    expect(tcpWorld.position[0]).toBeCloseTo(0.02808923222352791, 7);
    expect(tcpWorld.position[1]).toBeCloseTo(0.12290249543208553, 7);
    expect(tcpWorld.position[2]).toBeCloseTo(0.41870366325322006, 9);

    expectPoseClose(relativePose(tcpWorld, baseWorld!), tcpInBase);
  });
});
