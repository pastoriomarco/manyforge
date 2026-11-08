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

const door = { component_id: 'station', signal_name: 'door' };
const part = { component_id: 'station', signal_name: 'part' };
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
      { component_id: 'station', signal_name: 'door' }, { component_id: 'station', signal_name: 'part' },
    ]);
    expect(nodeMockSignals(node(undefined, { parameters: {} }))).toEqual([]);
    expect(nodeMockSignals({ node_id: 'io.wait_for_input', parameters: { signal: door } }))
      .toEqual([{ component_id: 'station', signal_name: 'door' }]);
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
    const other = { component_id: 'station', signal_name: 'lamp' };
    expect(mockSimulationProblem(node([{ signal: other, start_value: true }])))
      .toContain('mock_simulation_signal_unused: signal ["station","lamp"]');
  });

  it('refuses a second row for one signal', () => {
    expect(mockSimulationProblem(node([{ signal: door, start_value: true }, { signal: door, start_value: false }])))
      .toContain('mock_simulation_signal_duplicate: signal ["station","door"]');
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

  it('uses only exact qualified candidate signal profiles', () => {
    const facts = [
      { reference: door, ambiguous: false, value_type: 'bool', direction: 'input' as const, provider_profile: 'mock' },
      { reference: part, ambiguous: false, value_type: 'bool', direction: 'input' as const, provider_profile: 'ros_bool' },
    ];
    expect(Object.fromEntries(mockSignalProfiles(conditionSignals(condition), facts))).toEqual({
      '["station","door"]': 'mock', '["station","part"]': 'ros_bool',
    });
    expect(mockSignalProfiles(conditionSignals(condition), null).size).toBe(0);
    expect(mockSignalProfiles(conditionSignals(condition), [{ ...facts[0], ambiguous: true }]).size).toBe(0);
  });
});
