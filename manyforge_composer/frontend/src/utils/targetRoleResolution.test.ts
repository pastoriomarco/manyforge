import { describe, expect, it } from 'vitest';

import type { JsonValue, ProgramTreeNode } from '../types/api';
import {
  TRIAL_E_CONDITION, trialEAssignment, trialEDeployment, trialEProgram,
} from '../testFixtures/trialESignal';
import { conditionProblem, nodeConditionChoices } from './interruptAuthoring';
import { precisionContactStopWarnings } from './precisionContactStopWarnings';
import { resolvedInputSignals, resolvedTargetRoles } from './targetRoleResolution';

const program = trialEProgram([]);
const UNBOUND = 'condition: {"signal":{"resource_role":"probe_contact","target_role":"station"}} is not a bound input signal or a declared scalar key.';

describe('target roles from the Draft facts (binding null)', () => {
  it('resolves every assigned Program target role to its component and command target', () => {
    expect(resolvedTargetRoles(program, trialEAssignment)).toEqual([
      { target_role_id: 'arm', controllable_component_id: 'lite6_robot', command_target_id: 'arm' },
      { target_role_id: 'gripper', controllable_component_id: 'lite6_robot', command_target_id: 'gripper' },
      { target_role_id: 'station', controllable_component_id: 'station_io', command_target_id: 'io' },
    ]);
    expect(resolvedTargetRoles(program, null)).toEqual([]);
    const station = resolvedTargetRoles(program, trialEAssignment)[2];
    // curtain_blocked is declared but not a required resource role of station.
    expect(resolvedInputSignals(program, station, trialEDeployment())).toEqual([
      { resource_role_id: 'edge_sensor', signal_name: 'edge_sensor', value_type: 'bool' },
      { resource_role_id: 'probe_contact', signal_name: 'probe_contact', value_type: 'bool' },
    ]);
  });

  it('offers the declared inputs of an assigned role and accepts the Trial E condition', () => {
    const choices = nodeConditionChoices(program, null, [], trialEDeployment(), trialEAssignment);
    expect(choices.filter((choice) => choice.key === '').map((choice) => choice.label)).toEqual([
      'station · edge_sensor (edge_sensor, input)',
      'station · probe_contact (probe_contact, input)',
    ]);
    expect(conditionProblem(TRIAL_E_CONDITION, choices)).toBeNull();
  });

  it('keeps the existing refusal when the assignment or the Draft is unavailable', () => {
    for (const choices of [
      nodeConditionChoices(program, null, [], trialEDeployment(), null),
      nodeConditionChoices(program, null, [], null, trialEAssignment),
    ]) {
      expect(choices.some((choice) => choice.key === '')).toBe(false);
      expect(conditionProblem(TRIAL_E_CONDITION, choices)).toBe(UNBOUND);
    }
  });

  it('still refuses a truly unbound signal with the same message', () => {
    // The role is not assigned: no row for station.
    const unassigned = { ...trialEAssignment, target_roles: [{ command_target_id: 'arm', target_role_id: 'arm' }] };
    expect(conditionProblem(TRIAL_E_CONDITION,
      nodeConditionChoices(program, null, [], trialEDeployment(), unassigned))).toBe(UNBOUND);
    // The Draft declares no input row for a required role.
    expect(conditionProblem(TRIAL_E_CONDITION, nodeConditionChoices(
      program, null, [], trialEDeployment(false, ['edge_sensor']), trialEAssignment))).toBe(UNBOUND);
    // A declared input outside station's required_resource_roles is not offered.
    const choices = nodeConditionChoices(program, null, [], trialEDeployment(), trialEAssignment);
    expect(choices.some((choice) => choice.label.includes('curtain_blocked'))).toBe(false);
    const extra: JsonValue = { compare: { op: '==', value: true,
      operand: { signal: { resource_role: 'curtain_blocked', target_role: 'station' } } } };
    expect(conditionProblem(extra, choices)).toBe(
      'condition: {"signal":{"resource_role":"curtain_blocked","target_role":"station"}} is not a bound input signal or a declared scalar key.',
    );
    // Undeclared keys and bad literals are refused as before.
    expect(conditionProblem({ compare: { op: '==', value: true, operand: { key: 'nope' } } }, choices))
      .toBe('condition: {"key":"nope"} is not a bound input signal or a declared scalar key.');
    expect(conditionProblem({ compare: { ...(TRIAL_E_CONDITION as { compare: Record<string, JsonValue> }).compare, value: 1 } }, choices))
      .toBe('condition: the value must be a bool literal.');
  });

  it('feeds the probe Stop warning through the same resolver', () => {
    const probe: ProgramTreeNode = {
      node_id: 'motion.probe_search', instance_id: 'probe', children: [],
      parameters: { target_role: 'arm', stop_mode: 'soft_on_path' },
    };
    const warnings = precisionContactStopWarnings(
      probe, trialEDeployment(true), resolvedTargetRoles(program, trialEAssignment),
    );
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toContain('(lite6_robot · arm)');
    expect(precisionContactStopWarnings(probe, trialEDeployment(true), resolvedTargetRoles(program, null)))
      .toEqual([]);
  });
});
