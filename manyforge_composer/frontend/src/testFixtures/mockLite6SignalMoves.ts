import type { JsonValue, ProgramAuthoringDocument, ProgramTreeNode } from '../types/api';

/**
 * Minimal real-shaped parts of examples/mock_lite6_signal_moves.scenario: the
 * direct signal references and the Deployment Draft's station_io inputs plus
 * the arm's controlled Stop.
 */

export const TRIAL_E_CONDITION: JsonValue = {
  compare: {
    op: '==',
    operand: { signal: { component_id: 'station_io', signal_name: 'probe_contact' } },
    value: true,
  },
};

const mockInput = (role: string): JsonValue => ({
  command_target_id: 'io', controllable_component_id: 'station_io', direction: 'input',
  freshness: { mode: 'provider_validity' }, provider: { profile: 'mock' },
  resource_role_id: role, signal_name: role, source_id: `station_io.${role}`,
  transition_capability: 'sampled_only', update_mode: 'change_only', value_type: 'bool',
});

/** `inputs`: the declared station_io inputs (curtain_blocked is not a required role of station). */
export function trialEDeployment(
  allowNaturalCompletion = false,
  inputs: readonly string[] = ['curtain_blocked', 'edge_sensor', 'probe_contact'],
): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment_intent.v2',
    components: [
      {
        component_id: 'lite6_robot',
        component_profile_id: 'lite6',
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: {} },
        target_settings: { arm: { controlled_stop: {
          stop_mode: 'soft_on_path',
          allow_natural_completion_on_short_path: allowNaturalCompletion,
        } } },
      },
      {
        component_id: 'station_io',
        component_profile_id: 'io_device_v0',
        configuration: { runtime_kind: 'io_provider', runtime_configuration: {
          io_provider_devices: [{ controllable_component_id: 'station_io' }],
          io_signals: [
            {
              command_target_id: 'io', controllable_component_id: 'station_io', direction: 'output',
              provider: { channel: 'clamp', profile: 'mock_output' }, resource_role_id: 'clamp',
              signal_name: 'clamp', source_id: 'station_io.clamp', value_type: 'bool',
            },
            ...inputs.map(mockInput),
          ],
        } },
        target_settings: {},
      },
    ],
  };
}

export function trialEProgram(forest: ProgramTreeNode[]): ProgramAuthoringDocument {
  return {
    api_version: 'manyforge.program.v3', program_id: 'mock_lite6_signal_moves', program_profile: 'composable-cell-v1',
    name: 'mock_lite6_signal_moves', catalog: { api_version: 'manyforge.node_catalog.v3', content_version: '3.0.0' },
    parameters: [],
    blackboard: { namespaces: [], keys: [] },
    schema_version: 'manyforge.composer_authoring_forest.v2', forest, positions: {},
  };
}
