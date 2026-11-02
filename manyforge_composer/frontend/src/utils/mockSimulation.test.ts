import { describe, expect, it } from 'vitest';

import type { JsonValue, ProgramTreeNode } from '../types/api';
import {
  conditionSignals,
  mockSignalProfiles,
  mockSimulationProblem,
  nodeMockSignals,
  parseMockSimulation,
  serializeMockSimulation,
} from './mockSimulation';

const door = { target_role: 'cell', resource_role: 'door' };
const part = { target_role: 'cell', resource_role: 'part' };
const condition: JsonValue = {
  all: [
    { compare: { operand: { signal: door }, op: '==', value: true } },
    { not: { compare: { operand: { signal: part }, op: '==', value: false } } },
    { compare: { operand: { key: 'count' }, op: '>', value: 2 } },
  ],
};

function node(mock: JsonValue | undefined, patch: Partial<ProgramTreeNode> = {}): ProgramTreeNode {
  return {
    node_id: 'motion.move_until_signal',
    instance_id: 'approach',
    parameters: { interrupt_condition: condition, ...(mock === undefined ? {} : { mock_simulation: mock }) },
    children: [],
    ...patch,
  };
}

describe('mockSimulation', () => {
  it('lists the signals a condition reads, unique and sorted', () => {
    expect(conditionSignals(condition)).toEqual([
      { targetRole: 'cell', resourceRole: 'door' }, { targetRole: 'cell', resourceRole: 'part' },
    ]);
    expect(nodeMockSignals(node(undefined, { parameters: {} }))).toEqual([]);
    expect(nodeMockSignals({ node_id: 'io.wait_for_input', parameters: { target_role: 'cell', resource_role: 'door' } }))
      .toEqual([{ targetRole: 'cell', resourceRole: 'door' }]);
    expect(nodeMockSignals({ node_id: 'motion.move_to_pose', parameters: { interrupt_condition: condition } })).toEqual([]);
  });

  it('accepts valid rows and round-trips them exactly', () => {
    const rows: JsonValue[] = [
      { signal: door, start_value: false, change: { after_ms: 0, value: true } },
      { signal: part, start_value: true },
    ];
    expect(mockSimulationProblem(node(rows))).toBeNull();
    expect(mockSimulationProblem(node(undefined))).toBeNull();
    const parsed = parseMockSimulation(rows);
    expect(parsed.ok && serializeMockSimulation(parsed.rows)).toEqual(rows);
  });

  it('refuses a signal the node does not read', () => {
    const other = { target_role: 'cell', resource_role: 'lamp' };
    expect(mockSimulationProblem(node([{ signal: other, start_value: true }])))
      .toMatch(/^mock_simulation_signal_unused: signal cell\/lamp/);
  });

  it('refuses a second row for one signal', () => {
    expect(mockSimulationProblem(node([{ signal: door, start_value: true }, { signal: door, start_value: false }])))
      .toMatch(/^mock_simulation_signal_duplicate: signal cell\/door/);
  });

  it.each([
    [{ signal: door, start_value: true, change: { after_ms: -1, value: true } }],
    [{ signal: door, start_value: true, change: { after_ms: 1.5, value: true } }],
    [{ signal: door, start_value: true, change: { after_ms: '5', value: true } }],
    [{ signal: door, start_value: true, change: { value: true } }],
    [{ signal: door, start_value: 'on' }],
    [{ signal: { ...door, extra: 1 }, start_value: true }],
    [{ signal: door, start_value: true, delay: 1 }],
  ] as JsonValue[][])('refuses the invalid row %o', (row) => {
    expect(mockSimulationProblem(node([row]))).toMatch(/^mock_simulation_invalid: /);
  });

  it('refuses a value source anywhere inside the rows', () => {
    for (const path of ['mock_simulation', 'mock_simulation[0].start_value', 'mock_simulation[0].change.value']) {
      expect(mockSimulationProblem(node([{ signal: door, start_value: true }], {
        value_sources: { [path]: { kind: 'blackboard_key', reference: 'flag' } },
      }))).toMatch(/^mock_simulation_value_source_refused: /);
    }
  });

  it('finds each signal’s declared source profile through the binding', () => {
    const io = (resource: string, profile: string, extra: Record<string, JsonValue>) => ({
      controllable_component_id: 'station', command_target_id: 'io', resource_role_id: resource,
      signal_name: resource, source_id: resource, value_type: 'bool', direction: 'input',
      transition_capability: 'sampled_only', ...extra, provider: { profile, ...(extra.provider as object) },
    });
    const document = { components: [{ configuration: { io_signals: [
      io('door', 'mock', { update_mode: 'change_only', freshness: { mode: 'provider_validity' } }),
      io('part', 'ros_bool', { update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 1 },
        provider: { topic: '/part', liveness: { mode: 'periodic' } } }),
    ] } }] } as unknown as Record<string, JsonValue>;
    const binding = { target_role_bindings: [{
      target_role_id: 'cell', controllable_component_id: 'station', command_target_id: 'io',
    }] } as never;
    const profiles = mockSignalProfiles(conditionSignals(condition), binding, document);
    expect(Object.fromEntries(profiles)).toEqual({ 'cell/door': 'mock', 'cell/part': 'ros_bool' });
    expect(mockSignalProfiles(conditionSignals(condition), null, document).size).toBe(0);
  });
});
