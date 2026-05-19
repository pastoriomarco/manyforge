/**
 * Move-To target computation (Robot panel → Move To tab, ADR-0015 + ADR-0016).
 *
 * Resolves a user-picked entity (scene object, named frame, or
 * stored move-node target) and a set of orientation/offset overrides
 * into a concrete cartesian-jog or joint-jog target. Shared between:
 *   - RobotControlPanel's MoveToBody (renders the picker, dispatches Go)
 *   - SceneViewer's MoveToTargetGhost (renders the 3D ghost preview)
 *
 * Both call into the same `computeMoveToTarget` so the preview the
 * operator sees is what gets dispatched on Go — no drift between UI
 * preview and what the kernel actually executes.
 */
import { Quaternion, Vector3 } from 'three';

import type {
  MoveToOrientationMode,
  MoveToState,
} from '../store/viewerSelectionStore';
import type {
  SceneFrameState,
  SceneObjectState,
  SceneRobotState,
} from '../types/api';

export interface CartesianMoveTarget {
  kind: 'cartesian';
  /** Position in the same frame as `tcpPose` (base frame). */
  position: [number, number, number];
  /** xyzw quaternion in the base frame. */
  quaternion: [number, number, number, number];
  /** Short label used in the panel status row. */
  label: string;
}

export interface JointMoveTarget {
  kind: 'joint';
  jointNames: string[];
  jointPositionsRad: number[];
  label: string;
}

export type MoveToTarget = CartesianMoveTarget | JointMoveTarget;

export interface StoredPoseTarget {
  /** Move node name (uniquely identifies it within the program tree). */
  nodeName: string;
  motionType: 'pose_goal' | 'joint_goal';
  params: Record<string, unknown>;
}

export interface ComputeMoveToTargetArgs {
  moveTo: MoveToState;
  objects: SceneObjectState[];
  frames: SceneFrameState[];
  storedPoses: StoredPoseTarget[];
  tcpPose: { position_m?: number[]; orientation_xyzw?: number[] } | null;
  robot?: SceneRobotState | null;
}

/**
 * Resolve the Move-To selection into a concrete target. Returns null
 * when the selection is incomplete or the underlying data is missing
 * (e.g. operator picked an object that has been removed since).
 */
export function computeMoveToTarget(
  args: ComputeMoveToTargetArgs,
): MoveToTarget | null {
  const { moveTo, objects, frames, storedPoses, tcpPose } = args;

  if (moveTo.source === 'pose') {
    return resolveStoredPoseTarget(moveTo, storedPoses, tcpPose);
  }

  // Object / frame sources reduce to "entity has a pose in base frame".
  const entityPose = resolveEntityBasePose(moveTo, objects, frames);
  if (entityPose === null) return null;
  return assembleCartesian(moveTo, entityPose, tcpPose, entityLabel(moveTo, objects, frames));
}

function resolveEntityBasePose(
  moveTo: MoveToState,
  objects: SceneObjectState[],
  frames: SceneFrameState[],
): { position: Vector3; quaternion: Quaternion } | null {
  if (moveTo.source === 'object') {
    if (moveTo.objectId === null) return null;
    const obj = objects.find((o) => o.object_id === moveTo.objectId);
    if (obj === undefined) return null;
    const pose = (obj.pose_in_universe ?? {}) as Record<string, unknown>;
    const position = readTriple(pose.position_m);
    const quaternion = readQuad(pose.orientation_xyzw) ?? [0, 0, 0, 1];
    if (position === null) return null;
    return {
      position: new Vector3(...position),
      quaternion: new Quaternion(...quaternion),
    };
  }
  if (moveTo.source === 'frame') {
    if (moveTo.frameName === null) return null;
    const frame = frames.find((f) => f.name === moveTo.frameName);
    if (frame === undefined || !frame.poseAvailable || frame.poseInWorld == null) return null;
    const position = readTriple(frame.poseInWorld.position_m);
    const quaternion = readQuad(frame.poseInWorld.orientation_xyzw);
    if (position === null || quaternion === null) return null;
    return {
      position: new Vector3(...position),
      quaternion: new Quaternion(...quaternion),
    };
  }
  return null;
}

function entityLabel(
  moveTo: MoveToState,
  objects: SceneObjectState[],
  frames: SceneFrameState[],
): string {
  if (moveTo.source === 'object' && moveTo.objectId !== null) {
    const obj = objects.find((o) => o.object_id === moveTo.objectId);
    return obj ? `object ${obj.object_id}` : `object ${moveTo.objectId}`;
  }
  if (moveTo.source === 'frame' && moveTo.frameName !== null) {
    const f = frames.find((fr) => fr.name === moveTo.frameName);
    return f ? `frame ${f.name}` : `frame ${moveTo.frameName}`;
  }
  return 'entity';
}

function assembleCartesian(
  moveTo: MoveToState,
  entityPose: { position: Vector3; quaternion: Quaternion },
  tcpPose: { position_m?: number[]; orientation_xyzw?: number[] } | null,
  label: string,
): CartesianMoveTarget | null {
  // 1. Orientation mode picks the base orientation before offset.
  const baseOrientation = pickBaseOrientation(
    moveTo.orientationMode,
    entityPose,
    tcpPose,
  );
  if (baseOrientation === null) return null;

  // 2. Offset rpy applied as a local delta on top of the base
  //    orientation. The operator types degrees; convert to radians.
  const rpyRad: [number, number, number] = [
    (moveTo.offsetRpyDeg[0] * Math.PI) / 180,
    (moveTo.offsetRpyDeg[1] * Math.PI) / 180,
    (moveTo.offsetRpyDeg[2] * Math.PI) / 180,
  ];
  const rpyDelta = quatFromRpyRad(rpyRad);
  const finalOrientation = baseOrientation.clone().multiply(rpyDelta);

  // 3. Position = entity centroid + offset_xyz expressed in the
  //    FINAL orientation's local frame. "Offset z=0.10" with face-
  //    down therefore means "10 cm directly above the entity", which
  //    is the natural approach-pose convention. World-frame offset
  //    would force operators to recompute by hand whenever the
  //    entity is rotated.
  const offsetWorld = new Vector3(...moveTo.offsetXyzM).applyQuaternion(finalOrientation);
  const finalPosition = entityPose.position.clone().add(offsetWorld);

  return {
    kind: 'cartesian',
    position: [finalPosition.x, finalPosition.y, finalPosition.z],
    quaternion: [finalOrientation.x, finalOrientation.y, finalOrientation.z, finalOrientation.w],
    label,
  };
}

function pickBaseOrientation(
  mode: MoveToOrientationMode,
  entityPose: { position: Vector3; quaternion: Quaternion },
  tcpPose: { position_m?: number[]; orientation_xyzw?: number[] } | null,
): Quaternion | null {
  if (mode === 'use-entity') {
    return entityPose.quaternion.clone();
  }
  if (mode === 'keep-current') {
    if (tcpPose == null) return null;
    const q = readQuad(tcpPose.orientation_xyzw);
    return q ? new Quaternion(...q) : null;
  }
  if (mode === 'face-down') {
    // World "down" = -Z. We want the TCP's tool axis (also -Z by
    // convention) to point in world -Z. That's the identity
    // orientation for a standard tool frame. To keep some
    // determinism around yaw, we don't pin it — operators tweak
    // yaw via the offset rpy.
    return new Quaternion(0, 0, 0, 1);
  }
  return null;
}

function quatFromRpyRad(rpy: [number, number, number]): Quaternion {
  const [r, p, y] = rpy;
  const cr = Math.cos(r / 2);
  const sr = Math.sin(r / 2);
  const cp = Math.cos(p / 2);
  const sp = Math.sin(p / 2);
  const cy = Math.cos(y / 2);
  const sy = Math.sin(y / 2);
  const w = cr * cp * cy + sr * sp * sy;
  const qx = sr * cp * cy - cr * sp * sy;
  const qy = cr * sp * cy + sr * cp * sy;
  const qz = cr * cp * sy - sr * sp * cy;
  return new Quaternion(qx, qy, qz, w);
}

function resolveStoredPoseTarget(
  moveTo: MoveToState,
  storedPoses: StoredPoseTarget[],
  tcpPose: { position_m?: number[]; orientation_xyzw?: number[] } | null,
): MoveToTarget | null {
  if (moveTo.poseNodeName === null) return null;
  const pose = storedPoses.find((p) => p.nodeName === moveTo.poseNodeName);
  if (pose === undefined) return null;

  if (pose.motionType === 'joint_goal') {
    // Program YAML stores joint_goal as flat sibling keys, not nested:
    //   motion_type: joint_goal
    //   joint_goal_names: [...]
    //   joint_goal_positions_rad: [...]
    const jointNames = pose.params['joint_goal_names'];
    const jointPositions = pose.params['joint_goal_positions_rad'];
    if (!Array.isArray(jointNames) || !Array.isArray(jointPositions)) return null;
    if (jointNames.length === 0 || jointNames.length !== jointPositions.length) return null;
    return {
      kind: 'joint',
      jointNames: jointNames.map((n) => String(n)),
      jointPositionsRad: jointPositions.map((v) => Number(v)),
      label: `pose ${pose.nodeName}`,
    };
  }

  // pose_goal motion: nested object with position_m and either
  // orientation_xyzw (quaternion) or orientation_rpy_deg (Euler).
  // The stored pose becomes the entity pose; the same orientation/
  // offset pipeline applies so the operator can preview / offset
  // stored poses identically to raw entity targets.
  const poseGoal = pose.params['pose_goal'] as Record<string, unknown> | undefined;
  if (poseGoal === undefined) return null;
  const position = readTriple(poseGoal['position_m']);
  if (position === null) return null;
  const xyzw = readQuad(poseGoal['orientation_xyzw']);
  const rpyDeg = readTriple(poseGoal['orientation_rpy_deg']);
  let quaternion: [number, number, number, number];
  if (xyzw !== null) {
    quaternion = xyzw;
  } else if (rpyDeg !== null) {
    const rpyRad: [number, number, number] = [
      (rpyDeg[0] * Math.PI) / 180,
      (rpyDeg[1] * Math.PI) / 180,
      (rpyDeg[2] * Math.PI) / 180,
    ];
    const q = quatFromRpyRad(rpyRad);
    quaternion = [q.x, q.y, q.z, q.w];
  } else {
    quaternion = [0, 0, 0, 1];
  }
  const entityPose = {
    position: new Vector3(...position),
    quaternion: new Quaternion(...quaternion),
  };
  return assembleCartesian(moveTo, entityPose, tcpPose, `pose ${pose.nodeName}`);
}

function readTriple(value: unknown): [number, number, number] | null {
  if (!Array.isArray(value) || value.length !== 3) return null;
  const out: [number, number, number] = [Number(value[0]), Number(value[1]), Number(value[2])];
  return out.every(Number.isFinite) ? out : null;
}

function readQuad(value: unknown): [number, number, number, number] | null {
  if (!Array.isArray(value) || value.length !== 4) return null;
  const out: [number, number, number, number] = [
    Number(value[0]),
    Number(value[1]),
    Number(value[2]),
    Number(value[3]),
  ];
  return out.every(Number.isFinite) ? out : null;
}
