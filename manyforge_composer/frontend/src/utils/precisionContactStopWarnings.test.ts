import { describe, expect, it } from 'vitest';

import type { JsonValue, ProgramTreeNode } from '../types/api';
import { deploymentArmControlledStop, precisionContactStopWarnings } from './precisionContactStopWarnings';

const intent = (stop: Record<string, JsonValue>): Record<string, JsonValue> => ({
  api_version: 'manyforge.deployment_intent.v2',
  components: [{ component_id: 'robot', target_settings: { arm: { controlled_stop: stop } } }],
});
const compiled = (parameters: Record<string, JsonValue>): Record<string, JsonValue> => ({
  api_version: 'manyforge.deployment.v1',
  graph: { control_bindings: [
    { control_binding_id: 'b', controllable_component_id: 'robot', command_target_id: 'arm' },
  ] },
  policies: { stop_stage_parameters: [{
    control_binding_id: 'b', stop_escalation_stage_id: 'motion_controlled_stop',
    provider_contract_ref: { provider_contract_role_id: 'stop_contract' },
    default_contract_parameters: parameters,
  }] },
});
const probe = (stopMode: string): ProgramTreeNode => ({
  node_id: 'motion.probe_search', instance_id: 'probe', children: [],
  parameters: { target: { component_id: 'robot', command_target_id: 'arm' }, stop_mode: stopMode },
});
const NATURAL = 'This probe may finish a short remaining path instead of stopping on contact.';
const SPRING = 'If the soft Stop cannot be carried out, this probe will spring back instead of stopping at the contact point.';
const DISABLE = "To change it, disable the spring-back fallbacks in the Deployment's Stop settings for this arm.";

describe('precisionContactStopWarnings', () => {
  it('warns when natural completion is inherited from the Deployment', () => {
    const warnings = precisionContactStopWarnings(
      probe('spring_back_hold'),
      intent({ stop_mode: 'spring_back_hold', allow_natural_completion_on_short_path: true }),
    );
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toContain(NATURAL);
    expect(warnings[0]).toContain(
      "inherited from the Deployment's arm Stop settings (robot · arm) and applies to this node's Stop mode spring_back_hold.",
    );
  });

  it('gives no warning where the setting does not apply to the mode', () => {
    // Natural completion is not used by the hard stop; the substitution only by soft_on_path.
    const settings = { allow_natural_completion_on_short_path: true, recoverable_substitution_mode: 'spring_back_hold' };
    expect(precisionContactStopWarnings(probe('hard_deceleration'),
      compiled({ stop_mode: 'hard_deceleration', ...settings }))).toEqual([]);
    const fast = precisionContactStopWarnings(probe('spring_back_fast'),
      compiled({ stop_mode: 'spring_back_fast', ...settings }));
    expect(fast).toHaveLength(1);
    expect(fast[0]).toContain(NATURAL);
  });

  it('warns on a substitution with soft_on_path, from a compiled Deployment', () => {
    const document = compiled({
      stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: false,
      recoverable_substitution_mode: 'spring_back_hold',
    });
    expect(deploymentArmControlledStop(document, 'robot', 'arm')).toEqual({
      stopMode: 'soft_on_path', allowNaturalCompletion: false, fallbackStopModes: ['spring_back_hold'],
    });
    const warnings = precisionContactStopWarnings(probe('soft_on_path'), document);
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toContain(SPRING);
    expect(warnings[0]).toContain('The spring-back fallback (spring_back_hold) is inherited');
    expect(warnings[0]).toContain(DISABLE);
  });

  it.each([
    [['spring_back_hold'], 'spring_back_hold'],
    [['spring_back_fast'], 'spring_back_fast'],
    [['spring_back_hold', 'spring_back_fast'], 'spring_back_hold, spring_back_fast'],
  ])('warns on intent v2 spring-back fallbacks %j after a soft primary', (fallbacks, named) => {
    const document = intent({
      stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: false,
      fallback_stop_modes: fallbacks,
    });
    const warnings = precisionContactStopWarnings(probe('soft_on_path'), document);
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toContain(SPRING);
    expect(warnings[0]).toContain(`The spring-back fallback (${named}) is inherited from the Deployment's arm Stop settings (robot · arm)`);
    expect(warnings[0]).toContain(DISABLE);
  });

  it('gives no spring-back warning without fallbacks or after a spring-back primary', () => {
    const stops: Record<string, JsonValue>[] = [
      { stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: false },
      { stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: false, fallback_stop_modes: [] },
    ];
    for (const stop of stops) {
      expect(precisionContactStopWarnings(probe('soft_on_path'), intent(stop))).toEqual([]);
    }
    expect(precisionContactStopWarnings(probe('spring_back_hold'), intent({
      stop_mode: 'spring_back_hold', allow_natural_completion_on_short_path: false,
      fallback_stop_modes: ['spring_back_fast'],
    }))).toEqual([]);
  });

  it('reads a list field a compiled Deployment carries, guarded by presence', () => {
    const document = compiled({
      stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: false,
      fallback_stop_modes: ['spring_back_fast'],
    });
    expect(deploymentArmControlledStop(document, 'robot', 'arm')?.fallbackStopModes).toEqual(['spring_back_fast']);
    const warnings = precisionContactStopWarnings(probe('soft_on_path'), document);
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toContain('(spring_back_fast)');
    expect(deploymentArmControlledStop(compiled({
      stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: false,
    }), 'robot', 'arm')?.fallbackStopModes).toEqual([]);
  });

  it('ignores compiled rows that are not the stop contract', () => {
    const document = compiled({ stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: true });
    const rows = (document.policies as { stop_stage_parameters: Record<string, JsonValue>[] }).stop_stage_parameters;
    rows[0].provider_contract_ref = { provider_contract_role_id: 'other' };
    expect(deploymentArmControlledStop(document, 'robot', 'arm')).toBeNull();
  });

  it('gives no warning when neither setting is allowed, or for other nodes', () => {
    const stop = intent({ stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: false });
    expect(precisionContactStopWarnings(probe('soft_on_path'), stop)).toEqual([]);
    const natural = intent({ stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: true });
    expect(precisionContactStopWarnings(
      { ...probe('soft_on_path'), node_id: 'motion.move_until_signal' }, natural,
    )).toEqual([]);
    expect(precisionContactStopWarnings({ ...probe('soft_on_path'), parameters: {} }, natural)).toEqual([]);
  });

  it('says a run-time node mode must resolve to the Deployment mode', () => {
    const node = { ...probe('soft_on_path'), value_sources: {
      stop_mode: { kind: 'program_parameter' as const, reference: 'mode' },
    } };
    const warnings = precisionContactStopWarnings(node, intent({
      stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: true,
    }));
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toContain(
      "This node's Stop mode is set at run time; it must resolve to soft_on_path, the Deployment's Stop mode, for the probe to run.",
    );
  });
});
