import { describe, expect, it } from 'vitest';

import type { JsonValue } from '../types/api';
import {
  draftCarriedObjectLostSignals,
  resolveCarriedObjectLostSignalChoices,
} from './systemSignalAuthoring';

describe('Draft carried-object-lost signal rows', () => {
  it('matches the backend compiled-catalog fixture and ignores other capabilities', () => {
    // Parity pin for manyforge_deployment/test/test_io_signal_catalog.cpp:
    // CompiledCatalogDeclaresOneLostSignalPerGripperTarget.
    const document = {
      api_version: 'manyforge.deployment.v1',
      graph: {
        control_bindings: [
          { controllable_component_id: 'robot.b', command_target_id: 'hand', capability_id: 'tool.gripper.command' },
          { controllable_component_id: 'robot.a', command_target_id: 'gripper', capability_id: 'tool.gripper.command' },
          { controllable_component_id: 'robot.a', command_target_id: 'gripper', capability_id: 'tool.gripper.feedback' },
          { controllable_component_id: 'robot.a', command_target_id: 'arm', capability_id: 'execution.trajectory.execute' },
          { controllable_component_id: 'robot.a', command_target_id: 'gripper', capability_id: 'tool.gripper.command' },
        ],
      },
    } as unknown as Record<string, JsonValue>;

    expect(draftCarriedObjectLostSignals(document)).toEqual({
      deterministic: true,
      rows: [
        {
          controllableComponentId: 'robot.a', commandTargetId: 'gripper',
          signalName: 'carried_object_lost.gripper', resourceRoleId: 'carried_object_lost',
          direction: 'input', valueType: 'bool',
        },
        {
          controllableComponentId: 'robot.b', commandTargetId: 'hand',
          signalName: 'carried_object_lost.hand', resourceRoleId: 'carried_object_lost',
          direction: 'input', valueType: 'bool',
        },
      ],
    });
  });

  it('treats the exact command-binding table as empty for arm-only and unconfigured Real targets', () => {
    // The backend activation compiler emits no gripper binding for an arm-only
    // model or a Real component without its authored pneumatic gripper target.
    // Stale runtime rows alone must not fabricate those bindings.
    const document = {
      api_version: 'manyforge.deployment.v1',
      graph: {
        control_bindings: [
          { controllable_component_id: 'arm_only', command_target_id: 'arm', capability_id: 'execution.trajectory.execute' },
          { controllable_component_id: 'real_without_gripper', command_target_id: 'arm', capability_id: 'execution.trajectory.execute' },
        ],
        runtime_units: [{ configuration: {
          gripper_target_configurations: [
            { controllable_component_id: 'arm_only', command_target_id: 'gripper' },
            { controllable_component_id: 'some_other_component', command_target_id: 'hand' },
          ],
        } }],
      },
    } as unknown as Record<string, JsonValue>;

    expect(draftCarriedObjectLostSignals(document)).toEqual({ deterministic: true, rows: [] });
  });

  it('does not infer a v2 system signal from configuration rows without resolved finger-group facts', () => {
    // The Deployment Intent exposes component/profile ids and these rows, but
    // not the selected component profile's resolved `fingers` source joints.
    // The backend omits arm-only and Real-unconfigured gripper bindings, so a
    // row-shaped guess would not match the compiler for every Draft.
    const document = {
      api_version: 'manyforge.deployment_intent.v2',
      components: [
        { component_id: 'arm_only', component_profile_id: 'arm_only_profile', configuration: {
          runtime_configuration: { gripper_target_configurations: [
            { controllable_component_id: 'arm_only', command_target_id: 'gripper' },
          ] },
        } },
        { component_id: 'real_without_gripper', component_profile_id: 'real_profile', configuration: {
          runtime_configuration: { gripper_target_configurations: [] },
        } },
        { component_id: 'robot', component_profile_id: 'robot_profile', configuration: {
          runtime_configuration: { gripper_target_configurations: [
            { controllable_component_id: 'unrelated_stale_component', command_target_id: 'hand' },
          ] },
        } },
      ],
    } as unknown as Record<string, JsonValue>;

    expect(draftCarriedObjectLostSignals(document)).toEqual({ deterministic: false, rows: [] });
  });

  it('does not treat a graph-shaped unknown version as deterministic', () => {
    const document = {
      api_version: 'manyforge.deployment.v999',
      graph: { control_bindings: [
        { controllable_component_id: 'robot', command_target_id: 'gripper', capability_id: 'tool.gripper.command' },
      ] },
    } as unknown as Record<string, JsonValue>;

    expect(draftCarriedObjectLostSignals(document)).toEqual({ deterministic: false, rows: [] });
  });

  it('keeps last-Apply v2 candidates while the Draft changes and rejects stale owner rows', () => {
    const appliedRows = [{
      controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: 'carried_object_lost.gripper', resourceRoleId: 'carried_object_lost',
      direction: 'input', valueType: 'bool',
    }] as const;
    const changedDraft = {
      api_version: 'manyforge.deployment_intent.v2',
      components: [{ component_id: 'robot', configuration: { runtime_configuration: {} } }],
      policies: { interrupt_rules: [{ rule_id: 'rule_1' }] },
    } as unknown as Record<string, JsonValue>;
    const draftRows = draftCarriedObjectLostSignals(changedDraft);
    const resolution = resolveCarriedObjectLostSignalChoices(
      draftRows,
      appliedRows,
      'manyforge.deployment_intent.v2',
      false,
      { graphRevision: 'graph.1', retainedBundleId: 'bundle.1' },
      { graphRevision: 'graph.1', retainedBundleId: 'bundle.1' },
    );
    expect(resolution).toEqual({
      choices: [{ ...appliedRows[0], provenance: 'last_apply' }],
      needApply: false,
    });

    expect(resolveCarriedObjectLostSignalChoices(
      draftRows,
      appliedRows,
      'manyforge.deployment_intent.v2',
      false,
      { graphRevision: 'graph.2', retainedBundleId: 'bundle.1' },
      { graphRevision: 'graph.1', retainedBundleId: 'bundle.1' },
    )).toEqual({ choices: [], needApply: true });

    expect(resolveCarriedObjectLostSignalChoices(
      draftRows,
      appliedRows,
      'manyforge.deployment.v999',
      false,
      { graphRevision: 'graph.1', retainedBundleId: 'bundle.1' },
      { graphRevision: 'graph.1', retainedBundleId: 'bundle.1' },
    )).toEqual({ choices: [], needApply: true });
  });
});
