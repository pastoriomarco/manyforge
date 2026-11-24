import { describe, expect, it } from 'vitest';
import type { ProgramTreeNode } from '../types/api';
import { activeProgramWaits } from './programWaits';

const leaf = (instance_id: string, node_id: string, parameters: ProgramTreeNode['parameters'],
  value_sources?: ProgramTreeNode['value_sources']): ProgramTreeNode => ({
  instance_id, node_id, parameters, children: [], ...(value_sources ? { value_sources } : {}),
});

const tree: ProgramTreeNode = {
  instance_id: 'root', node_id: 'control.parallel', parameters: {}, children: [
    leaf('robot_1_wait_cycle_on', 'logic.wait_for_value', { key: 'cell.cycle_on', operator: '==', value: true }),
    leaf('loader_wait_cycle_on', 'logic.wait_for_value', { key: 'cell.cycle_on', operator: '==', value: true }),
    leaf('wait_vice', 'io.wait_for_input',
      { signal: { component_id: 'lite6_controller_io', signal_name: 'CI3' }, expected_value: true, edge: 'rising' }),
    leaf('wait_closed', 'gripper.wait_for_status',
      { target: { component_id: 'lite_gripper', command_target_id: 'gripper' }, expected_state: 'closed', timeout_s: 2 }),
    leaf('wait_limit', 'logic.wait_for_value', { key: 'cell.count', operator: '>=', value: 0 },
      { value: { kind: 'blackboard_key', reference: 'cell.limit' } }),
    leaf('idle_wait', 'logic.wait_for_value', { key: 'cell.other', value: 1 }),
    leaf('move', 'motion.move_to_pose', {}),
  ],
};

describe('activeProgramWaits (OD-354)', () => {
  it('lists the running wait nodes, grouped by condition, in tree order', () => {
    const running = Object.fromEntries([
      'robot_1_wait_cycle_on', 'loader_wait_cycle_on', 'wait_vice', 'wait_closed', 'wait_limit', 'move',
    ].map((id) => [id, 'running']));
    expect(activeProgramWaits(tree, { ...running, idle_wait: 'idle', root: 'running' })).toEqual([
      { condition: 'cell.cycle_on == true', nodeInstanceIds: ['robot_1_wait_cycle_on', 'loader_wait_cycle_on'] },
      { condition: 'input lite6_controller_io/CI3 = true (rising edge)', nodeInstanceIds: ['wait_vice'] },
      { condition: 'gripper lite_gripper closed', nodeInstanceIds: ['wait_closed'] },
      { condition: 'cell.count >= key cell.limit', nodeInstanceIds: ['wait_limit'] },
    ]);
  });

  it('ignores held and finished waits', () => {
    expect(activeProgramWaits(tree, { robot_1_wait_cycle_on: 'held', loader_wait_cycle_on: 'success' })).toEqual([]);
  });
});
