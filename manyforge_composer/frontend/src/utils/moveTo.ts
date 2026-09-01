import { Euler, Quaternion, Vector3 } from 'three';
import type { ProgramBindingDocument } from '../types/api';
import type { PoseTuple } from './poseTransforms';

export type MoveToOrientationMode = 'keep-current' | 'face-down' | 'use-entity';

export interface CartesianMoveDraftOptions {
  orientationMode: MoveToOrientationMode;
  offsetXyzM: [number, number, number];
  offsetRpyRad: [number, number, number];
}

export interface StoredPoseResolutionContext {
  programTargetRoleId: string;
  baseFrameRoleId: string;
  sceneResourcePoses: ReadonlyMap<string, PoseTuple>;
  frameRolePoses: ReadonlyMap<string, PoseTuple>;
}

function finitePose(pose: PoseTuple | null): pose is PoseTuple {
  return pose !== null
    && pose.position.every(Number.isFinite)
    && pose.quaternion.every(Number.isFinite)
    && new Quaternion(...pose.quaternion).lengthSq() > 1e-12;
}

function selectedOrientation(
  mode: MoveToOrientationMode,
  entityPose: PoseTuple,
  currentTcpPose: PoseTuple | null,
): Quaternion | null {
  if (mode === 'use-entity') return new Quaternion(...entityPose.quaternion).normalize();
  if (mode === 'face-down') return new Quaternion(0, 0, 0, 1);
  return finitePose(currentTcpPose)
    ? new Quaternion(...currentTcpPose.quaternion).normalize()
    : null;
}

/** Return Program target roles bound to one exact addressed command target. */
export function boundProgramTargetRoleIds(
  binding: ProgramBindingDocument | null,
  controllableComponentId: string,
  commandTargetId: string,
): string[] {
  if (binding === null) return [];
  return binding.target_role_bindings
    .filter((row) => (
      row.controllable_component_id === controllableComponentId
      && row.command_target_id === commandTargetId
    ))
    .map((row) => row.target_role_id)
    .sort();
}

function recordAt(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

/** Parse the Program pose value while retaining no implicit frame assumption. */
export function canonicalProgramPose(value: unknown): PoseTuple | null {
  const pose = recordAt(value);
  const position = pose?.position_m;
  const orientation = recordAt(pose?.orientation);
  if (!Array.isArray(position) || position.length !== 3 || orientation === null) {
    return null;
  }
  const numericPosition = position.map(Number) as [number, number, number];
  if (!numericPosition.every(Number.isFinite)) return null;
  let quaternion: Quaternion | null = null;
  if (Array.isArray(orientation.xyzw) && orientation.xyzw.length === 4) {
    const values = orientation.xyzw.map(Number) as [number, number, number, number];
    if (values.every(Number.isFinite)) quaternion = new Quaternion(...values);
  } else if (Array.isArray(orientation.rpy_deg) && orientation.rpy_deg.length === 3) {
    const values = orientation.rpy_deg.map(Number);
    if (values.every(Number.isFinite)) {
      quaternion = new Quaternion().setFromEuler(new Euler(
        values[0] * Math.PI / 180,
        values[1] * Math.PI / 180,
        values[2] * Math.PI / 180,
        'XYZ',
      ));
    }
  }
  if (quaternion === null || quaternion.lengthSq() < 1e-12) return null;
  quaternion.normalize();
  return {
    position: numericPosition,
    quaternion: [quaternion.x, quaternion.y, quaternion.z, quaternion.w],
  };
}

function referencePose(
  value: unknown,
  context: StoredPoseResolutionContext,
): PoseTuple | null {
  const reference = recordAt(value);
  if (reference === null) return null;
  if (reference.type === 'scene_resource') {
    return typeof reference.scene_resource_id === 'string'
      ? context.sceneResourcePoses.get(reference.scene_resource_id) ?? null
      : null;
  }
  if (reference.type === 'frame_role') {
    return typeof reference.frame_role_id === 'string'
      ? context.frameRolePoses.get(reference.frame_role_id) ?? null
      : null;
  }
  if (reference.type === 'pose') return canonicalProgramPose(reference.pose);
  return null;
}

/**
 * Resolve an authored stored pose through the normative target/offset-reference
 * rule. Every input pose is already expressed in the direct-control base frame.
 */
export function resolveStoredProgramPose(
  parameters: Readonly<Record<string, unknown>>,
  context: StoredPoseResolutionContext,
): PoseTuple | null {
  const goal = canonicalProgramPose(parameters.goal);
  if (goal === null) return null;
  const mode = parameters.target_mode ?? 'absolute';
  if (mode === 'absolute') {
    if (parameters.target_reference !== undefined
      || parameters.offset_reference !== undefined) return null;
    const frame = recordAt(recordAt(parameters.goal)?.frame_ref);
    return frame?.kind === 'target_frame'
      && frame.target_role === context.programTargetRoleId
      && frame.frame_role === context.baseFrameRoleId
      ? goal
      : null;
  }
  if (mode !== 'relative') return null;
  const target = referencePose(parameters.target_reference, context);
  if (target === null) return null;
  const offset = parameters.offset_reference === undefined
    ? target
    : referencePose(parameters.offset_reference, context);
  if (offset === null) return null;
  const offsetOrientation = new Quaternion(...offset.quaternion).normalize();
  const goalOrientation = new Quaternion(...goal.quaternion).normalize();
  const position = new Vector3(...target.position).add(
    new Vector3(...goal.position).applyQuaternion(offsetOrientation),
  );
  offsetOrientation.multiply(goalOrientation).normalize();
  return {
    position: [position.x, position.y, position.z],
    quaternion: [
      offsetOrientation.x,
      offsetOrientation.y,
      offsetOrientation.z,
      offsetOrientation.w,
    ],
  };
}

/**
 * Compute a local Cartesian target from one already-resolved portable pose.
 * Addressed target and command-schema selection remain backend-owned; stored
 * joint targets bypass this pose-only calculation and retain their exact units.
 */
export function computeCartesianMoveDraft(
  entityPose: PoseTuple,
  currentTcpPose: PoseTuple | null,
  options: CartesianMoveDraftOptions,
): PoseTuple | null {
  if (!finitePose(entityPose)) return null;
  if (
    !options.offsetXyzM.every(Number.isFinite)
    || !options.offsetRpyRad.every(Number.isFinite)
  ) {
    return null;
  }
  const orientation = selectedOrientation(
    options.orientationMode,
    entityPose,
    currentTcpPose,
  );
  if (orientation === null) return null;

  const [roll, pitch, yaw] = options.offsetRpyRad;
  const delta = new Quaternion().setFromEuler(
    new Euler(roll, pitch, yaw, 'XYZ'),
  );
  orientation.multiply(delta).normalize();
  const position = new Vector3(...entityPose.position).add(
    new Vector3(...options.offsetXyzM).applyQuaternion(orientation),
  );
  return {
    position: [position.x, position.y, position.z],
    quaternion: [orientation.x, orientation.y, orientation.z, orientation.w],
  };
}
