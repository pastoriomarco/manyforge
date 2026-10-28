import { describe, expect, it } from 'vitest';

import type { ProgramBindingDocument } from '../types/api';
import {
  ioEmptyOmits,
  ioSignalRoleChoices,
  ioTimeoutHint,
  ioUnsupportedChoices,
} from './ioAuthoring';

const digest = `sha256:${'b'.repeat(64)}`;
const signal = (resource_role_id: string, signal_name: string, direction: 'input' | 'output') => ({
  resource_role_id, signal_name, value_type: 'bool' as const, direction,
  transition_capability: 'sampled_only' as const, signal_descriptor_digest: digest,
});
const binding = {
  target_role_bindings: [
    { target_role_id: 'arm', signal_role_bindings: [
      signal('lamp', 'DO1', 'output'), signal('part_present', 'DI0', 'input'),
    ] },
    { target_role_id: 'gripper' },
  ],
} as unknown as ProgramBindingDocument;

describe('io node authoring', () => {
  it('offers only the bound input signals of the node target, with name and direction', () => {
    for (const nodeId of ['io.get_input', 'io.check_input', 'io.wait_for_input']) {
      expect(ioSignalRoleChoices(nodeId, 'arm', binding)).toEqual([{
        value: 'part_present', label: 'part_present · DI0 (input)', group: 'arm input signals',
      }]);
    }
    // A bound target without signal rows has no input to offer.
    expect(ioSignalRoleChoices('io.get_input', 'gripper', binding)).toEqual([]);
  });

  it('falls back to the ordinary picker without binding info for the target', () => {
    expect(ioSignalRoleChoices('io.get_input', 'arm', null)).toBeNull();
    expect(ioSignalRoleChoices('io.get_input', 'other', binding)).toBeNull();
    expect(ioSignalRoleChoices('io.get_input', undefined, binding)).toBeNull();
    // Not an io node.
    expect(ioSignalRoleChoices('motion.move_to_pose', 'arm', binding)).toBeNull();
  });

  it('offers io.set_output only the bound output signals of its target', () => {
    expect(ioSignalRoleChoices('io.set_output', 'arm', binding)).toEqual([{
      value: 'lamp', label: 'lamp · DO1 (output)', group: 'arm output signals',
    }]);
    expect(ioSignalRoleChoices('io.set_output', 'gripper', binding)).toEqual([]);
  });

  it('offers the generated carried-object-lost input for a required target role before Apply', () => {
    const systemSignal = {
      controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: 'carried_object_lost.gripper', resourceRoleId: 'carried_object_lost',
      direction: 'input', valueType: 'bool',
    } as const;
    const facts = {
      program: { target_roles: { gripper: {
        component_role: 'tool', required_resource_roles: ['carried_object_lost'],
      } } },
      roleAssignment: {
        component_roles: [{ component_role_id: 'tool', controllable_component_id: 'robot' }],
        target_roles: [{ target_role_id: 'gripper', command_target_id: 'gripper' }],
      },
      deploymentDocument: null,
    };

    expect(ioSignalRoleChoices('io.get_input', 'gripper', null, facts, [systemSignal])).toEqual([{
      value: 'carried_object_lost',
      label: 'carried_object_lost · carried_object_lost.gripper (input)',
      group: 'gripper input signals',
    }]);
    expect(ioSignalRoleChoices('io.set_output', 'gripper', null, facts, [systemSignal])).toBeNull();
    expect(ioSignalRoleChoices('io.get_input', 'gripper', null, facts, [
      { ...systemSignal, provenance: 'last_apply' },
    ])?.[0]?.label).toBe('carried_object_lost · carried_object_lost.gripper (input) · from last Apply');
  });

  it('does not keep an Applied system row when the current Draft target identity changed', () => {
    const systemSignal = {
      controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: 'carried_object_lost.gripper', resourceRoleId: 'carried_object_lost',
      direction: 'input', valueType: 'bool',
    } as const;
    const staleBinding = {
      target_role_bindings: [{ target_role_id: 'gripper', controllable_component_id: 'old_robot',
        command_target_id: 'old_gripper', signal_role_bindings: [
          signal('carried_object_lost', 'carried_object_lost.old_gripper', 'input'),
        ] }],
    } as unknown as ProgramBindingDocument;
    const facts = {
      program: { target_roles: { gripper: {
        component_role: 'tool', required_resource_roles: ['carried_object_lost'],
      } } },
      roleAssignment: {
        component_roles: [{ component_role_id: 'tool', controllable_component_id: 'robot' }],
        target_roles: [{ target_role_id: 'gripper', command_target_id: 'gripper' }],
      },
      deploymentDocument: null,
    };

    expect(ioSignalRoleChoices('io.get_input', 'gripper', staleBinding, facts, [systemSignal])).toEqual([]);
  });

  it('keeps the ordinary no-Deployment result when no carried-object system input applies', () => {
    const facts = {
      program: { target_roles: { arm: {
        component_role: 'arm_component', required_resource_roles: ['part_present'],
      } } },
      roleAssignment: {
        component_roles: [{ component_role_id: 'arm_component', controllable_component_id: 'robot' }],
        target_roles: [{ target_role_id: 'arm', command_target_id: 'arm' }],
      },
      deploymentDocument: null,
    };
    expect(ioSignalRoleChoices('io.get_input', 'arm', null, facts, [])).toBeNull();
    expect(ioSignalRoleChoices('io.set_output', 'arm', null, facts, [])).toBeNull();
  });

  it('marks every edge mode except level unsupported, only on wait_for_input', () => {
    const schema = { enum: ['level', 'rising', 'falling'] };
    expect(ioUnsupportedChoices('io.wait_for_input', 'edge', schema)?.values)
      .toEqual(['rising', 'falling']);
    expect(ioUnsupportedChoices('io.wait_for_input', 'edge', { enum: ['level'] })).toBeUndefined();
    expect(ioUnsupportedChoices('io.check_input', 'edge', schema)).toBeUndefined();
  });

  it('lets an empty timeout_s mean wait forever and states the catalog range', () => {
    expect(ioEmptyOmits('io.wait_for_input', 'timeout_s')).toBe(true);
    expect(ioEmptyOmits('logic.wait', 'timeout_s')).toBe(false);
    expect(ioTimeoutHint({ type: 'number', minimum: 0.01, maximum: 3600 }))
      .toContain('0.01–3600 s');
  });
});
