import { describe, expect, it } from 'vitest';
import {
  classifyStructuralAssetFailure,
  normalizeQuaternion,
} from './structuralTransforms';

describe('normalizeQuaternion', () => {
  it('normalizes the authored xyzw orientation without reordering it', () => {
    const normalized = normalizeQuaternion([0, 0, 3, 4]);

    expect(normalized[0]).toBe(0);
    expect(normalized[1]).toBe(0);
    expect(normalized[2]).toBeCloseTo(0.6, 15);
    expect(normalized[3]).toBeCloseTo(0.8, 15);
    expect(Math.hypot(...normalized)).toBeCloseTo(1, 15);
  });

  it('rejects a zero quaternion instead of substituting identity', () => {
    expect(() => normalizeQuaternion([0, 0, 0, 0])).toThrow(
      'Cannot render a zero or non-finite quaternion',
    );
  });
});

describe('classifyStructuralAssetFailure', () => {
  it('recognizes a stale keyed model response', () => {
    expect(classifyStructuralAssetFailure(
      new Error('fetch responded with HTTP 409 for stale URDF mesh'),
    )).toBe('stale_model');
  });

  it('does not mislabel an ordinary parse failure as stale', () => {
    expect(classifyStructuralAssetFailure(new Error('GLB parse failed'))).toBe(
      'asset_unavailable',
    );
  });
});
