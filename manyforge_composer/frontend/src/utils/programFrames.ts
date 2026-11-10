import type { JsonValue, ProgramCollisionBodyReference, ProgramDocument, ProgramFrameReference, ProgramPoseFrame } from '../types/api';

export type { ProgramPoseFrame };

/** Frame facts supplied by the captured Deployment/model catalog. */
export interface FrameScope {
  program: Pick<ProgramDocument, 'scene'>;
  frames?: readonly ProgramFrameReference[];
  bodies?: readonly ProgramCollisionBodyReference[];
  unavailableFrames?: readonly { reference: ProgramFrameReference; reason: string }[];
  movingComponentId?: string;
  /** Exact retained frames on the selected moving target, including mounted tools. */
  movingFrames?: readonly ProgramFrameReference[];
  allowedFrames?: readonly ProgramPoseFrame[];
}

export interface ProgramFrameChoice {
  key: string;
  frame: ProgramPoseFrame;
  label: string;
  group: string;
}

function validFrame(value: JsonValue | undefined): value is { component_id: string; frame_id: string } {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return false;
  return Object.keys(value).sort().join(',') === 'component_id,frame_id'
    && typeof value.component_id === 'string'
    && /^[a-z][a-z0-9_.-]{0,127}$/.test(value.component_id)
    && typeof value.frame_id === 'string'
    && /^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$/.test(value.frame_id);
}

/** Parse only closed v3 pose-frame values. */
export function poseFrameOf(value: JsonValue | undefined): ProgramPoseFrame | null {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return null;
  const keys = Object.keys(value).sort().join(',');
  if (value.kind === 'cell_frame' && keys === 'kind') return { kind: 'cell_frame' };
  if (value.kind === 'target_frame' && keys === 'frame,kind' && validFrame(value.frame)) {
    return { kind: 'target_frame', frame: { ...value.frame } };
  }
  if (value.kind === 'scene_resource' && keys === 'kind,scene_resource_id'
      && typeof value.scene_resource_id === 'string' && value.scene_resource_id !== '') {
    return { kind: 'scene_resource', scene_resource_id: value.scene_resource_id };
  }
  return null;
}

export function frameKey(frame: ProgramPoseFrame): string {
  if (frame.kind === 'cell_frame') return 'cell';
  if (frame.kind === 'scene_resource') return `scene:${frame.scene_resource_id}`;
  return `component:${frame.frame.component_id}:${frame.frame.frame_id}`;
}

export function frameLabel(frame: ProgramPoseFrame): string {
  if (frame.kind === 'cell_frame') return 'Cell';
  if (frame.kind === 'scene_resource') return frame.scene_resource_id;
  return `${frame.frame.component_id} · ${frame.frame.frame_id}`;
}

export function canonicalFrames(frames: readonly ProgramPoseFrame[]): ProgramPoseFrame[] {
  const order = (frame: ProgramPoseFrame): [string, string, string] => [
    frame.kind,
    frame.kind === 'target_frame' ? frame.frame.component_id
      : frame.kind === 'scene_resource' ? frame.scene_resource_id : '',
    frame.kind === 'target_frame' ? frame.frame.frame_id : '',
  ];
  const compare = (left: string, right: string) => (left < right ? -1 : left > right ? 1 : 0);
  const unique = new Map(frames.map((frame) => [frameKey(frame), frame]));
  return [...unique.values()].sort((left, right) => {
    const a = order(left);
    const b = order(right);
    return compare(a[0], b[0]) || compare(a[1], b[1]) || compare(a[2], b[2]);
  });
}

/** Use only supplied model facts; missing facts do not manufacture choices. */
export function programFrameChoices(scope: FrameScope): ProgramFrameChoice[] {
  const choices: ProgramFrameChoice[] = [];
  const keyOf = (ref: ProgramFrameReference) => `${ref.component_id}\0${ref.frame_id}`;
  const moving = new Set((scope.movingFrames ?? []).map(keyOf));
  const frames = [...new Map([...(scope.frames ?? []), ...(scope.movingFrames ?? [])]
    .map((frame) => [keyOf(frame), frame] as const)).values()];
  frames.sort((a, b) => Number(moving.has(keyOf(b))) - Number(moving.has(keyOf(a))));
  for (const reference of frames) {
    const frame: ProgramPoseFrame = { kind: 'target_frame', frame: { ...reference } };
    choices.push({ key: frameKey(frame), frame, label: frameLabel(frame),
      group: moving.has(keyOf(reference)) ? `Moving robot · ${scope.movingComponentId ?? reference.component_id}`
        : scope.movingComponentId === undefined ? 'Robots' : 'Other robots' });
  }
  choices.push({ key: 'cell', frame: { kind: 'cell_frame' }, label: 'Cell', group: 'Cell' });
  for (const resource of scope.program.scene ?? []) {
    const frame: ProgramPoseFrame = { kind: 'scene_resource', scene_resource_id: resource.scene_resource_id };
    choices.push({ key: frameKey(frame), frame, label: frameLabel(frame), group: 'Scene objects' });
  }
  if (scope.allowedFrames === undefined) return choices;
  const allowed = new Set(scope.allowedFrames.map(frameKey));
  return choices.filter((choice) => allowed.has(choice.key));
}

export interface FrameOffer {
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
    const selected = choice.frame.frame;
    return offer.otherRobots || (scope.movingFrames ?? []).some((frame) =>
      frame.component_id === selected.component_id && frame.frame_id === selected.frame_id);
  });
}
