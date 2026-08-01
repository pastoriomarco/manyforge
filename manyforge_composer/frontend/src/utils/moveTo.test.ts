import { describe, expect, it } from 'vitest';
import { computeCartesianMoveDraft } from './moveTo';
import type { PoseTuple } from './poseTransforms';

const entity: PoseTuple = {
  position: [1, 2, 3],
  quaternion: [0, 0, 0, 1],
};

describe('Cartesian move authoring', () => {
  it('applies local offsets to the selected entity orientation', () => {
    expect(
      computeCartesianMoveDraft(entity, null, {
        orientationMode: 'use-entity',
        offsetXyzM: [0, 0, 0.1],
        offsetRpyRad: [0, 0, 0],
      }),
    ).toEqual({
      position: [1, 2, 3.1],
      quaternion: [0, 0, 0, 1],
    });
  });

  it('fails closed when keep-current has no observed TCP pose', () => {
    expect(
      computeCartesianMoveDraft(entity, null, {
        orientationMode: 'keep-current',
        offsetXyzM: [0, 0, 0],
        offsetRpyRad: [0, 0, 0],
      }),
    ).toBeNull();
  });
});
