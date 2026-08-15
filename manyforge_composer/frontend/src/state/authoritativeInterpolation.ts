import * as THREE from 'three';

import type { QuaternionXyzwTuple, Vector3Tuple } from '../types/api';
import { normalizeQuaternion } from '../utils/structuralTransforms';

/**
 * Viewer interpolation is deliberately short and presentation-only. A long
 * source gap must converge to the newest authoritative sample rather than
 * extending a visual trajectory into an unobserved future.
 */
export const MAX_AUTHORITATIVE_INTERPOLATION_MS = 250;

export interface AuthoritativeTransformFrame {
  streamIdentity: string;
  sourceSequence: number;
  observedAtNs: number;
  positionM: Vector3Tuple;
  orientationXyzw: QuaternionXyzwTuple;
}

export interface AuthoritativeTransformTransition {
  fromPosition: THREE.Vector3;
  fromQuaternion: THREE.Quaternion;
  toPosition: THREE.Vector3;
  toQuaternion: THREE.Quaternion;
  startedAtMs: number;
  durationMs: number;
}

function tupleEqual(
  left: readonly number[],
  right: readonly number[],
): boolean {
  return left.every((value, index) => Object.is(value, right[index]));
}

export function sameAuthoritativeTransformFrame(
  left: AuthoritativeTransformFrame,
  right: AuthoritativeTransformFrame,
): boolean {
  return left.streamIdentity === right.streamIdentity
    && left.sourceSequence === right.sourceSequence
    && left.observedAtNs === right.observedAtNs
    && sameAuthoritativeTransformPose(left, right);
}

export function sameAuthoritativeTransformPose(
  left: AuthoritativeTransformFrame,
  right: AuthoritativeTransformFrame,
): boolean {
  return tupleEqual(left.positionM, right.positionM)
    && tupleEqual(left.orientationXyzw, right.orientationXyzw);
}

/**
 * Return a bounded interpolation interval only for a strictly advancing frame
 * from the same authoritative stream. Epoch changes, duplicate/conflicting
 * samples, sequence regressions, and timestamp regressions all snap.
 */
export function authoritativeInterpolationDurationMs(
  previous: AuthoritativeTransformFrame | null,
  next: AuthoritativeTransformFrame,
): number {
  if (
    previous === null
    || previous.streamIdentity !== next.streamIdentity
    || !Number.isSafeInteger(previous.sourceSequence)
    || !Number.isSafeInteger(next.sourceSequence)
    || next.sourceSequence <= previous.sourceSequence
    || !Number.isFinite(previous.observedAtNs)
    || !Number.isFinite(next.observedAtNs)
    || next.observedAtNs <= previous.observedAtNs
  ) return 0;

  const sourceDeltaMs = (next.observedAtNs - previous.observedAtNs) / 1e6;
  if (!Number.isFinite(sourceDeltaMs) || sourceDeltaMs <= 0) return 0;
  return Math.min(sourceDeltaMs, MAX_AUTHORITATIVE_INTERPOLATION_MS);
}

export function createAuthoritativeTransformTransition(
  previous: AuthoritativeTransformFrame | null,
  next: AuthoritativeTransformFrame,
  renderedPosition: THREE.Vector3,
  renderedQuaternion: THREE.Quaternion,
  startedAtMs: number,
): AuthoritativeTransformTransition | null {
  const durationMs = authoritativeInterpolationDurationMs(previous, next);
  if (durationMs === 0) return null;
  const orientation = normalizeQuaternion(next.orientationXyzw);
  return {
    fromPosition: renderedPosition.clone(),
    fromQuaternion: renderedQuaternion.clone().normalize(),
    toPosition: new THREE.Vector3(...next.positionM),
    toQuaternion: new THREE.Quaternion(...orientation),
    startedAtMs,
    durationMs,
  };
}

/**
 * Apply one visual sample without allocating. Time is clamped at both ends, so
 * this function can interpolate between authoritative frames but can never
 * extrapolate beyond either frame.
 */
export function applyAuthoritativeTransformTransition(
  transition: AuthoritativeTransformTransition,
  nowMs: number,
  targetPosition: THREE.Vector3,
  targetQuaternion: THREE.Quaternion,
): boolean {
  const alpha = THREE.MathUtils.clamp(
    (nowMs - transition.startedAtMs) / transition.durationMs,
    0,
    1,
  );
  targetPosition.lerpVectors(
    transition.fromPosition,
    transition.toPosition,
    alpha,
  );
  targetQuaternion.slerpQuaternions(
    transition.fromQuaternion,
    transition.toQuaternion,
    alpha,
  );
  return alpha === 1;
}
