import { Euler, Matrix4, Quaternion, Vector3 } from 'three';
import type { ProgramBindingDocument } from '../types/api';
import type { PoseTuple } from './poseTransforms';
import { rpyDegToXyzw } from './orientation';

export type MoveToOrientationMode = 'keep-current' | 'face-down' | 'use-entity';

export interface CartesianMoveDraftOptions {
  orientationMode: MoveToOrientationMode;
  offsetXyzM: [number, number, number];
  offsetRpyRad: [number, number, number];
}

export const DEFAULT_MOVE_TO_OPTIONS: CartesianMoveDraftOptions = {
  orientationMode: 'keep-current', offsetXyzM: [0, 0, 0], offsetRpyRad: [0, 0, 0],
};

export type MoveToDraftSource =
  | { id: string; kind: 'pose'; pose: PoseTuple }
  | { id: string; kind: 'joint'; positionsRad: number[] };

export interface MoveToDraft {
  source: MoveToDraftSource;
  /** Capture once; live observations must not rotate an authored offset. */
  currentTcpPose: PoseTuple;
  options: CartesianMoveDraftOptions;
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
    // Stored Program RPY is intrinsic ZYX, like the backend.
    const xyzw = rpyDegToXyzw(orientation.rpy_deg);
    if (xyzw !== undefined) quaternion = new Quaternion(...xyzw);
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
    new Vector3(...options.offsetXyzM).applyQuaternion(new Quaternion(...entityPose.quaternion).normalize()),
  );
  return {
    position: [position.x, position.y, position.z],
    quaternion: [orientation.x, orientation.y, orientation.z, orientation.w],
  };
}

/** Inverse of the manual source-relative draft, not stored Program semantics. */
export function cartesianMoveOffsetsFromTarget(
  entityPose: PoseTuple,
  currentTcpPose: PoseTuple | null,
  options: CartesianMoveDraftOptions,
  target: PoseTuple,
): CartesianMoveDraftOptions | null {
  if (!finitePose(entityPose) || !finitePose(target)) return null;
  const reference = selectedOrientation(options.orientationMode, entityPose, currentTcpPose);
  if (reference === null) return null;
  const offset = new Vector3(...target.position).sub(new Vector3(...entityPose.position))
    .applyQuaternion(new Quaternion(...entityPose.quaternion).normalize().invert());
  const rotation = reference.invert().multiply(new Quaternion(...target.quaternion).normalize()).normalize();
  const matrix = new Matrix4().makeRotationFromQuaternion(rotation).elements;
  const cosine = Math.hypot(matrix[0], matrix[4]);
  const pitch = Math.atan2(matrix[8], cosine);
  // Unlike display-only Euler extraction, do not flatten a merely near-
  // singular rotation. At the singularity retain the previous yaw gauge.
  const yaw = cosine > 1e-10 ? Math.atan2(-matrix[4], matrix[0]) : options.offsetRpyRad[2];
  const roll = cosine > 1e-10 ? Math.atan2(-matrix[9], matrix[10])
    : Math.atan2(matrix[6], matrix[5]) - Math.sign(pitch) * yaw;
  // Choose the nearest equivalent Euler representation to keep the fields
  // continuous across +/-pi. The target itself always remains a quaternion.
  const near = (angle: number, previous: number) => angle + 2 * Math.PI * Math.round((previous - angle) / (2 * Math.PI));
  const candidates = [[roll, pitch, yaw], [roll + Math.PI, Math.PI - pitch, yaw + Math.PI]]
    .map((angles) => angles.map((angle, index) => near(angle, options.offsetRpyRad[index])));
  const distance = (angles: number[]) => angles.reduce((sum, angle, index) => sum + (angle - options.offsetRpyRad[index]) ** 2, 0);
  const angles = distance(candidates[0]) <= distance(candidates[1]) ? candidates[0] : candidates[1];
  return {
    ...options,
    offsetXyzM: [offset.x, offset.y, offset.z],
    offsetRpyRad: angles as [number, number, number],
  };
}
