import { describe, expect, it } from 'vitest';

import type { ProgramTreeNode } from '../types/api';
import { manualFillPreviewValues } from './manualFillPreview';

function node(parameters: ProgramTreeNode['parameters'], value_sources: ProgramTreeNode['value_sources'] = {}) {
  return {
    instance_id: 'move',
    node_id: 'motion.move_to_pose',
    children: [],
    parameters,
    value_sources,
  } as ProgramTreeNode;
}

describe('manual fill field previews', () => {
  it('derives field markers from the exact parameter payload and source removals', () => {
    const selected = node({
      target_mode: 'relative',
      base_frame: { component_id: 'cell', frame_id: 'cell_base' },
      goal: {
        frame_ref: { kind: 'cell_frame' },
        position_m: [0, 0, 0],
        orientation: { xyzw: [0, 0, 0, 1] },
      },
    }, {
      'goal.position_m[1]': { kind: 'program_parameter', reference: 'y' },
    });
    const next = {
      base_frame: { component_id: 'arm', frame_id: 'arm_base' },
      goal: {
        frame_ref: { kind: 'target_frame', frame: { component_id: 'arm', frame_id: 'arm_base' } },
        position_m: [0.2, 0.4, 0.6],
        orientation: { xyzw: [0.1, 0.2, 0.3, 0.9] },
      },
    };
    const changes = manualFillPreviewValues(selected, next, {});

    expect(changes.get('target_mode')).toBe('default');
    expect(changes.get('base_frame')).toBe('arm · arm_base frame');
    expect(changes.get('goal.frame_ref')).toBe('arm · arm_base frame');
    expect(changes.get('goal.position_m[0]')).toBe('0.2');
    expect(changes.get('goal.position_m[1]')).toBe('0.4');
    expect(changes.get('goal.orientation.xyzw[3]')).toBe('0.9');
    expect(changes.size).toBe(10);
  });

  it('rounds previewed numbers to four decimals', () => {
    const changes = manualFillPreviewValues(node({ goal: { position_m: [0, 0, 0] } }),
      { goal: { position_m: [0.123456, -0.00004, 1.5] } }, {});

    expect(changes.get('goal.position_m[0]')).toBe('0.1235');
    expect(changes.get('goal.position_m[1]')).toBe('0');
    expect(changes.get('goal.position_m[2]')).toBe('1.5');
  });

  it('uses joint names for the target rows instead of list positions', () => {
    const selected = node({ goal: [
      { joint_name: 'joint_b', position_rad: 0.2 },
      { joint_name: 'joint_a', position_rad: 0.1 },
    ] }, {
      'goal[0].position_rad': { kind: 'program_parameter', reference: 'joint_b_position' },
    });
    const changes = manualFillPreviewValues(selected, { goal: [
      { joint_name: 'joint_a', position_rad: -0.4 },
      { joint_name: 'joint_b', position_rad: 0.7 },
    ] }, {});

    expect(changes.get('goal[joint_a].position_rad')).toBe('-0.4');
    expect(changes.get('goal[joint_b].position_rad')).toBe('0.7');
    expect(changes.has('goal[0].position_rad')).toBe(false);
    expect(changes.size).toBe(2);
  });
});
