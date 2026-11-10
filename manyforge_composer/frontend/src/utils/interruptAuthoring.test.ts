import { describe, expect, it } from 'vitest';

import type {
  BlackboardKeyDeclaration, JsonValue, ProgramAuthoringDocument, ProgramTreeNode,
} from '../types/api';
import {
  canonicalScope,
  changedOutsideOutputs,
  conditionKeys,
  conditionProblem,
  deploymentInputSignals,
  globalConditionChoices,
  globalRuleChoices,
  globalRuleIds,
  globalRuleKeyNotes,
  globalRuleProblem,
  globalRuleScopeGroups,
  globalRuleScopeKind,
  INTERRUPT_KEY_INITIAL_REQUIRED,
  interruptConditionKeyUses,
  interruptKeyInitialProblem,
  newGlobalRule,
  nodeConditionChoices,
  operandId,
  unguardedNodeOutcomeKeys,
  commonStopStages,
  operatorsFor,
  qualifiedStopStages,
  successThroughStages,
  type GlobalInterruptRule,
} from './interruptAuthoring';
import { programBlackboardKeys } from './programAuthoring';

const candidateSignals = [{ reference: { component_id: 'arm', signal_name: 'DI0' }, ambiguous: false,
  value_type: 'bool', direction: 'input' as const }];

const program = {
  blackboard: {
    namespaces: [{ namespace_id: 'cell', owner: 'program' }],
    keys: [
      { key: 'cell.count', type: 'int64', access: 'read_write', persistence: 'run' },
      { key: 'cell.label', type: 'string', access: 'read', persistence: 'run' },
      { key: 'cell.pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
    ],
  },
  forest: [{ node_id: 'sequence', instance_id: 'main', parameters: {}, children: [
    { node_id: 'motion.move_until_signal', instance_id: 'seek', parameters: {}, children: [] },
    { node_id: 'motion.move_to_pose', instance_id: 'approach', parameters: {}, children: [] },
  ] }],
} as unknown as ProgramAuthoringDocument;

const signalOperand = { signal: { component_id: 'arm', signal_name: 'DI0' } };
const compare = (operand: JsonValue, op: string, value: JsonValue): JsonValue => ({ compare: { operand, op, value } });

describe('interrupt condition authoring', () => {
  const choices = nodeConditionChoices(program, candidateSignals, ['part_lost']);

  it('offers bound input signals of Program targets and every declared scalar key', () => {
    expect(choices.map((choice) => choice.label)).toEqual([
      'arm · DI0 (input)',
      'cell.count (int64)',
      'cell.label (string)',
      'part_lost (rule state)',
      'approach (node outcome)',
      'seek (node outcome)',
    ]);
    expect(choices.find((choice) => choice.key === 'node_outcome.seek')?.values)
      .toContain('no_signal_trip');
    expect(choices.find((choice) => choice.key === 'node_outcome.approach')?.values)
      .not.toContain('signal_trip');
    expect(choices.find((choice) => choice.key === 'interrupt_state.part_lost')?.values)
      .toEqual(['inactive', 'clear', 'tripped']);
  });

  it('lists outcome keys for the interrupt nodes and the global-rule state keys', () => {
    const keys = programBlackboardKeys(program, ['part_lost']).map((key) => key.key);
    expect(keys).toContain('node_outcome.seek');
    expect(keys).toContain('interrupt_state.part_lost');
    expect(programBlackboardKeys(program).map((key) => key.key)).not.toContain('interrupt_state.part_lost');
  });

  it('applies operators by type and checks literals and operands', () => {
    expect(operatorsFor('bool')).toEqual(['==', '!=']);
    expect(operatorsFor('string')).toEqual(['==', '!=']);
    expect(operatorsFor('float64')).toHaveLength(6);
    expect(conditionProblem(compare(signalOperand, '==', true), choices)).toBeNull();
    expect(conditionProblem(compare(signalOperand, '<', true), choices)).toContain('does not apply');
    expect(conditionProblem(compare({ key: 'cell.count' }, '>=', 3), choices)).toBeNull();
    expect(conditionProblem(compare({ key: 'cell.count' }, '>=', 2.5), choices)).toContain('int64');
    expect(conditionProblem(compare({ key: 'cell.count' }, '>=', 2 ** 53), choices)).toContain('2^53');
    expect(conditionProblem(compare({ key: 'cell.pose' }, '==', 1), choices)).toContain('not a candidate');
    expect(conditionProblem(compare({ key: 'missing' }, '==', 1), choices)).toContain('not a candidate');
    expect(conditionProblem({ all: [] }, choices)).toContain('at least one');
    expect(conditionProblem({ all: [], any: [] } as JsonValue, choices)).toContain('exactly one');
    expect(conditionProblem({ compare: { operand: signalOperand, op: '==', value: true, extra: 1 } }, choices))
      .toContain('exactly');
    expect(conditionProblem({ any: [compare(signalOperand, '==', true),
      { not: compare({ key: 'node_outcome.seek' }, '!=', 'completed') }] }, choices)).toBeNull();
  });

  it('enforces the depth and node limits only', () => {
    let deep: JsonValue = compare(signalOperand, '==', true);
    for (let level = 0; level < 8; level += 1) deep = { not: deep };
    expect(conditionProblem(deep, choices)).toContain('deeper than 8');
    const many = { all: Array.from({ length: 64 }, () => compare(signalOperand, '==', true)) };
    expect(conditionProblem(many, choices)).toContain('more than 64');
    const inputs = Array.from({ length: 17 }, (_, index) => ({
      id: JSON.stringify({ key: `k${index}` }), operand: { key: `k${index}` }, label: `k${index}`,
      key: `k${index}`, valueType: 'bool' as const,
    }));
    expect(conditionProblem({ any: inputs.map((input) => compare(input.operand, '==', true)) }, inputs))
      .toBeNull();
    const widest = Array.from({ length: 63 }, (_, index) => ({
      id: JSON.stringify({ key: `w${index}` }), operand: { key: `w${index}` }, label: `w${index}`,
      key: `w${index}`, valueType: 'bool' as const,
    }));
    expect(conditionProblem({ any: widest.map((input) => compare(input.operand, '==', true)) }, widest))
      .toBeNull();
    expect(conditionKeys({ all: [compare({ key: 'b' }, '==', 1), { not: compare({ key: 'a' }, '==', 1) }] }))
      .toEqual(['a', 'b']);
  });

  it('offers success stages from the entry stage downwards', () => {
    const stages = [{ id: 'one', label: 'A' }, { id: 'two', label: 'B' }, { id: 'three', label: 'C' }];
    expect(successThroughStages(stages, undefined).map((stage) => stage.id)).toEqual(['one', 'two', 'three']);
    expect(successThroughStages(stages, 'two').map((stage) => stage.id)).toEqual(['two', 'three']);
    expect(successThroughStages(stages, 'unknown')).toEqual([]);
  });
});

describe('generated carried-object-lost operands', () => {
  it('uses the exact candidate identity for node and global conditions', () => {
    const candidate = { reference: { component_id: 'robot', signal_name: 'carried_object_lost.gripper' },
      ambiguous: false, value_type: 'bool', direction: 'input' as const };
    const choices = nodeConditionChoices(program, [candidate], []);
    expect(conditionProblem(compare({ signal: candidate.reference }, '==', true), choices)).toBeNull();
    const global = globalRuleChoices({}, [], undefined, [{ controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: candidate.reference.signal_name, direction: 'input', valueType: 'bool' }]);
    expect(global.some((choice) => choice.id === operandId({ signal: {
      controllable_component_id: 'robot', signal_name: candidate.reference.signal_name } }))).toBe(true);
  });
});

describe('global interrupt rules', () => {
  const document = {
    components: [{ component_id: 'station', configuration: { runtime_configuration: { io_signals: [
      { controllable_component_id: 'station', signal_name: 'part_present', direction: 'input', value_type: 'bool' },
      { controllable_component_id: 'station', signal_name: 'clamp', direction: 'output', value_type: 'bool' },
    ] } } }],
    policies: { interrupt_rules: [{ rule_id: 'part_lost' }, { rule_id: 'Bad' }] },
  } as unknown as Record<string, JsonValue>;
  const choices = globalConditionChoices(deploymentInputSignals(document), []);
  const operand = { signal: { controllable_component_id: 'station', signal_name: 'part_present' } };

  it('reads input signals and rule ids from the Draft', () => {
    expect(choices.map((choice) => choice.label)).toEqual(['station · part_present (input)']);
    expect(globalRuleIds(document)).toEqual(['part_lost']);
    expect(globalRuleIds({ policies: {} })).toEqual([]);
  });

  it('creates a closed default rule and validates every field', () => {
    const rule = newGlobalRule(['rule_1'], choices[0]);
    expect(rule).toEqual({
      rule_id: 'rule_2', scope: 'all_motion', priority: 0, on_trip: 'stop',
      condition: { compare: { operand, op: '==', value: true } },
    });
    const valid = rule as unknown as JsonValue;
    expect(globalRuleProblem(valid, [], choices)).toBeNull();
    const withRule = (patch: Record<string, JsonValue>) => ({ ...(rule as unknown as Record<string, JsonValue>), ...patch });
    expect(globalRuleProblem(valid, ['rule_2'], choices)).toContain('already uses');
    expect(globalRuleProblem(withRule({ priority: 101 }), [], choices)).toContain('0 to 100');
    // on_trip is stop | notify; gate, full_stop and the latch fields are gone.
    expect(globalRuleProblem(withRule({ on_trip: 'notify' }), [], choices)).toBeNull();
    expect(globalRuleProblem(withRule({ on_trip: 'gate' }), [], choices)).toContain('On trip');
    expect(globalRuleProblem(withRule({ on_trip: 'full_stop' }), [], choices)).toContain('On trip');
    expect(globalRuleProblem(withRule({ on_trip: 'never' }), [], choices)).toContain('On trip');
    const { on_trip: _onTrip, ...withoutOnTrip } = rule as unknown as Record<string, JsonValue>;
    void _onTrip;
    expect(globalRuleProblem(withoutOnTrip, [], choices)).toContain('On trip');
    expect(globalRuleProblem(withRule({ idle_behaviour: 'latch' }), [], choices)).toContain('Unknown');
    expect(globalRuleProblem(withRule({ latch_release: 'operator' }), [], choices)).toContain('Unknown');
    expect(globalRuleProblem(withRule({ fallback_outcome: 'fail_node' }), [], choices)).toContain('Unknown');
    expect(globalRuleProblem(withRule({ success_through_stage_id: 'x' }), [], choices)).toContain('Unknown');
    // all_motion | {execution_groups} | everything; priority omitted = 0.
    const { priority: _omitted, ...withoutPriority } = rule as unknown as Record<string, JsonValue>;
    void _omitted;
    expect(globalRuleProblem(withoutPriority, [], choices)).toBeNull();
    expect(globalRuleProblem(withRule({ scope: 'everything' }), [], choices)).toBeNull();
    expect(globalRuleProblem(withRule({ scope: 'all' }), [], choices)).toContain('Scope is');
    expect(globalRuleProblem(withRule({ scope: { motion_group_ids: ['a'] } }), [], choices)).toContain('Scope is');
    expect(globalRuleProblem(withRule({ scope: { execution_groups: [] } }), [], choices)).toContain('execution group');
    expect(globalRuleProblem(withRule({ scope: { execution_groups: ['b', 'a'] } }), [], choices)).toContain('sorted');
    expect(globalRuleProblem(withRule({ scope: { execution_groups: ['a', 'a'] } }), [], choices)).toContain('unique');
    expect(globalRuleProblem(withRule({ scope: canonicalScope(['b', 'a', 'b']) as JsonValue }), [], choices)).toBeNull();
    expect(canonicalScope(['b', 'a', 'b'])).toEqual({ execution_groups: ['a', 'b'] });
    expect(globalRuleScopeKind('all_motion')).toBe('all_motion');
    expect(globalRuleScopeKind({ execution_groups: ['a'] })).toBe('execution_groups');
    expect(globalRuleScopeKind('all')).toBeNull();
    expect(globalRuleScopeGroups({ execution_groups: ['a', 'b'] })).toEqual(['a', 'b']);
    // after_stop: hold | fail_node, stop only; active_at_start: boolean.
    expect(globalRuleProblem(withRule({ after_stop: 'hold' }), [], choices)).toBeNull();
    expect(globalRuleProblem(withRule({ after_stop: 'fail_node' }), [], choices)).toBeNull();
    expect(globalRuleProblem(withRule({ after_stop: 'retry' }), [], choices)).toContain('hold or fail_node');
    expect(globalRuleProblem(withRule({ on_trip: 'notify', after_stop: 'hold' }), [], choices))
      .toContain('does not apply to on_trip notify');
    expect(globalRuleProblem(withRule({ on_trip: 'notify', stop_entry_stage_id: 'stage.one' }), [], choices))
      .toContain('does not apply to on_trip notify');
    expect(globalRuleProblem(withRule({ active_at_start: true }), [], choices)).toBeNull();
    expect(globalRuleProblem(withRule({ active_at_start: 'yes' }), [], choices)).toContain('Active at start');
    expect(globalRuleProblem(withRule({ condition: { compare: { operand, op: '>', value: true } } }), [], choices))
      .toContain('does not apply');
  });

  it('nests "changed outside" under each output that holds one', () => {
    const eligible = changedOutsideOutputs([
      { signal_id: 'station/clamp' }, { signal_id: 'station/clamp.changed_outside' },
    ]);
    expect([...eligible]).toEqual(['station/clamp']);
    const offered = globalRuleChoices(document, [], undefined, [], eligible);
    const changed = { signal: { controllable_component_id: 'station', signal_name: 'clamp.changed_outside' } };
    expect(offered.filter((choice) => choice.key === '').map((choice) => choice.label)).toEqual([
      'station · part_present (input)',
      'station · clamp → changed outside',
    ]);
    expect(conditionProblem(compare(changed, '==', true), offered)).toBeNull();
    // Not offered without its core-held sample, but kept for a rule that reads it.
    expect(globalRuleChoices(document, [], undefined).some((choice) => choice.id === operandId(changed))).toBe(false);
    expect(conditionProblem(compare(changed, '==', true),
      globalRuleChoices(document, [], compare(changed, '==', true)))).toBeNull();
  });

  it('includes notify in the typed global rule reaction', () => {
    const rule: GlobalInterruptRule = {
      rule_id: 'signal_notice',
      condition: {
        compare: {
          operand: { signal: { controllable_component_id: 'station', signal_name: 'part_present' } },
          op: '==',
          value: true,
        },
      },
      scope: 'all_motion',
      on_trip: 'notify',
    };
    expect(rule.on_trip).toBe('notify');
  });
});


describe('interrupt condition keys declare initial', () => {
  const key = (name: string, initial?: JsonValue) => ({
    key: name, type: 'bool', access: 'read_write', persistence: 'run',
    ...(initial === undefined ? {} : { initial }),
  }) as BlackboardKeyDeclaration;
  const compare = (name: string) => ({ compare: { operand: { key: name }, op: '==', value: true } });

  it('flags a declared key without initial with the Core reason; false and 0 are valid', () => {
    const keys = [key('cell.ready'), key('cell.armed', false), key('cell.count', 0)];
    expect(interruptKeyInitialProblem(compare('cell.armed'), keys)).toBeNull();
    expect(interruptKeyInitialProblem(compare('cell.count'), keys)).toBeNull();
    const problem = interruptKeyInitialProblem({ not: compare('cell.ready') }, keys);
    expect(problem).toContain(INTERRUPT_KEY_INITIAL_REQUIRED);
    expect(problem).toContain('cell.ready');
    // Signals and undeclared keys are not this check's concern.
    expect(interruptKeyInitialProblem(compare('cell.unknown'), keys)).toBeNull();
    expect(interruptKeyInitialProblem({ compare: { operand: { signal: { component_id: 'a', signal_name: 'b' } },
      op: '==', value: true } }, keys)).toBeNull();
  });

  it('collects the keys node rules and global rules read', () => {
    const forest = [{ node_id: 'sequence', instance_id: 'root', parameters: {}, children: [
      { node_id: 'motion.move_until_signal', instance_id: 'm', parameters: { interrupt_condition: compare('cell.a') },
        children: [] },
      { node_id: 'logic.check_value', instance_id: 'c', parameters: { key: 'cell.b' }, children: [] },
    ] }] as unknown as ProgramTreeNode[];
    const deployment = { policies: { interrupt_rules: [{ rule_id: 'r', condition: compare('cell.c') }] } } as unknown as
      Record<string, JsonValue>;
    expect([...interruptConditionKeyUses(forest, deployment)].sort()).toEqual(['cell.a', 'cell.c']);
  });
});

describe('qualified Stop chain pickers', () => {
  const response = (ids: string[]) => ({
    controlBindingId: 'c', controlBindingRevision: 'r', controlRouteId: 'route', nominalStopMode: null,
    stages: ids.map((stageId, index) => ({
      stageId,
      stageKind: ['nominal', 'controlled_fallback', 'software_hard', 'future_kind'][index],
      commandKind: 'controller_goal_cancel',
    })),
  });
  const arm = { controllableComponentId: 'arm', commandTargetId: 'tcp', stopResponse: response(['s1', 's2', 's3']) };

  it('labels stages by kind and stores their ids, in chain order', () => {
    expect(qualifiedStopStages(response(['s1', 's2', 's3', 's4']))).toEqual([
      { id: 's1', label: '1. Nominal stop (controller goal cancel)' },
      { id: 's2', label: '2. Controlled fallback (controller goal cancel)' },
      { id: 's3', label: '3. Software hard stop (controller goal cancel)' },
      { id: 's4', label: '4. Future kind (controller goal cancel)' },
    ]);
  });

  it('offers success-through only at or after the chosen entry', () => {
    const stages = qualifiedStopStages(arm.stopResponse);
    expect(successThroughStages(stages, 's2').map((stage) => stage.id)).toEqual(['s2', 's3']);
    expect(successThroughStages(stages, undefined).map((stage) => stage.id)).toEqual(['s1', 's2', 's3']);
    expect(successThroughStages(stages, 'gone')).toEqual([]);
  });

  it('gives a global rule only the stages every published chain qualifies', () => {
    const other = { controllableComponentId: 'arm2', commandTargetId: 'tcp', stopResponse: response(['s1', 's3']) };
    expect(commonStopStages([arm, other])?.map((stage) => stage.id)).toEqual(['s1', 's3']);
    expect(commonStopStages([{ ...arm, stopResponse: undefined }])).toBeNull();
    expect(commonStopStages([])).toBeNull();
  });
});

describe('signal liveness in interrupt conditions', () => {
  const input = (signalName: string, roleId: string, provider: Record<string, JsonValue>) => ({
    controllable_component_id: 'station', command_target_id: 'io', resource_role_id: roleId, signal_name: signalName,
    source_id: signalName, value_type: 'bool', direction: 'input', transition_capability: 'sampled_only',
    update_mode: provider.liveness !== undefined && (provider.liveness as Record<string, JsonValue>).mode === 'change_only'
      ? 'change_only' : 'sampled',
    freshness: provider.liveness !== undefined && (provider.liveness as Record<string, JsonValue>).mode === 'change_only'
      ? { mode: 'provider_validity' } : { mode: 'sample_age', maximum_age_ns: 1_000_000 },
    provider,
  });
  const document = {
    components: [{ component_id: 'station', configuration: { runtime_kind: 'io_provider', runtime_configuration: { io_signals: [
      input('door', 'door', { profile: 'ros_bool', topic: '/door', liveness: { mode: 'change_only' } }),
      input('beam', 'beam', { profile: 'ros_bool', topic: '/beam', liveness: { mode: 'periodic' } }),
      input('di3', 'di3', { profile: 'ros2_control_gpio', topic: '/gpio/states', group: 'io', interface_name: 'di3' }),
    ] } } }],
  } as unknown as Record<string, JsonValue>;
  const door = { signal: { controllable_component_id: 'station', signal_name: 'door' } };
  const beam = { signal: { controllable_component_id: 'station', signal_name: 'beam' } };
  const di3 = { signal: { controllable_component_id: 'station', signal_name: 'di3' } };

  it('labels a change-only global operand and refuses it with a clear error', () => {
    const choices = globalConditionChoices(deploymentInputSignals(document), []);
    expect(choices.map((choice) => [choice.label, choice.liveness])).toEqual([
      ['station · beam (input)', 'periodic'],
      ['station · di3 (input)', 'periodic'],
      ['station · door (input) · liveness unproven', 'change_only'],
    ]);
    expect(conditionProblem(compare(beam, '==', true), choices)).toBeNull();
    expect(conditionProblem(compare(di3, '==', true), choices)).toBeNull();
    const refused = conditionProblem({ any: [compare(beam, '==', true), { not: compare(door, '==', true) }] }, choices);
    expect(refused).toContain('station · door (input) is a change-only source');
    expect(refused).toContain('cannot be an interrupt condition operand');
  });

  it('uses candidate liveness for exact node-rule signals', () => {
    const rows = [{ reference: { component_id: 'station', signal_name: 'door' }, ambiguous: false,
      value_type: 'bool', direction: 'input' as const, liveness: 'change_only' as const },
    { reference: { component_id: 'station', signal_name: 'beam' }, ambiguous: false,
      value_type: 'bool', direction: 'input' as const, liveness: 'periodic' as const }];
    const choices = nodeConditionChoices(program, rows, []);
    expect(conditionProblem(compare({ signal: rows[0].reference }, '==', true), choices)).toContain('change-only source');
    expect(conditionProblem(compare({ signal: rows[1].reference }, '==', true), choices)).toBeNull();
  });
});

describe('system keys and their values', () => {
  const choices = nodeConditionChoices(program, candidateSignals, ['part_lost']);
  const outcome = (key: string) => ({ key: `node_outcome.${key}` });

  it('picks node outcome values from a list that ends with none', () => {
    expect(choices.find((choice) => choice.key === 'node_outcome.approach')?.values).toEqual([
      'completed', 'failed', 'preempted', 'superseded', 'refused_by_priority', 'none',
    ]);
    expect(choices.find((choice) => choice.key === 'node_outcome.seek')?.values?.at(-1)).toBe('none');
    expect(conditionProblem(compare(outcome('seek'), '!=', 'none'), choices)).toBeNull();
  });

  it('needs no initial for system keys', () => {
    const keys = programBlackboardKeys(program, ['part_lost']);
    const condition = { all: [compare(outcome('seek'), '!=', 'completed'),
      compare({ key: 'interrupt_state.part_lost' }, '==', 'tripped')] };
    expect(interruptKeyInitialProblem(condition, keys)).toBeNull();
    expect(interruptKeyInitialProblem(compare({ key: 'cell.count' }, '>=', 1), keys))
      .toContain(INTERRUPT_KEY_INITIAL_REQUIRED);
  });

  it('flags a != node outcome test that is not guarded against none', () => {
    expect(unguardedNodeOutcomeKeys(compare(outcome('seek'), '!=', 'completed'))).toEqual(['node_outcome.seek']);
    expect(unguardedNodeOutcomeKeys({ all: [compare(outcome('seek'), '!=', 'completed'),
      compare(outcome('seek'), '!=', 'none')] })).toEqual([]);
    expect(unguardedNodeOutcomeKeys(compare(outcome('seek'), '==', 'completed'))).toEqual([]);
    expect(unguardedNodeOutcomeKeys(compare({ key: 'cell.label' }, '!=', 'x'))).toEqual([]);
  });
});

describe('global rules: user keys and no rule states', () => {
  const rule = (id: string, condition: JsonValue) => ({ rule_id: id, scope: 'all_motion', on_trip: 'stop', condition });
  const document = (rules: JsonValue[]) => ({ policies: { interrupt_rules: rules } }) as Record<string, JsonValue>;
  const doorOpen = compare({ key: 'cell.door_open' }, '==', true);
  const doorKey: BlackboardKeyDeclaration = { key: 'cell.door_open', type: 'bool', access: 'read', persistence: 'run', initial: false };
  const stateKey: BlackboardKeyDeclaration = { key: 'interrupt_state.b', type: 'string', access: 'read', persistence: 'persistent' };

  it('keeps pending user keys editable and never offers rule states', () => {
    const rules = document([rule('a', doorOpen), rule('b', doorOpen)]);
    expect(globalRuleChoices(rules, null, doorOpen).map((choice) => choice.key)).toEqual(['cell.door_open']);
    expect(globalRuleChoices(rules, [doorKey, stateKey], undefined).map((choice) => choice.key)).toEqual(['cell.door_open']);
    expect(conditionProblem(doorOpen, globalRuleChoices(rules, null, doorOpen))).toBeNull();
  });

  it('notes waiting, undeclared, rule-state and mixed operands', () => {
    expect(globalRuleKeyNotes(doorOpen, null)).toEqual({ waiting: ['cell.door_open'], undeclared: [], stateOperands: [], mixed: false });
    expect(globalRuleKeyNotes(doorOpen, []).undeclared).toEqual(['cell.door_open']);
    expect(globalRuleKeyNotes(doorOpen, [doorKey])).toEqual({ waiting: [], undeclared: [], stateOperands: [], mixed: false });
    const mixed = { any: [compare({ signal: { controllable_component_id: 'station', signal_name: 'part_present' } }, '==', true), doorOpen] };
    expect(globalRuleKeyNotes(mixed, [doorKey]).mixed).toBe(true);
    const state = compare({ key: 'interrupt_state.b' }, '==', 'tripped');
    expect(globalRuleKeyNotes(state, [stateKey]).stateOperands).toEqual(['interrupt_state.b']);
    expect(globalRuleProblem(rule('a', state), [], [])).toContain('global_rule_state_operand_unsupported');
  });
});
