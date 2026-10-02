import { describe, expect, it } from 'vitest';

import type { ProgramDocument } from '../types/api';
import {
  canonicalFrames,
  offeredFrameChoices,
  poseFrameOf,
  programFrameChoices,
  type ProgramPoseFrame,
} from './programFrames';

const program = {
  target_roles: {
    arm: { required_frame_roles: ['base', 'tcp'] },
    peer: { required_frame_roles: ['base', 'tcp'] },
  },
  scene: [{ scene_resource_id: 'table' }],
} as unknown as Pick<ProgramDocument, 'target_roles' | 'scene'>;

describe('program frames', () => {
  it('lists the moving robot first, then other robots by name, the cell and Scene objects', () => {
    const choices = programFrameChoices({ program, movingTargetRole: 'peer' });
    expect(choices.map((choice) => [choice.group, choice.label])).toEqual([
      ['Moving robot · peer', 'peer · base'],
      ['Moving robot · peer', 'peer · tcp'],
      ['Other robots', 'arm · base'],
      ['Other robots', 'arm · tcp'],
      ['Cell', 'Cell'],
      ['Scene objects', 'table'],
    ]);
    // The same frame name on two robots stays two frames.
    expect(new Set(choices.map((choice) => choice.key)).size).toBe(choices.length);
    expect(programFrameChoices({ program }).slice(0, 4).map((choice) => choice.group))
      .toEqual(['Robots', 'Robots', 'Robots', 'Robots']);
  });

  it('offers only a key’s allowed frames and a field’s accepted kinds', () => {
    const allowed: ProgramPoseFrame[] = [
      { kind: 'scene_resource', scene_resource_id: 'table' },
      { kind: 'target_frame', target_role: 'arm', frame_role: 'tcp' },
    ];
    expect(programFrameChoices({ program, allowedFrames: allowed }).map((choice) => choice.key))
      .toEqual(['robot:arm:tcp', 'scene:table']);
    expect(offeredFrameChoices(
      { program, movingTargetRole: 'arm' },
      { otherRobots: false, cell: false, scene: false },
    ).map((choice) => choice.key)).toEqual(['robot:arm:base', 'robot:arm:tcp']);
  });

  it('orders allowed frames canonically like the Program validator and drops duplicates', () => {
    const tcp: ProgramPoseFrame = { kind: 'target_frame', target_role: 'arm', frame_role: 'tcp' };
    expect(canonicalFrames([
      tcp,
      { kind: 'scene_resource', scene_resource_id: 'table' },
      { kind: 'target_frame', target_role: 'arm', frame_role: 'base' },
      { kind: 'cell_frame' },
      tcp,
    ])).toEqual([
      { kind: 'cell_frame' },
      { kind: 'scene_resource', scene_resource_id: 'table' },
      { kind: 'target_frame', target_role: 'arm', frame_role: 'base' },
      tcp,
    ]);
  });

  it('reads only closed pose frames', () => {
    expect(poseFrameOf({ kind: 'cell_frame' })).toEqual({ kind: 'cell_frame' });
    expect(poseFrameOf({ kind: 'cell_frame', cell_id: 'c' })).toBeNull();
    expect(poseFrameOf({ kind: 'target_frame', target_role: 'arm' })).toBeNull();
    expect(poseFrameOf({ kind: 'scene_resource', scene_resource_id: '' })).toBeNull();
  });
});
