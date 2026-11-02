import { describe, expect, it } from 'vitest';

import type { JsonValue } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import {
  defaultTcpChoice,
  defaultTcpEdit,
  primaryToolChoices,
  toolsWithoutTcp,
  toolWithoutTcpWarning,
} from './componentTcpChoices';

const target = (commandTargetId: string, tcpFrameRoleIds: string[]) => ({
  commandTargetId, tcpFrameRoleIds, defaultTcpFrameRoleId: tcpFrameRoleIds[0],
});

const profiles: Record<string, ComponentProfileFacts> = {
  arm_v0: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [target('arm', ['flange'])] },
  gripper_v0: { tags: ['gripper', 'tool'], attachmentInterfaces: [], tcpTargets: [target('gripper', ['tcp'])] },
  // A two-target tool with a `tcp` on both targets.
  dual_v0: { tags: ['tool'], attachmentInterfaces: [], tcpTargets: [target('arm', ['tcp']), target('gripper', ['tcp', 'edge'])] },
  bracket_v0: { tags: ['tool'], attachmentInterfaces: [], tcpTargets: [] },
  camera_v0: { tags: ['camera'], attachmentInterfaces: [], tcpTargets: [target('camera', ['optical'])] },
};

function draft(
  components: Array<[string, string, Record<string, JsonValue>?]>,
  attachments: Array<[string, string, string, boolean?]>,
): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment_intent.v2',
    components: components.map(([id, profile, extra]) => ({ component_id: id, component_profile_id: profile, ...extra })),
    attachments: attachments.map(([parent, parentInterface, child, primary]) => ({
      parent: { component_id: parent, interface: parentInterface },
      child: { component_id: child, interface: 'mount' },
      ...(primary === undefined ? {} : { primary }),
    })),
    policies: {},
  };
}

describe('Default TCP choice (B5)', () => {
  it('is needed only when the component has TCPs on more than one target', () => {
    const document = draft([['robot', 'arm_v0'], ['hand', 'gripper_v0'], ['dual', 'dual_v0'], ['bracket', 'bracket_v0']], []);
    expect(defaultTcpChoice(document, 'robot', profiles)).toBeNull();
    expect(defaultTcpChoice(document, 'hand', profiles)).toBeNull();
    expect(defaultTcpChoice(document, 'bracket', profiles)).toBeNull();
    // Unknown facts (catalog not read yet): no choice.
    expect(defaultTcpChoice(document, 'dual', {})).toBeNull();
    expect(defaultTcpChoice(document, 'dual', profiles)).toEqual({
      options: [
        { commandTargetId: 'arm', frameRoleId: 'tcp' },
        { commandTargetId: 'gripper', frameRoleId: 'tcp' },
        { commandTargetId: 'gripper', frameRoleId: 'edge' },
      ],
      value: null,
    });
  });

  it('offers the custom TCPs on each target with TCPs and reads the Draft value', () => {
    const document = draft([['dual', 'dual_v0', {
      custom_tcps: [{ tcp_id: 'probe', parent_frame: 'tcp', pose: { position_m: [0, 0, 0.1], orientation: { rpy_deg: [0, 0, 0] } } }],
      default_tcp: { command_target_id: 'gripper', frame_role_id: 'probe' },
    }]], []);
    const choice = defaultTcpChoice(document, 'dual', profiles)!;
    expect(choice.options).toContainEqual({ commandTargetId: 'arm', frameRoleId: 'probe' });
    expect(choice.options).toContainEqual({ commandTargetId: 'gripper', frameRoleId: 'probe' });
    expect(choice.value).toEqual({ commandTargetId: 'gripper', frameRoleId: 'probe' });
  });

  it('writes the pair to the component row', () => {
    const document = draft([['robot', 'arm_v0'], ['dual', 'dual_v0']], []);
    expect(defaultTcpEdit(document, 'dual', { commandTargetId: 'gripper', frameRoleId: 'tcp' })).toEqual({
      path: ['components', 1, 'default_tcp'],
      operation: 'set',
      value: { command_target_id: 'gripper', frame_role_id: 'tcp' },
    });
    expect(defaultTcpEdit(document, 'missing', { commandTargetId: 'arm', frameRoleId: 'tcp' })).toBeNull();
  });
});

describe('Primary tool choice (B5)', () => {
  const components: Array<[string, string]> = [
    ['robot', 'arm_v0'], ['hand', 'gripper_v0'], ['nozzle', 'bracket_v0'], ['camera', 'camera_v0'],
  ];

  it('is needed only when two or more tools share one parent interface', () => {
    expect(primaryToolChoices(draft(components, [['robot', 'flange', 'hand']]), 'robot', profiles)).toEqual([]);
    // A camera (not a tool) next to one tool needs no choice.
    expect(primaryToolChoices(draft(components, [
      ['robot', 'flange', 'hand'], ['robot', 'flange', 'camera'],
    ]), 'robot', profiles)).toEqual([]);
    expect(primaryToolChoices(draft(components, [
      ['robot', 'flange', 'hand'], ['robot', 'flange', 'camera'], ['robot', 'flange', 'nozzle'],
    ]), 'robot', profiles)).toEqual([{ interfaceId: 'flange', tools: ['hand', 'nozzle'], primary: null }]);
    expect(primaryToolChoices(draft(components, [
      ['robot', 'flange', 'hand'], ['robot', 'flange', 'nozzle', true],
    ]), 'robot', profiles)).toEqual([{ interfaceId: 'flange', tools: ['hand', 'nozzle'], primary: 'nozzle' }]);
    // The tools' own page has no choice.
    expect(primaryToolChoices(draft(components, [
      ['robot', 'flange', 'hand'], ['robot', 'flange', 'nozzle'],
    ]), 'hand', profiles)).toEqual([]);
  });
});

describe('Robot without a TCP (B-run-1)', () => {
  it('warns while the primary flange tool has no TCP', () => {
    const components: Array<[string, string]> = [['robot', 'arm_v0'], ['hand', 'gripper_v0'], ['bracket', 'bracket_v0']];
    expect(toolsWithoutTcp(draft(components, [['robot', 'flange', 'hand']]), profiles)).toEqual([]);
    expect(toolsWithoutTcp(draft(components, []), profiles)).toEqual([]);
    const warned = toolsWithoutTcp(draft(components, [['robot', 'flange', 'bracket']]), profiles);
    expect(warned).toEqual([{ robotId: 'robot', toolId: 'bracket' }]);
    expect(toolWithoutTcpWarning(warned[0])).toBe(
      'bracket on robot has no TCP; Programs can\'t run until one of its frames is marked as a TCP.');
    // The primary tool decides: a gripper marked primary next to the bracket.
    expect(toolsWithoutTcp(draft(components, [
      ['robot', 'flange', 'bracket'], ['robot', 'flange', 'hand', true],
    ]), profiles)).toEqual([]);
    expect(toolsWithoutTcp(draft(components, [
      ['robot', 'flange', 'hand'], ['robot', 'flange', 'bracket', true],
    ]), profiles)).toEqual([{ robotId: 'robot', toolId: 'bracket' }]);
  });
});
