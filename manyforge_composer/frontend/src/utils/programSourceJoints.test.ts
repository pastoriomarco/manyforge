import { describe, expect, it } from 'vitest';
import type { ProgramTreeNode } from '../types/api';
import { programSourceJointInventory } from './programSourceJoints';

const target = { component_id: 'robot', command_target_id: 'arm' };
const node: ProgramTreeNode = {
  node_id: 'motion.move_to_joint_target', instance_id: 'move',
  parameters: { target, goal: [] }, children: [],
};

describe('Program source-joint inventory', () => {
  it('uses the exact candidate target and preserves unique joint order', () => {
    expect(programSourceJointInventory(node, [{ target, source_joint_names: ['joint1', 'joint2', 'joint1'] }]))
      .toEqual({ jointNames: ['joint1', 'joint2'], target, unavailableReason: null });
  });
  it('never borrows another target or unavailable candidate', () => {
    expect(programSourceJointInventory(node, null).unavailableReason).toContain('Deployment Draft');
    expect(programSourceJointInventory(node, [{ target: { ...target, component_id: 'other' }, source_joint_names: ['x'] }])
      .unavailableReason).toContain('absent or ambiguous');
    expect(programSourceJointInventory(node, [
      { target, source_joint_names: ['a'] }, { target, source_joint_names: ['b'] },
    ]).unavailableReason).toContain('absent or ambiguous');
  });
  it('requires an explicit target and never invents joints', () => {
    expect(programSourceJointInventory({ ...node, parameters: {} }, []).target).toBeNull();
    expect(programSourceJointInventory(node, [{ target, source_joint_names: [] }]).unavailableReason)
      .toContain('no commandable source joints');
  });
});
