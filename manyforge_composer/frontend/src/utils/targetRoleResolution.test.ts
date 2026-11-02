import { describe, expect, it } from 'vitest';

import type { JsonValue, ProgramAuthoringDocument, ProgramTreeNode } from '../types/api';
import {
  TRIAL_E_CONDITION, trialEAssignment, trialEDeployment, trialEProgram,
} from '../testFixtures/mockLite6SignalMoves';
import { conditionProblem, nodeConditionChoices, stopStagesForTargetRole } from './interruptAuthoring';
import { ioSignalRoleChoices } from './ioAuthoring';
import { mockSignalProfiles } from './mockSimulation';
import { precisionContactStopWarnings } from './precisionContactStopWarnings';
import {
  resolvedInputSignals, resolvedOutputSignals, resolvedTargetRoleBindings, resolvedTargetRoles,
} from './targetRoleResolution';

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

  it('offers the declared inputs of an assigned role and accepts the signal_moves condition', () => {
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

describe('binding-null consumers use the same resolver', () => {
  // station also requires its clamp output; curtain_blocked stays not required.
  const withClamp: ProgramAuthoringDocument = { ...program, target_roles: { ...program.target_roles,
    station: { component_role: 'station', required_resource_roles: ['edge_sensor', 'probe_contact', 'clamp'] } } };
  const facts = (deploymentDocument: Record<string, JsonValue> | null = trialEDeployment(),
    roleAssignment: Record<string, JsonValue> | null = trialEAssignment, roles = withClamp) => (
    { program: roles, roleAssignment, deploymentDocument });
  /** The binding an Apply of the same facts compiles (binding-row shape). */
  const equivalentBinding = (roles = withClamp) => ({ target_role_bindings: resolvedTargetRoleBindings(
    roles, trialEAssignment, trialEDeployment(),
  ).map((row) => ({ ...row, signal_role_bindings: row.signal_role_bindings.map((signal) => ({
    ...signal, transition_capability: 'sampled_only', signal_descriptor_digest: `sha256:${'c'.repeat(64)}` })) })) }) as never;

  it('resolves declared outputs filtered by required_resource_roles', () => {
    const station = resolvedTargetRoles(withClamp, trialEAssignment)[2];
    expect(resolvedOutputSignals(withClamp, station, trialEDeployment())).toEqual([
      { resource_role_id: 'clamp', signal_name: 'clamp', value_type: 'bool' },
    ]);
    // clamp is declared but not a required resource role of the signal_moves station.
    expect(resolvedOutputSignals(program, resolvedTargetRoles(program, trialEAssignment)[2], trialEDeployment()))
      .toEqual([]);
    expect(resolvedTargetRoleBindings(withClamp, trialEAssignment, null)).toEqual([]);
    expect(resolvedTargetRoleBindings(withClamp, null, trialEDeployment())).toEqual([]);
  });

  it('offers io nodes the same signal choices as the equivalent binding', () => {
    for (const nodeId of ['io.wait_for_input', 'io.get_input', 'io.check_input', 'io.set_output']) {
      const resolved = ioSignalRoleChoices(nodeId, 'station', null, facts());
      expect(resolved, nodeId).toEqual(ioSignalRoleChoices(nodeId, 'station', equivalentBinding()));
      expect(resolved, nodeId).not.toBeNull();
    }
    expect(ioSignalRoleChoices('io.wait_for_input', 'station', null, facts())?.map((choice) => choice.label)).toEqual([
      'edge_sensor · edge_sensor (input)', 'probe_contact · probe_contact (input)',
    ]);
    expect(ioSignalRoleChoices('io.set_output', 'station', null, facts())).toEqual([
      { value: 'clamp', label: 'clamp · clamp (output)', group: 'station output signals' },
    ]);
    // Not required → not offered: the "no output signal" hint case, as with the binding.
    expect(ioSignalRoleChoices('io.set_output', 'station', null, facts(trialEDeployment(), trialEAssignment, program)))
      .toEqual([]);
    expect(ioSignalRoleChoices('io.set_output', 'station', equivalentBinding(program))).toEqual([]);
    // A resolved role with no declared signal: empty, as the binding path.
    expect(ioSignalRoleChoices('io.get_input', 'arm', null, facts())).toEqual([]);
  });

  it('lets a non-null binding take precedence over a conflicting Draft', () => {
    // The Draft resolves station → station_io/io with clamp; the binding says otherwise.
    const binding = { target_role_bindings: [{
      target_role_id: 'station', controllable_component_id: 'other_io', command_target_id: 'io',
      signal_role_bindings: [{ resource_role_id: 'lamp', signal_name: 'DO7', value_type: 'bool',
        direction: 'output', transition_capability: 'sampled_only', signal_descriptor_digest: `sha256:${'d'.repeat(64)}` }],
    }] } as never;
    expect(ioSignalRoleChoices('io.set_output', 'station', binding, facts())).toEqual([
      { value: 'lamp', label: 'lamp · DO7 (output)', group: 'station output signals' },
    ]);
    expect(ioSignalRoleChoices('io.get_input', 'station', binding, facts())).toEqual([]);
    // The binding's other_io has no declared source in the Draft: no profile from the Draft target.
    expect(mockSignalProfiles([{ targetRole: 'station', resourceRole: 'probe_contact' }], binding,
      trialEDeployment(), program, trialEAssignment).size).toBe(0);
  });

  it('keeps the ordinary resource-role picker when facts are missing', () => {
    expect(ioSignalRoleChoices('io.get_input', 'station', null)).toBeNull();
    expect(ioSignalRoleChoices('io.get_input', 'station', null, facts(null))).toBeNull();
    expect(ioSignalRoleChoices('io.get_input', 'station', null, facts(trialEDeployment(), null))).toBeNull();
    expect(ioSignalRoleChoices('io.get_input', 'unassigned', null, facts())).toBeNull();
    expect(ioSignalRoleChoices('io.get_input', undefined, null, facts())).toBeNull();
  });

  it('finds the Stop stages of the resolved robot target, as the binding path does', () => {
    const response = { controlBindingId: 'c', controlBindingRevision: 'r', controlRouteId: 'route',
      nominalStopMode: null, stages: [
        { stageId: 'motion_controlled_stop', stageKind: 'nominal', commandKind: 'controlled_stop' },
        { stageId: 'goal_cancel', stageKind: 'controlled_fallback', commandKind: 'controller_goal_cancel' },
      ] };
    const chains = [
      { controllableComponentId: 'lite6_robot', commandTargetId: 'gripper' },
      { controllableComponentId: 'lite6_robot', commandTargetId: 'arm', stopResponse: response },
    ];
    const rows = resolvedTargetRoles(program, trialEAssignment);
    const resolved = stopStagesForTargetRole(chains, rows, 'arm');
    expect(resolved?.map((stage) => stage.id)).toEqual(['motion_controlled_stop', 'goal_cancel']);
    expect(resolved).toEqual(stopStagesForTargetRole(chains, (equivalentBinding(program) as {
      target_role_bindings: typeof rows }).target_role_bindings, 'arm'));
    // Unpublished chain or missing assignment: "available after Apply" as today.
    expect(stopStagesForTargetRole(chains, rows, 'gripper')).toBeNull();
    expect(stopStagesForTargetRole(chains, resolvedTargetRoles(program, null), 'arm')).toBeNull();
  });

  it('gives Mock simulation rows the source profile of the resolved target', () => {
    const signals = [{ targetRole: 'station', resourceRole: 'probe_contact' },
      { targetRole: 'station', resourceRole: 'gone' }];
    const resolved = mockSignalProfiles(signals, null, trialEDeployment(), program, trialEAssignment);
    expect(Object.fromEntries(resolved)).toEqual({ 'station/probe_contact': 'mock' });
    expect(resolved).toEqual(mockSignalProfiles(signals, equivalentBinding(program), trialEDeployment()));
    // Missing facts: unknown profiles (editable), as today.
    expect(mockSignalProfiles(signals, null, trialEDeployment()).size).toBe(0);
    expect(mockSignalProfiles(signals, null, trialEDeployment(), program, null).size).toBe(0);
    expect(mockSignalProfiles(signals, null, null, program, trialEAssignment).size).toBe(0);
  });
});
