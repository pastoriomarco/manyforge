import { describe, expect, it } from 'vitest';
import { Euler, Quaternion, Vector3 } from 'three';
import { DEFAULT_GIZMO_SETTINGS, snapGizmoPose } from './gizmo';
import type { PoseTuple } from './poseTransforms';
import { samePose } from './poseTransforms';
import { cartesianMoveOffsetsFromTarget, DEFAULT_MOVE_TO_OPTIONS } from './moveTo';

const start: PoseTuple = { position: [0.123, -0.237, 0.344], quaternion: new Quaternion().setFromEuler(new Euler(0.1, 0.2, 0.7)).toArray() };
describe('gizmo snapping', () => {
  it('lands moved world axes on the grid and keeps untouched axes', () => {
    const settings = { ...DEFAULT_GIZMO_SETTINGS, snap: true };
    expect(snapGizmoPose(start, start, settings)).toEqual(start);
    const snapped = snapGizmoPose(start, { ...start, position: [0.139, -0.251, 0.344] }, settings).position;
    expect(snapped[0]).toBeCloseTo(0.14, 12);
    expect(snapped[1]).toBeCloseTo(-0.25, 12);
    expect(snapped[2]).toBe(0.344);
    expect(snapGizmoPose(start, start, DEFAULT_GIZMO_SETTINGS)).toBe(start);
  });
  it('a small drag from an off-grid start still lands on the nearest grid position', () => {
    const settings = { ...DEFAULT_GIZMO_SETTINGS, snap: true };
    const snapped = snapGizmoPose(start, { ...start, position: [0.126, -0.237, 0.344] }, settings).position;
    expect(snapped[0]).toBeCloseTo(0.13, 12);
    expect(samePose({ ...start, position: snapped }, start)).toBe(false);
  });
  it('snaps local translation before resolving non-grid source offsets', () => {
    const delta = new Vector3(0.016, 0, 0).applyQuaternion(new Quaternion(...start.quaternion));
    const target = { ...start, position: new Vector3(...start.position).add(delta).toArray() };
    const snapped = snapGizmoPose(start, target, { ...DEFAULT_GIZMO_SETTINGS, snap: true, space: 'local' });
    const local = new Vector3(...snapped.position).sub(new Vector3(...start.position)).applyQuaternion(new Quaternion(...start.quaternion).invert());
    expect(local.x).toBeCloseTo(0.02, 12);
    expect(local.y).toBeCloseTo(0, 12);
    const source = { ...start, quaternion: [0, 0, 0, 1] as PoseTuple['quaternion'] };
    const offsets = cartesianMoveOffsetsFromTarget(source, start, DEFAULT_MOVE_TO_OPTIONS, snapped)!;
    expect(offsets.offsetXyzM[0]).not.toBeCloseTo(0.02, 4);
    expect(snapped.quaternion).toEqual(start.quaternion);
  });
  it('snaps rotation about either axis basis, preserving position and scale semantics', () => {
    for (const space of ['local', 'world'] as const) {
      const initial = new Quaternion(...start.quaternion);
      const rotation = new Quaternion().setFromAxisAngle(new Vector3(0, 1, 0), -12 * Math.PI / 180);
      const snappedRotation = new Quaternion().setFromAxisAngle(new Vector3(0, 1, 0), -10 * Math.PI / 180);
      const target = { ...start, quaternion: (space === 'local' ? initial.clone().multiply(rotation) : rotation.multiply(initial.clone())).toArray() };
      const expected = { ...start, quaternion: (space === 'local' ? initial.multiply(snappedRotation) : snappedRotation.multiply(initial)).toArray() };
      expect(samePose(snapGizmoPose(start, target, { ...DEFAULT_GIZMO_SETTINGS, snap: true, mode: 'rotate', space }), expected)).toBe(true);
      expect(snapGizmoPose(start, target, { ...DEFAULT_GIZMO_SETTINGS, snap: true, mode: 'scale' })).toBe(target);
    }
  });
});
