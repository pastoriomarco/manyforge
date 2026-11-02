import { Quaternion, Vector3 } from 'three';
import type { PoseTuple } from './poseTransforms';

export interface GizmoSettings {
  mode: 'translate' | 'rotate' | 'scale';
  space: 'local' | 'world';
  snap: boolean;
  linearStepM: number;
  angularStepDeg: number;
}

export const DEFAULT_GIZMO_SETTINGS: GizmoSettings = {
  mode: 'translate', space: 'world', snap: false, linearStepM: 0.01, angularStepDeg: 5,
};
export const LINEAR_STEP_OPTIONS = [0.001, 0.01, 0.1] as const;
export const ANGULAR_STEP_OPTIONS = [0.1, 0.5, 1, 5, 10, 30] as const;

/**
 * Snap so the gizmo lands on a snapped position. In world space each moved
 * axis lands on the world grid of `linearStepM`; an axis the drag did not move
 * keeps its value. In local space the grid is the start pose's frame, so the
 * change along each local axis is a whole number of steps. Rotation snaps the
 * change from mouse-down to whole `angularStepDeg` steps.
 */
export function snapGizmoPose(start: PoseTuple, raw: PoseTuple, settings: GizmoSettings): PoseTuple {
  if (!settings.snap || settings.mode === 'scale') return raw;
  const initial = new Quaternion(...start.quaternion).normalize();
  if (settings.mode === 'translate') {
    const step = settings.linearStepM;
    if (!Number.isFinite(step) || step <= 0) return raw;
    const snap = (value: number) => Math.round(value / step) * step;
    if (settings.space === 'world') {
      const position = raw.position.map((value, axis) => (
        value === start.position[axis] ? value : snap(value))) as [number, number, number];
      return { position, quaternion: raw.quaternion };
    }
    const delta = new Vector3(...raw.position).sub(new Vector3(...start.position));
    delta.applyQuaternion(initial.clone().invert());
    delta.set(...delta.toArray().map(snap) as [number, number, number]);
    delta.applyQuaternion(initial);
    delta.add(new Vector3(...start.position));
    return { position: delta.toArray(), quaternion: raw.quaternion };
  }
  const step = settings.angularStepDeg * Math.PI / 180;
  if (!Number.isFinite(step) || step <= 0) return raw;
  const target = new Quaternion(...raw.quaternion).normalize();
  const delta = settings.space === 'local'
    ? initial.clone().invert().multiply(target)
    : target.multiply(initial.clone().invert());
  if (delta.w < 0) delta.set(-delta.x, -delta.y, -delta.z, -delta.w);
  const axis = new Vector3(delta.x, delta.y, delta.z);
  const angle = 2 * Math.atan2(axis.length(), delta.w);
  const rotation = axis.lengthSq() < 1e-20 ? new Quaternion()
    : new Quaternion().setFromAxisAngle(axis.normalize(), Math.round(angle / step) * step);
  const orientation = (settings.space === 'local'
    ? initial.multiply(rotation) : rotation.multiply(initial)).normalize();
  return { position: raw.position, quaternion: orientation.toArray() };
}
