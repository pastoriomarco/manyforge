import type {
  CandidateResponse,
  PreviewLinkPosesRequest,
  SceneGripperState,
  SceneRobotState,
} from '../types/api';

const GRIPPER_GHOST_STATUSES = new Set([
  'planning',
  'ready',
  'stale',
  'approving',
]);

export interface GripperGhostPreview {
  gripperId: string;
  targetPosition: number;
  linkNames: string[];
  request: PreviewLinkPosesRequest;
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

function withinLimit(
  target: number,
  lower: number | null,
  upper: number | null,
): boolean {
  if (isFiniteNumber(lower) && target < lower) return false;
  if (isFiniteNumber(upper) && target > upper) return false;
  return true;
}

/**
 * Build the side-effect-free FK request used to render a staged gripper
 * candidate ghost.
 *
 * The backend gripper candidate does not compute an arm trajectory, so there
 * is no candidate.ghost payload. For visualization, use the current robot
 * state as the FK seed and override only the gripper command joint to the
 * staged target. The renderer filters to the gripper subtree links so the UI
 * shows a gripper ghost, not a duplicate whole robot.
 */
export function gripperGhostPreviewFromCandidate(
  candidate: CandidateResponse | null,
  robot: SceneRobotState | null,
  grippers: SceneGripperState[],
): GripperGhostPreview | null {
  if (
    candidate === null ||
    candidate.inputMode !== 'gripper' ||
    candidate.gripperTarget === null ||
    candidate.gripperTarget === undefined ||
    !GRIPPER_GHOST_STATUSES.has(candidate.status)
  ) {
    return null;
  }
  const target = candidate.gripperTarget.targetPosition;
  if (!isFiniteNumber(target) || robot === null) {
    return null;
  }
  const gripper = grippers.find(
    (g) => g.gripper_id === candidate.gripperTarget?.gripperId,
  );
  if (gripper === undefined) {
    return null;
  }
  if (!withinLimit(target, gripper.limit_lower, gripper.limit_upper)) {
    return null;
  }
  const gripperJoint = gripper.joint_name.trim();
  if (gripperJoint.length === 0) {
    return null;
  }
  const subtreeLinks = gripper.visualization?.subtree_links ?? [];
  const linkNames = Array.from(
    new Set(
      subtreeLinks
        .map((link) => link.name.trim())
        .filter((name) => name.length > 0),
    ),
  );
  if (linkNames.length === 0) {
    return null;
  }

  const jointNames: string[] = [];
  const jointPositions: number[] = [];
  for (const name of robot.joint_names) {
    const position = robot.joint_positions[name];
    if (!isFiniteNumber(position)) continue;
    jointNames.push(name);
    jointPositions.push(position);
  }
  if (jointNames.length === 0) {
    return null;
  }
  const existingIndex = jointNames.indexOf(gripperJoint);
  if (existingIndex >= 0) {
    jointPositions[existingIndex] = target;
  } else {
    jointNames.push(gripperJoint);
    jointPositions.push(target);
  }

  return {
    gripperId: gripper.gripper_id,
    targetPosition: target,
    linkNames,
    request: {
      kind: 'joint',
      joint_names: jointNames,
      joint_positions_rad: jointPositions,
      link_names: linkNames,
    },
  };
}
