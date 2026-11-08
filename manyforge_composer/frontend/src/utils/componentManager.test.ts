import { describe, expect, it } from 'vitest';

import type { JsonValue, SourceModelTarget } from '../types/api';
import type { MachineCatalogEntry } from '../types/componentCatalog';
import {
  addComponentProblem,
  machineCatalogEntry,
  machineMembers,
  withoutMachine,
  componentFrameTargets,
  componentManagerRows,
  componentRowText,
  componentSignals,
  draftGripperFrames,
  framePrefixFor,
  mountCandidates,
  mountParents,
  nextFreeComponentId,
  renameComponentRows,
  resolvedRobotTcp,
  toolMountRow,
  withMountRobot,
} from './componentManager';

const robot = {
  component_id: 'lite6_robot',
  component_profile_id: 'lite6_mock_commanded_v0',
  configuration: {
    runtime_kind: 'ros2_control',
    runtime_configuration: {
      component_streams: [{ controllable_component_id: 'lite6_robot', stream_id: 'measured_joints' }],
      gripper_target_configurations: [{
        controllable_component_id: 'lite6_robot',
        pneumatic_io: { outputs: [{ signal: { controllable_component_id: 'gripper_io', signal_name: 'valve' } }] },
      }],
    },
  },
};

const device = {
  component_id: 'gripper_io',
  component_profile_id: 'io_device_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'io_provider',
    runtime_configuration: {
      io_provider_devices: [{ controllable_component_id: 'gripper_io' }],
      io_signals: [{ controllable_component_id: 'gripper_io', signal_name: 'valve', source_id: 'gripper_io.valve' }],
    },
  },
};

function document(...components: JsonValue[]): Record<string, JsonValue> {
  return { api_version: 'manyforge.deployment_intent.v2', components, attachments: [], policies: {} };
}

describe('Component Manager rows', () => {
  it('classifies Draft components against Manager Applied', () => {
    const rows = componentManagerRows(document(robot, device), ['lite6_robot', 'old_robot']);
    expect(rows.map((row) => [row.componentId, row.kind, row.change])).toEqual([
      ['gripper_io', 'io_device', 'added'],
      ['lite6_robot', 'robot', 'applied'],
      ['old_robot', null, 'removed'],
    ]);
    // A compiled (non-Scenario) Draft lists the Applied components unchanged.
    expect(componentManagerRows({ api_version: 'manyforge.deployment.v1' }, ['b', 'a'])
      .map((row) => [row.componentId, row.change])).toEqual([['a', 'applied'], ['b', 'applied']]);
    // Nothing applied yet: every Draft component is to be added.
    expect(componentManagerRows(document(robot), null).map((row) => row.change)).toEqual(['added']);
  });
});

describe('Tools mounted on a robot flange', () => {
  const hand = {
    component_id: 'lite_gripper',
    component_profile_id: 'lite_gripper_v0',
    configuration: { runtime_kind: 'ros2_control', runtime_configuration: {} },
  };
  const profiles = {
    lite6_mock_commanded_v0: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
    lite_gripper_v0: { tags: ['gripper', 'tool'], attachmentInterfaces: [], tcpTargets: [] },
  };
  const mounted = (...components: JsonValue[]) => ({
    ...document(...components), attachments: [toolMountRow('lite6_robot', 'lite_gripper')],
  });

  it('lists a tool with its mount', () => {
    const rows = componentManagerRows(mounted(robot, hand), null, profiles);
    // One tree: the robot, then the tool mounted on it.
    expect(rows.map((row) => [row.componentId, row.kind, row.mount])).toEqual([
      ['lite6_robot', 'robot', null],
      ['lite_gripper', 'tool', 'lite6_robot.flange'],
    ]);
    // Not mounted yet: the component type's `tool` tag still names the kind.
    expect(componentManagerRows(document(hand), null, profiles)[0].kind).toBe('tool');
    // Without type facts an unmounted model row reads as a robot.
    expect(componentManagerRows(document(hand), null)[0].kind).toBe('robot');
  });

  it('offers robots with a free flange as mounts, one tool per flange', () => {
    expect(mountCandidates(document(robot, hand), profiles)).toEqual(['lite6_robot']);
    expect(mountCandidates(mounted(robot, hand), profiles)).toEqual([]);
    // A profile without type facts or without a flange is never offered.
    expect(mountCandidates(document(robot), {})).toEqual([]);
    expect(mountCandidates(document(hand, device), profiles)).toEqual([]);
  });

  it('writes the flange-to-mount attachment row', () => {
    expect(toolMountRow('lite6_robot', 'lite_gripper')).toEqual({
      parent: { component_id: 'lite6_robot', interface: 'flange' },
      child: { component_id: 'lite_gripper', interface: 'mount' },
    });
  });

  it('resolves the robot tcp: mounted tool, else own tcp, else flange (D1)', () => {
    const frames = (tcp: string | null) => [
      { frame_role_id: 'base', source_link_name: 'link_base' },
      { frame_role_id: 'flange', source_link_name: 'link_eef' },
      ...(tcp === null ? [] : [{ frame_role_id: 'tcp', source_link_name: tcp }]),
    ];
    expect(resolvedRobotTcp(mounted(robot, hand), 'lite6_robot', frames('link_tcp'))).toBe('lite_gripper default TCP');
    expect(resolvedRobotTcp(mounted(robot, hand), 'lite6_robot', null)).toBe('lite_gripper default TCP');
    expect(resolvedRobotTcp(document(robot), 'lite6_robot', frames('link_tcp'))).toBe('robot TCP (link_tcp)');
    expect(resolvedRobotTcp(document(robot), 'lite6_robot', frames('link_eef'))).toBe('flange (link_eef)');
    expect(resolvedRobotTcp(document(robot), 'lite6_robot', frames(null))).toBe('flange');
    // Not a robot motion target (no base frame), or nothing Applied: no annotation.
    expect(resolvedRobotTcp(document(robot), 'lite6_robot', [{ frame_role_id: 'tcp', source_link_name: 'link_tcp' }]))
      .toBeNull();
    expect(resolvedRobotTcp(document(robot), 'lite6_robot', null)).toBeNull();
  });
});

describe('Add component rows', () => {
  it('renames a component everywhere it is named, including source ids', () => {
    const [renamedDevice, renamedRobot] = renameComponentRows([device, robot], {
      gripper_io: 'gripper_io_2', lite6_robot: 'lite6_b',
    });
    expect(renamedDevice.component_id).toBe('gripper_io_2');
    expect(JSON.stringify(renamedDevice)).not.toContain('"gripper_io"');
    expect(JSON.stringify(renamedDevice)).toContain('"gripper_io_2.valve"');
    expect(renamedRobot.component_id).toBe('lite6_b');
    expect(JSON.stringify(renamedRobot)).not.toContain('lite6_robot');
    // The robot's gripper still names the renamed device.
    expect(JSON.stringify(renamedRobot)).toContain('"controllable_component_id":"gripper_io_2"');
    // Other strings (profiles, signal names) are untouched.
    expect(renamedRobot.component_profile_id).toBe('lite6_mock_commanded_v0');
  });

  it('points every robot_component_id of added rows at the mount robot', () => {
    const valve: Record<string, JsonValue> = {
      component_id: 'valve_io',
      configuration: {
        runtime_configuration: {
          io_signals: [
            { signal_name: 'contact', provider: { profile: 'geometric_contact', robot_component_id: 'lite6_robot',
              target_scene_resource_id: 'workpiece' } },
          ],
        },
      },
    };
    const [bound] = withMountRobot([valve], 'lite6_robot_2');
    expect(JSON.stringify(bound)).toContain('"robot_component_id":"lite6_robot_2"');
    expect(JSON.stringify(bound)).not.toContain('"lite6_robot"');
    expect(bound.configuration).toMatchObject({ runtime_configuration: { io_signals: [
      { provider: { profile: 'geometric_contact', robot_component_id: 'lite6_robot_2' } },
    ] } });
    // Rows without one are unchanged.
    expect(withMountRobot([device], 'lite6_robot_2')).toEqual([device]);
  });

  it('chooses free ids and frame prefixes', () => {
    expect(nextFreeComponentId('lite6_robot', new Set(['lite6_robot', 'lite6_robot_2']))).toBe('lite6_robot_3');
    expect(nextFreeComponentId('ur5_robot', new Set(['lite6_robot']))).toBe('ur5_robot');
    expect(framePrefixFor('lite6.robot-b')).toBe('lite6_robot_b/');
  });

  it('refuses existing, repeated and malformed ids', () => {
    const draft = document(robot);
    expect(addComponentProblem([device], draft)).toBeNull();
    expect(addComponentProblem([robot], draft)).toContain('already has a component "lite6_robot"');
    expect(addComponentProblem([device, device], draft)).toContain('Two added components');
    expect(addComponentProblem([{ ...device, component_id: 'Gripper' }], draft)).toContain('lowercase');
  });
});

describe('Component page facts', () => {
  const mockInput = (component: string, signal: string): Record<string, JsonValue> => ({
    controllable_component_id: component, command_target_id: 'io', resource_role_id: signal,
    signal_name: signal, source_id: `${component}_source`, value_type: 'bool', direction: 'input',
    update_mode: 'change_only', transition_capability: 'sampled_only',
    freshness: { mode: 'provider_validity' }, provider: { profile: 'mock' },
  });
  const output = (component: string, signal: string, provider: Record<string, JsonValue>): Record<string, JsonValue> => ({
    controllable_component_id: component, command_target_id: 'io', resource_role_id: signal,
    signal_name: signal, source_id: `${component}_source`, value_type: 'bool', direction: 'output', provider,
  });
  const ioDevice = (id: string, signals: JsonValue[]): Record<string, JsonValue> => ({
    component_id: id, component_profile_id: 'io_device_v0',
    configuration: { runtime_kind: 'io_provider', runtime_configuration: { io_signals: signals } },
  });
  const cell = {
    api_version: 'manyforge.deployment_intent.v2',
    components: [
      { component_id: 'lite6_robot', component_profile_id: 'lite6_mock_commanded_v0',
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: {} } },
      { component_id: 'lite_gripper', component_profile_id: 'lite_gripper_v0',
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: {
          gripper_target_configurations: [{
            controllable_component_id: 'lite_gripper', command_target_id: 'gripper',
            tcp_frame: 'tcp', attach_frame: 'attach', mount_frame: 'mount',
            pneumatic_io: {
              outputs: [{ signal: { controllable_component_id: 'gripper_io', signal_name: 'valve' },
                open_value: true, close_value: false }],
              inputs: { open: { signal: { controllable_component_id: 'gripper_io', signal_name: 'open_sensor' } } },
            },
          }],
        } } },
      ioDevice('gripper_io', [
        mockInput('gripper_io', 'open_sensor'),
        output('gripper_io', 'valve', { profile: 'mock_output', channel: 'valve' }),
      ]),
      ioDevice('contact_io', [{
        ...mockInput('contact_io', 'touching'), update_mode: 'sampled',
        freshness: { mode: 'sample_age', maximum_age_ns: 100_000_000 },
        provider: { profile: 'geometric_contact', robot_component_id: 'lite6_robot',
          target_scene_resource_id: 'box' },
      }]),
    ],
    attachments: [toolMountRow('lite6_robot', 'lite_gripper')],
  } as Record<string, JsonValue>;

  it('lists the signals an I/O device declares and the gripper that uses them', () => {
    const signals = componentSignals(cell, 'gripper_io');
    expect(signals.owned.map((row) => [row.signalName, row.direction, row.detail])).toEqual([
      ['open_sensor', 'input', 'Mock input'],
      ['valve', 'output', 'Mock output'],
    ]);
    expect(signals.uses).toEqual([]);
    expect(signals.usedBy.map((row) => `${row.componentId}.${row.signalName}: ${row.detail}`)).toEqual([
      'gripper_io.open_sensor: Open input of lite_gripper',
      'gripper_io.valve: valve output of lite_gripper',
    ]);
  });

  it('lists the signals a gripper uses without assigning robot contact to its frame', () => {
    const signals = componentSignals(cell, 'lite_gripper');
    expect(signals.owned).toEqual([]);
    expect(signals.uses.map((row) => `${row.componentId}.${row.signalName} (${row.direction}): ${row.detail}`)).toEqual([
      'gripper_io.open_sensor (input): Open input of gripper',
      'gripper_io.valve (output): valve output of gripper',
    ]);
    expect(signals.usedBy).toEqual([]);
  });

  it('lists geometric contact under the selected robot whose Active TCP it follows', () => {
    const signals = componentSignals(cell, 'lite6_robot');
    expect(signals.owned).toEqual([]);
    expect(signals.usedBy).toEqual([{
      componentId: 'contact_io', signalName: 'touching', direction: 'input',
      detail: 'Active TCP contact with box',
    }]);
  });

  it('reads a gripper\'s authored frames from the nested runtime configuration', () => {
    expect(draftGripperFrames(cell, 'lite_gripper')).toEqual([
      { targetId: 'gripper', tcpFrame: 'tcp', attachFrame: 'attach', mountFrame: 'mount' },
    ]);
    expect(draftGripperFrames(cell, 'lite6_robot')).toEqual([]);
  });

  it('lists the Applied frames of each target with the tcp it resolves to', () => {
    const target = (componentId: string, targetId: string, frames: Array<[string, string | null]>) => ({
      component_id: componentId, target_id: targetId, collision_bodies: [], model_groups: [],
      frames: frames.map(([role, link]) => ({
        frame_kind: 'component_model_frame', frame_role_id: role, model_instance_id: 'm', source_link_name: link,
      })),
    }) as SourceModelTarget;
    const targets = [
      target('lite6_robot', 'arm', [['base', 'link_base'], ['tcp', 'link_eef'], ['flange', 'link_eef']]),
      target('other', 'arm', [['base', 'b']]),
    ];
    // The mounted tool's default TCP is the robot's tcp (D1).
    expect(componentFrameTargets(targets, cell, 'lite6_robot')).toEqual([{
      targetId: 'arm',
      frames: [
        { frameRoleId: 'base', sourceLinkName: 'link_base' },
        { frameRoleId: 'tcp', sourceLinkName: 'link_eef' },
        { frameRoleId: 'flange', sourceLinkName: 'link_eef' },
      ],
      resolvedTcp: 'lite_gripper default TCP',
    }]);
    // Without the tool: the flange, named by its link.
    const bare = { ...cell, attachments: [] } as Record<string, JsonValue>;
    expect(componentFrameTargets(targets, bare, 'lite6_robot')[0].resolvedTcp).toBe('flange (link_eef)');
    expect(componentFrameTargets(null, cell, 'lite6_robot')).toEqual([]);
  });


});

describe('machines: owners and members', () => {
  const owner = { component_id: 'lite6', component_profile_id: 'ufactory.lite6_machine', lane: 'real', configuration: {} };
  const arm = {
    component_id: 'lite6_robot', component_profile_id: 'ufactory.lite6', owner: 'lite6', role: 'arm',
    model_source: { urdf: { project_path: 'lite6.urdf' }, package_roots: {} },
  };
  const io = { component_id: 'lite6_robot_controller_io', component_profile_id: 'ufactory.lite6_controller_io',
    owner: 'lite6', role: 'controller_io' };
  const tool = { component_id: 'lite6_robot_tool_io', component_profile_id: 'ufactory.lite6_tool_io',
    owner: 'lite6', role: 'tool_io', active: false };
  const machine: MachineCatalogEntry = {
    modelId: 'ufactory.lite6_machine', title: 'UFactory Lite 6', lanes: ['mock', 'real'],
    defaultOwnerId: 'lite6',
    ownerRow: { component_id: 'lite6', component_profile_id: 'ufactory.lite6_machine', configuration: {} },
    members: [
      { role: 'arm', modelId: 'ufactory.lite6', title: 'Arm',
        row: { component_id: 'lite6_arm', component_profile_id: 'ufactory.lite6', model_source: arm.model_source } },
      { role: 'controller_io', modelId: 'ufactory.lite6_controller_io', title: 'Controller I/O',
        row: { component_id: 'lite6_controller_io', component_profile_id: 'ufactory.lite6_controller_io' } },
    ],
  };

  it('lists a machine as its owner with its members right under it', () => {
    const rows = componentManagerRows(document(io, device, tool, owner, arm), ['lite6'], {
      'ufactory.lite6': { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
    });
    expect(rows.map((row) => [row.componentId, row.kind, row.owner, row.role, row.active])).toEqual([
      ['gripper_io', 'io_device', null, null, true],
      ['lite6', 'machine', null, null, true],
      ['lite6_robot', 'robot', 'lite6', 'arm', true],
      ['lite6_robot_controller_io', 'io_device', 'lite6', 'controller_io', true],
      ['lite6_robot_tool_io', 'io_device', 'lite6', 'tool_io', false],
    ]);
    // Members sort under their owner, whatever their own ids.
    const early = { ...io, component_id: 'a_io' };
    expect(componentManagerRows(document(early, owner), null).map((row) => row.componentId)).toEqual(['lite6', 'a_io']);
    expect(machineMembers(document(owner, arm, device), 'lite6').map((row) => row.component_id)).toEqual(['lite6_robot']);
  });

  it('lists one tree: the machine, its arm with the tool mounted on it, then its I/O members', () => {
    const gripper = { component_id: 'lite_gripper', component_profile_id: 'ufactory.lite_gripper' };
    const draft = { ...document(gripper, owner, arm, io, tool, device), attachments: [toolMountRow('lite6_robot', 'lite_gripper')] };
    expect(mountParents(draft)).toEqual({ lite_gripper: 'lite6_robot' });
    const rows = componentManagerRows(draft, ['lite6', 'lite6_robot', 'lite_gripper']);
    expect(rows.map((row) => [row.componentId, row.depth])).toEqual([
      ['gripper_io', 0],
      ['lite6', 0],
      ['lite6_robot', 1],
      ['lite_gripper', 2],
      ['lite6_robot_controller_io', 1],
      ['lite6_robot_tool_io', 1],
    ]);
    const text = (id: string) => {
      const row = rows.find((candidate) => candidate.componentId === id)!;
      return componentRowText(row, row.owner === null && row.depth > 0);
    };
    expect(text('lite6')).toBe('Machine · ufactory.lite6_machine');
    expect(text('lite6_robot')).toBe('Arm · ufactory.lite6');
    expect(text('lite_gripper')).toBe('Tool on flange · ufactory.lite_gripper');
    expect(text('lite6_robot_controller_io')).toBe('I/O device · controller_io');
    expect(rows.find((row) => row.componentId === 'lite6_robot_tool_io')?.active).toBe(false);
    // A tool whose parent isn't listed stays at the top with its whole mount.
    const alone = componentManagerRows({ ...document(gripper), attachments: draft.attachments }, null);
    expect(alone.map((row) => [row.componentId, row.depth])).toEqual([['lite_gripper', 0]]);
    expect(componentRowText(alone[0], false)).toBe('Tool on lite6_robot.flange · ufactory.lite_gripper');
  });

  it('renames follow the owner link; mounting a tool leaves it alone', () => {
    const [renamedOwner, renamedArm] = renameComponentRows([owner, arm], { lite6: 'cell_a', lite6_robot: 'cell_a_arm' });
    expect(renamedOwner.component_id).toBe('cell_a');
    expect(renamedArm).toMatchObject({ component_id: 'cell_a_arm', owner: 'cell_a', role: 'arm' });
    expect(renameComponentRows([arm], { lite6_robot: 'x' })[0].owner).toBe('lite6');
    expect(withMountRobot([arm], 'other_robot')[0].owner).toBe('lite6');
  });

  it('removes a machine with its members and their attachments; other rows stay', () => {
    const draft = { ...document(owner, arm, io, device), attachments: [
      { parent: { component_id: 'lite6_robot', interface: 'flange' }, child: { component_id: 'hand', interface: 'mount' } },
      { parent: { component_id: 'other', interface: 'flange' }, child: { component_id: 'hand2', interface: 'mount' } },
    ] };
    const kept = withoutMachine(draft, 'lite6');
    expect(kept.components).toEqual([device]);
    expect(kept.attachments).toHaveLength(1);
  });

  it('builds a machine add from the model\'s ready rows: owner on its lane, members with default ids', () => {
    const added = machineCatalogEntry(machine, 'real', {
      'ufactory.lite6': { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
    });
    expect(added).toMatchObject({ kind: 'machine', componentId: 'lite6', tags: ['articulated-arm'], lane: 'real' });
    expect(added.components).toEqual([
      { component_id: 'lite6', component_profile_id: 'ufactory.lite6_machine', lane: 'real', configuration: {} },
      { component_id: 'lite6_arm', component_profile_id: 'ufactory.lite6', model_source: arm.model_source,
        owner: 'lite6', role: 'arm' },
      { component_id: 'lite6_controller_io', component_profile_id: 'ufactory.lite6_controller_io',
        owner: 'lite6', role: 'controller_io' },
    ]);
  });
});
