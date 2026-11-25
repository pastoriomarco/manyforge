import { describe, expect, it } from 'vitest';
import type { ProgramTreeNode } from '../types/api';
import { activeProgramWaits, programWaitLine } from './programWaits';
import type { SignalSample } from '../types/components';

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
      { condition: 'input lite6_controller_io/CI3 = true (rising edge)', nodeInstanceIds: ['wait_vice'],
        signalId: 'lite6_controller_io/CI3' },
      { condition: 'gripper lite_gripper closed', nodeInstanceIds: ['wait_closed'] },
      { condition: 'cell.count >= key cell.limit', nodeInstanceIds: ['wait_limit'] },
    ]);
  });

  it('ignores held and finished waits', () => {
    expect(activeProgramWaits(tree, { robot_1_wait_cycle_on: 'held', loader_wait_cycle_on: 'success' })).toEqual([]);
  });

  it('shows an input wait with its input\'s current pushed value, marked when stale', () => {
    const [wait] = activeProgramWaits(tree, { wait_vice: 'running' });
    const sample = (value: SignalSample['value'], fresh: boolean): SignalSample => ({
      signal_id: 'lite6_controller_io/CI3', value, source: 'device', age_ns: 40e9, fresh,
    });
    expect(programWaitLine(wait, [sample(false, false)]))
      .toBe('input lite6_controller_io/CI3 = true (rising edge) · now false (stale) — wait_vice');
    expect(programWaitLine(wait, [sample(true, true)]))
      .toBe('input lite6_controller_io/CI3 = true (rising edge) · now true — wait_vice');
    expect(programWaitLine(wait, [sample(null, false)])).toContain('· now unknown (stale) —');
    // No sample, or the samples are not known: the condition alone.
    expect(programWaitLine(wait, [])).toBe('input lite6_controller_io/CI3 = true (rising edge) — wait_vice');
    expect(programWaitLine(wait, null)).toBe('input lite6_controller_io/CI3 = true (rising edge) — wait_vice');
  });

  it('names no input for a wait whose signal comes from a key', () => {
    const keyed: ProgramTreeNode = leaf('wait_keyed', 'io.wait_for_input', { expected_value: true },
      { signal: { kind: 'blackboard_key', reference: 'cell.signal' } });
    const [wait] = activeProgramWaits({ ...tree, children: [keyed] }, { wait_keyed: 'running' });
    expect(wait.signalId).toBeUndefined();
    expect(programWaitLine(wait, [])).toBe('input key cell.signal = true — wait_keyed');
  });

  it('shows how long a wait has run and the key or gripper value the runner read', () => {
    const waits = activeProgramWaits(tree, { robot_1_wait_cycle_on: 'running', loader_wait_cycle_on: 'running', wait_closed: 'running' });
    const observation = {
      runningSinceUtcNs: { robot_1_wait_cycle_on: '1795514405000000000', loader_wait_cycle_on: '1795514400000000000' },
      waitValues: {
        loader_wait_cycle_on: { kind: 'key' as const, key: 'cell.cycle_on', value: false },
        wait_closed: { kind: 'gripper' as const, componentId: 'lite_gripper', state: 'open' },
      },
      nowMs: 1_795_514_440_900,
    };
    // The earliest waiting node counts; whole seconds.
    expect(programWaitLine(waits[0], null, observation))
      .toBe('cell.cycle_on == true · now false · 40 s — robot_1_wait_cycle_on, loader_wait_cycle_on');
    expect(programWaitLine(waits[1], null, observation)).toBe('gripper lite_gripper closed · now open — wait_closed');
    // An older status without these members: the line as before.
    expect(programWaitLine(waits[0], null, { nowMs: 0 }))
      .toBe('cell.cycle_on == true — robot_1_wait_cycle_on, loader_wait_cycle_on');
  });
});
