import { describe, expect, it } from 'vitest';
import * as THREE from 'three';

import type { AuthoritativeTransformFrame } from './authoritativeInterpolation';
import {
  MAX_AUTHORITATIVE_INTERPOLATION_MS,
  applyAuthoritativeTransformTransition,
  authoritativeInterpolationDurationMs,
  createAuthoritativeTransformTransition,
  sameAuthoritativeTransformFrame,
  sameAuthoritativeTransformPose,
} from './authoritativeInterpolation';

function frame(
  overrides: Partial<AuthoritativeTransformFrame> = {},
): AuthoritativeTransformFrame {
  return {
    streamIdentity: 'owner-1:source-1:epoch-1',
    sourceSequence: 1,
    observedAtNs: 1_000_000_000,
    positionM: [0, 0, 0],
    orientationXyzw: [0, 0, 0, 1],
    ...overrides,
  };
}

describe('authoritative viewer interpolation', () => {
  it('admits only strictly advancing frames from the same stream', () => {
    const previous = frame();
    const advancing = frame({
      sourceSequence: 3,
      observedAtNs: 1_040_000_000,
    });
    expect(authoritativeInterpolationDurationMs(previous, advancing)).toBe(40);
    expect(authoritativeInterpolationDurationMs(previous, frame({
      streamIdentity: 'owner-2:source-1:epoch-1',
      sourceSequence: 2,
      observedAtNs: 1_040_000_000,
    }))).toBe(0);
    expect(authoritativeInterpolationDurationMs(previous, frame({
      sourceSequence: 1,
      observedAtNs: 1_040_000_000,
    }))).toBe(0);
    expect(authoritativeInterpolationDurationMs(previous, frame({
      sourceSequence: 2,
      observedAtNs: 999_000_000,
    }))).toBe(0);
  });

  it('caps long gaps and treats exact duplicate frames as unchanged', () => {
    expect(MAX_AUTHORITATIVE_INTERPOLATION_MS).toBe(50);
    const previous = frame();
    const next = frame({
      sourceSequence: 2,
      observedAtNs: 2_000_000_000,
    });
    expect(authoritativeInterpolationDurationMs(previous, next))
      .toBe(MAX_AUTHORITATIVE_INTERPOLATION_MS);
    expect(sameAuthoritativeTransformFrame(previous, { ...previous })).toBe(true);
    expect(sameAuthoritativeTransformFrame(previous, {
      ...previous,
      positionM: [0.001, 0, 0],
    })).toBe(false);
    expect(sameAuthoritativeTransformPose(previous, {
      ...next,
      positionM: previous.positionM,
    })).toBe(true);
  });

  it('lerps position, uses shortest-arc slerp, and never extrapolates', () => {
    const previous = frame();
    const next = frame({
      sourceSequence: 2,
      observedAtNs: 1_100_000_000,
      positionM: [2, 4, 6],
      // The negative representation exercises shortest-arc quaternion slerp.
      orientationXyzw: [0, 0, -Math.SQRT1_2, -Math.SQRT1_2],
    });
    const transition = createAuthoritativeTransformTransition(
      previous,
      next,
      new THREE.Vector3(0, 0, 0),
      new THREE.Quaternion(0, 0, 0, 1),
      500,
    );
    expect(transition).not.toBeNull();
    if (transition === null) return;

    const position = new THREE.Vector3();
    const quaternion = new THREE.Quaternion();
    expect(applyAuthoritativeTransformTransition(
      transition,
      450,
      position,
      quaternion,
    )).toBe(false);
    expect(position.toArray()).toEqual([0, 0, 0]);

    expect(applyAuthoritativeTransformTransition(
      transition,
      525,
      position,
      quaternion,
    )).toBe(false);
    expect(position.toArray()).toEqual([1, 2, 3]);
    expect(quaternion.z).toBeCloseTo(Math.sin(Math.PI / 8));
    expect(quaternion.w).toBeCloseTo(Math.cos(Math.PI / 8));

    expect(applyAuthoritativeTransformTransition(
      transition,
      650,
      position,
      quaternion,
    )).toBe(true);
    expect(position.toArray()).toEqual([2, 4, 6]);
    expect(Math.abs(quaternion.z)).toBeCloseTo(Math.SQRT1_2);
    expect(Math.abs(quaternion.w)).toBeCloseTo(Math.SQRT1_2);
  });
});
