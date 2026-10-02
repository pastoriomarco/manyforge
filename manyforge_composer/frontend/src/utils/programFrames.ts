import type { JsonValue, ProgramDocument, ProgramPoseFrame } from '../types/api';

export type { ProgramPoseFrame };

/** The frames one editor may offer, from the Program's declarations. */
export interface FrameScope {
  program: Pick<ProgramDocument, 'target_roles' | 'scene'>;
  /** The robot a node moves; its frames are listed first. */
  movingTargetRole?: string;
  /** A pose key's `allowed_frames`: only these are offered. */
  allowedFrames?: readonly ProgramPoseFrame[];
}

export interface ProgramFrameChoice {
  key: string;
  frame: ProgramPoseFrame;
  label: string;
  group: string;
}

function text(value: JsonValue | undefined): string | null {
  return typeof value === 'string' && value !== '' ? value : null;
}

/** The pose frame a JSON value stands for, or null when it is not one. */
export function poseFrameOf(value: JsonValue | undefined): ProgramPoseFrame | null {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return null;
  const keys = Object.keys(value).sort().join(',');
  if (value.kind === 'cell_frame' && keys === 'kind') return { kind: 'cell_frame' };
  const targetRole = text(value.target_role);
  const frameRole = text(value.frame_role);
  if (
    value.kind === 'target_frame' && keys === 'frame_role,kind,target_role'
    && targetRole !== null && frameRole !== null
  ) {
    return { kind: 'target_frame', target_role: targetRole, frame_role: frameRole };
  }
  const resource = text(value.scene_resource_id);
  if (value.kind === 'scene_resource' && keys === 'kind,scene_resource_id' && resource !== null) {
    return { kind: 'scene_resource', scene_resource_id: resource };
  }
  return null;
}

export function frameKey(frame: ProgramPoseFrame): string {
  if (frame.kind === 'cell_frame') return 'cell';
  if (frame.kind === 'scene_resource') return `scene:${frame.scene_resource_id}`;
  return `robot:${frame.target_role}:${frame.frame_role}`;
}

/** Robot plus frame, so equal frame names of two robots stay distinct. */
export function frameLabel(frame: ProgramPoseFrame): string {
  if (frame.kind === 'cell_frame') return 'Cell';
  if (frame.kind === 'scene_resource') return frame.scene_resource_id;
  return `${frame.target_role} · ${frame.frame_role}`;
}

/** The canonical `allowed_frames` order: kind, then robot or Scene object, then frame role. */
export function canonicalFrames(frames: readonly ProgramPoseFrame[]): ProgramPoseFrame[] {
  const order = (frame: ProgramPoseFrame): [string, string, string] => [
    frame.kind,
    frame.kind === 'target_frame'
      ? frame.target_role
      : frame.kind === 'scene_resource' ? frame.scene_resource_id : '',
    frame.kind === 'target_frame' ? frame.frame_role : '',
  ];
  const compare = (left: string, right: string) => (left < right ? -1 : left > right ? 1 : 0);
  const unique = new Map(frames.map((frame) => [frameKey(frame), frame]));
  return [...unique.values()].sort((left, right) => {
    const a = order(left);
    const b = order(right);
    return compare(a[0], b[0]) || compare(a[1], b[1]) || compare(a[2], b[2]);
  });
}

function frameRoles(target: JsonValue): string[] {
  if (target === null || typeof target !== 'object' || Array.isArray(target)) return [];
  const roles = target.required_frame_roles;
  return Array.isArray(roles)
    ? roles.filter((role): role is string => typeof role === 'string')
    : [];
}

/**
 * Every frame the Program declares, in selector order: the moving robot's own
 * frames first, then other robots' frames, then the cell and Scene objects.
 */
export function programFrameChoices(scope: FrameScope): ProgramFrameChoice[] {
  const choices: ProgramFrameChoice[] = [];
  const moving = scope.movingTargetRole;
  const robots = Object.entries(scope.program.target_roles ?? {});
  const robot = (targetRole: string, roles: string[], group: string) => {
    for (const frameRole of roles) {
      const frame: ProgramPoseFrame = {
        kind: 'target_frame', target_role: targetRole, frame_role: frameRole,
      };
      choices.push({ key: frameKey(frame), frame, label: frameLabel(frame), group });
    }
  };
  if (moving !== undefined && scope.program.target_roles?.[moving] !== undefined) {
    robot(moving, frameRoles(scope.program.target_roles[moving]), `Moving robot · ${moving}`);
  }
  for (const [targetRole, target] of robots) {
    if (targetRole !== moving) {
      robot(targetRole, frameRoles(target), moving === undefined ? 'Robots' : 'Other robots');
    }
  }
  choices.push({ key: 'cell', frame: { kind: 'cell_frame' }, label: 'Cell', group: 'Cell' });
  for (const resource of scope.program.scene ?? []) {
    const frame: ProgramPoseFrame = {
      kind: 'scene_resource', scene_resource_id: resource.scene_resource_id,
    };
    choices.push({ key: frameKey(frame), frame, label: frameLabel(frame), group: 'Scene objects' });
  }
  if (scope.allowedFrames === undefined) return choices;
  const allowed = new Set(scope.allowedFrames.map(frameKey));
  return choices.filter((choice) => allowed.has(choice.key));
}

/** Which kinds of frame one field accepts (from its catalog schema). */
export interface FrameOffer {
  /** Robots other than the moving one; without it only the moving robot's frames. */
  otherRobots: boolean;
  cell: boolean;
  scene: boolean;
}

export function offeredFrameChoices(
  scope: FrameScope,
  offer: FrameOffer = { otherRobots: true, cell: true, scene: true },
): ProgramFrameChoice[] {
  return programFrameChoices(scope).filter((choice) => {
    if (choice.frame.kind === 'cell_frame') return offer.cell;
    if (choice.frame.kind === 'scene_resource') return offer.scene;
    return offer.otherRobots || choice.frame.target_role === scope.movingTargetRole;
  });
}
