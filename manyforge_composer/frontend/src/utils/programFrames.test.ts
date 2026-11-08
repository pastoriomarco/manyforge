import { describe, expect, it } from 'vitest';

import type { ProgramDocument, ProgramFrameReference } from '../types/api';
import { canonicalFrames, offeredFrameChoices, poseFrameOf, programFrameChoices } from './programFrames';

const program = { scene: [{ scene_resource_id: 'table' }] } as unknown as Pick<ProgramDocument, 'scene'>;
const frames: ProgramFrameReference[] = [
  { component_id: 'arm', frame_id: 'base' },
  { component_id: 'arm', frame_id: 'tcp' },
  { component_id: 'peer', frame_id: 'base' },
  { component_id: 'peer', frame_id: 'tcp' },
];

describe('program frames', () => {
  it('lists only captured component-frame facts and preserves component ownership', () => {
    const choices = programFrameChoices({ program, frames, movingComponentId: 'peer', movingFrames: frames.slice(2) });
    expect(choices.map((choice) => [choice.group, choice.label])).toEqual([
      ['Moving robot · peer', 'peer · base'], ['Moving robot · peer', 'peer · tcp'],
      ['Other robots', 'arm · base'], ['Other robots', 'arm · tcp'],
      ['Cell', 'Cell'], ['Scene objects', 'table'],
    ]);
    expect(new Set(choices.map((choice) => choice.key)).size).toBe(choices.length);
    expect(programFrameChoices({ program }).map((choice) => choice.key)).toEqual(['cell', 'scene:table']);
  });

  it('filters exact direct frame pairs and offered kinds', () => {
    const allowed = [
      { kind: 'scene_resource' as const, scene_resource_id: 'table' },
      { kind: 'target_frame' as const, frame: { component_id: 'arm', frame_id: 'tcp' } },
    ];
    expect(programFrameChoices({ program, frames, allowedFrames: allowed }).map((choice) => choice.key))
      .toEqual(['component:arm:tcp', 'scene:table']);
    expect(offeredFrameChoices({ program, frames, movingComponentId: 'arm', movingFrames: frames.slice(0, 2) },
      { otherRobots: false, cell: false, scene: false }).map((choice) => choice.key))
      .toEqual(['component:arm:base', 'component:arm:tcp']);
  });

  it('retains a mounted-tool frame in the exact moving-target selection', () => {
    const tool = { component_id: 'tool', frame_id: 'tcp' };
    const choices = offeredFrameChoices({ program, frames: [...frames, tool],
      movingComponentId: 'arm', movingFrames: [frames[0]!, tool] },
    { otherRobots: false, cell: false, scene: false });
    expect(choices.map((choice) => choice.key)).toEqual(['component:arm:base', 'component:tool:tcp']);
  });

  it('parses closed v3 frames, rejects role aliases, and orders pairs canonically', () => {
    const arm = { kind: 'target_frame' as const, frame: { component_id: 'arm', frame_id: '2_TCP' } };
    const peer = { kind: 'target_frame' as const, frame: { component_id: 'peer', frame_id: '2_TCP' } };
    expect(poseFrameOf(arm)).toEqual(arm);
    expect(poseFrameOf({ kind: 'target_frame', target_role: 'arm', frame_role: 'tcp' })).toBeNull();
    expect(poseFrameOf({ kind: 'target_frame', frame: { component_id: 'arm', frame_id: 'tcp', extra: true } })).toBeNull();
    expect(poseFrameOf({ kind: 'target_frame', frame: { component_id: 'ARM', frame_id: 'tcp' } })).toBeNull();
    expect(canonicalFrames([peer, arm, arm])).toEqual([arm, peer]);
  });
});
