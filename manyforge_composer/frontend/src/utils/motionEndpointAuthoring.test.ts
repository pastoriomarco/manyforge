import { describe, expect, it } from 'vitest';

import type { JsonValue, ProgramAuthoringDocument, ProgramTreeNode } from '../types/api';
import {
  availableMotionTcps,
  buildMotionEndpointMutation,
  hasUnrepresentedTcpOwner,
  motionEndpointChoiceKey,
  motionEndpointKeyOfNode,
  programMotionEndpointChoices,
  programTcpSelectorChoices,
  tcpSelectorKey,
} from './motionEndpointAuthoring';

const program = {
  target_roles: {
    arm: { required_frame_roles: ['base', 'tcp'] },
    tool: { required_frame_roles: ['tcp', 'jaw_contact'] },
    undeclared: { required_frame_roles: [] },
  },
} as unknown as Pick<ProgramAuthoringDocument, 'target_roles'>;

describe('motion endpoint authoring', () => {
  it('offers only explicitly declared target/frame pairs in stable order', () => {
    expect(programMotionEndpointChoices(program).map(({ label }) => label)).toEqual([
      'arm · base', 'arm · tcp', 'tool · jaw_contact', 'tool · tcp',
    ]);
  });

  it('labels a frame with what it resolves to, keeping the key', () => {
    const choices = programMotionEndpointChoices(program, (targetRole, frameRole) => (
      targetRole === 'arm' && frameRole === 'tcp' ? 'lite_gripper default TCP' : null
    ));
    expect(choices.map(({ label }) => label)).toEqual([
      'arm · base', 'arm · tcp → lite_gripper default TCP', 'tool · jaw_contact', 'tool · tcp',
    ]);
    expect(choices[1].key).toBe(motionEndpointChoiceKey('arm', 'tcp'));
  });

  it('reads only an explicit target/frame endpoint', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.move_to_pose',
      instance_id: 'move',
      parameters: { tcp_endpoint: { kind: 'entry', target_role: 'arm', frame_role: 'tcp' } },
      children: [],
    };

    expect(motionEndpointKeyOfNode(node)).toBe(motionEndpointChoiceKey('arm', 'tcp'));
    expect(node.parameters).toEqual({ tcp_endpoint: { kind: 'entry', target_role: 'arm', frame_role: 'tcp' } });
    expect(motionEndpointKeyOfNode({
      ...node,
      parameters: { target_role: 'arm' },
    })).toBe(tcpSelectorKey({ kind: 'current' }));
  });

  it('does not display malformed literal selectors as Current', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.move_to_pose', instance_id: 'move', parameters: {}, children: [],
    };
    expect(motionEndpointKeyOfNode(node)).toBe(tcpSelectorKey({ kind: 'current' }));
    const malformed: JsonValue[] = [null, [], 'current', 1, {}, { kind: [] }, { kind: {} }];
    for (const value of malformed) {
      expect(motionEndpointKeyOfNode({ ...node, parameters: { tcp_endpoint: value } })).toBeNull();
    }
  });

  it('replaces the endpoint atomically and preserves unrelated values and sources', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.move_to_pose',
      instance_id: 'move',
      parameters: {
        target_role: 'arm',
        tcp_endpoint: { kind: 'entry', target_role: 'arm', frame_role: 'tcp' },
        goal: { position_m: [0, 0, 0] },
      },
      value_sources: {
        velocity_scale: { kind: 'program_parameter', reference: 'motion_scale' },
      },
      children: [],
    };
    const mutation = buildMotionEndpointMutation(node, {
      kind: 'entry', target_role: 'tool', frame_role: 'jaw_contact',
    });

    expect(mutation.args.parameters).toEqual({
      target_role: 'arm',
      goal: { position_m: [0, 0, 0] },
      tcp_endpoint: { kind: 'entry', target_role: 'tool', frame_role: 'jaw_contact' },
    });
    expect(mutation.args.value_sources).toEqual(node.value_sources);
    expect(node.parameters.tcp_endpoint).toEqual({ kind: 'entry', target_role: 'arm', frame_role: 'tcp' });
  });

  it('keeps selector keys distinct even when roles resemble kind names', () => {
    expect(tcpSelectorKey({ kind: 'default_of_role', role: 'tip' })).not.toBe(
      tcpSelectorKey({ kind: 'entry', target_role: 'default_of_role', frame_role: 'tip' }),
    );
  });

  it('keeps symbolic defaults distinct and writes Set TCP selectors', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.set_tcp', instance_id: 'set', parameters: {}, children: [],
    };
    const selector = { kind: 'default_of_role' as const, role: 'tool' };
    const mutation = buildMotionEndpointMutation(node, selector, 'tcp');
    expect(mutation.args.parameters).toEqual({ tcp: selector });
    expect(motionEndpointKeyOfNode({ ...node, parameters: { tcp: selector } }, 'tcp'))
      .toBe(tcpSelectorKey(selector));
  });

  describe('TCP-C: TCPs the Deployment offers', () => {
    const bound = [
      { target_role_id: 'arm', controllable_component_id: 'lite6_robot', command_target_id: 'arm' },
      { target_role_id: 'tool', controllable_component_id: 'lite_gripper', command_target_id: 'gripper' },
    ];
    const robotRow = (frameRoleId: string, qualifiedName: string, isDefault = false) => ({
      controllableComponentId: 'lite6_robot', commandTargetId: 'arm', frameRoleId, qualifiedName, isDefault, mountedHere: true,
    });
    const toolRow = (frameRoleId: string) => ({
      controllableComponentId: 'lite_gripper', commandTargetId: 'gripper', frameRoleId,
      qualifiedName: `lite_gripper.${frameRoleId}`, isDefault: false, mountedHere: true,
    });

    // The default's owner-qualified name says which point it is in each of
    // the three cases of the default rule.
    it.each([
      ['a mounted tool: its default TCP', 'tcp', 'lite_gripper.tcp'],
      ['no tool: the robot\'s own default TCP', 'tcp', 'lite6_robot.tcp'],
      ['no tool and no robot tcp: the flange', 'flange', 'lite6_robot.flange'],
    ])('labels the default with its owner: %s', (_case, frameRole, qualified) => {
      const available = availableMotionTcps(bound, [{ tcps: [robotRow(frameRole, qualified, true)] }]);
      const choice = programMotionEndpointChoices(program, () => null, available)
        .find((candidate) => candidate.key === motionEndpointChoiceKey('arm', frameRole));
      expect(choice?.label).toBe(`${qualified} (default) · arm · ${frameRole}`);
    });

    it('offers the mounted tool\'s TCPs on the tool\'s target role, and Draft custom TCPs', () => {
      const available = availableMotionTcps(bound, [{ tcps: [
        robotRow('tcp', 'lite_gripper.tcp', true),
        robotRow('flange', 'lite6_robot.flange'),
        robotRow('pen', 'lite6_robot.pen'),
        toolRow('cup_edge'),
      ] }], (componentId) => (componentId === 'lite6_robot' ? ['pen', 'probe']
        : componentId === 'lite_gripper' ? ['jaw'] : []));
      const labels = programMotionEndpointChoices(program, () => null, available).map(({ label }) => label);
      expect(labels).toEqual(expect.arrayContaining([
        'lite_gripper.tcp (default) · arm · tcp',
        'lite6_robot.flange · arm · flange',
        'lite6_robot.pen · arm · pen',
        'lite6_robot.probe (after Apply) · arm · probe',
        // A Program reaches a tool TCP as {tool role, id}, never {arm, id}.
        'lite_gripper.cup_edge · tool · cup_edge',
        'lite_gripper.jaw (after Apply) · tool · jaw',
        'arm · base',
      ]));
      expect(labels.some((label) => label.endsWith('· arm · cup_edge'))).toBe(false);
      // Applied once, not again as a Draft custom TCP.
      expect(labels.filter((label) => label.includes('.pen'))).toHaveLength(1);
    });

    it('hides other robots by default, while showing unmounted tools', () => {
      const mounted = robotRow('tcp', 'lite6_robot.tcp', true);
      const elsewhere = { ...toolRow('other_tip'), mountedHere: false, mountedElsewhere: true };
      const unmounted = { ...toolRow('free_tip'), mountedHere: false, mountedElsewhere: false };
      const hidden = availableMotionTcps(bound, [{ tcps: [mounted, elsewhere, unmounted] }]);
      expect(hidden.map((tcp) => tcp.frameRole)).toEqual(['tcp', 'free_tip']);
      const shown = availableMotionTcps(bound, [{ tcps: [mounted, elsewhere, unmounted] }], () => [], true);
      expect(shown.map((tcp) => tcp.frameRole)).toEqual(['tcp', 'other_tip', 'free_tip']);
    });

    it('filters both entries and role defaults belonging to another robot', () => {
      const descriptor = { tcps: [
        { ...robotRow('tcp', 'lite6_robot.tcp', true), mountedElsewhere: false },
        { ...toolRow('tip'), mountedHere: false, mountedElsewhere: true },
      ] };
      const hidden = programTcpSelectorChoices(program, bound, descriptor, false);
      expect(hidden.some((choice) => choice.label === 'Default TCP of tool')).toBe(false);
      expect(hidden.some((choice) => choice.label.includes('lite_gripper.tip'))).toBe(false);
      const shown = programTcpSelectorChoices(program, bound, descriptor, true);
      expect(shown.some((choice) => choice.label === 'Default TCP of tool')).toBe(true);
      expect(shown.some((choice) => choice.label.includes('lite_gripper.tip'))).toBe(true);
    });

    it('omits unrepresented owners and indicates that a role is needed', () => {
      const descriptor = { tcps: [
        { ...robotRow('tcp', 'lite6_robot.tcp', true), mountedElsewhere: false },
        { ...toolRow('tip'), mountedElsewhere: false },
      ] };
      const armOnly = bound.slice(0, 1);
      expect(programTcpSelectorChoices(program, armOnly, descriptor, false)
        .some((choice) => choice.label.includes('lite_gripper.tip'))).toBe(false);
      expect(hasUnrepresentedTcpOwner(program, armOnly, descriptor, false)).toBe(true);
      expect(hasUnrepresentedTcpOwner(program, bound, descriptor, false)).toBe(false);
      const elsewhere = { tcps: [{ ...descriptor.tcps[1], mountedElsewhere: true }] };
      expect(hasUnrepresentedTcpOwner(program, armOnly, elsewhere, false)).toBe(false);
      expect(hasUnrepresentedTcpOwner(program, armOnly, elsewhere, true)).toBe(true);
      expect(hasUnrepresentedTcpOwner(program, armOnly, undefined, false)).toBe(false);
    });

    it('offers a Draft custom TCP on every Applied target of a component that carries TCPs', () => {
      const twoTargets = [
        { target_role_id: 'left', controllable_component_id: 'dual_arm', command_target_id: 'left_arm' },
        { target_role_id: 'right', controllable_component_id: 'dual_arm', command_target_id: 'right_arm' },
      ];
      const row = (commandTargetId: string) => ({
        controllableComponentId: 'dual_arm', commandTargetId, frameRoleId: 'tcp',
        qualifiedName: 'dual_arm.tcp', isDefault: true, mountedHere: true,
      });
      const available = availableMotionTcps(twoTargets, [{ tcps: [row('left_arm')] }, { tcps: [row('right_arm')] }],
        (componentId) => (componentId === 'dual_arm' ? ['probe'] : []));
      expect(available.filter((tcp) => tcp.draftOnly).map((tcp) => [tcp.targetRole, tcp.frameRole]))
        .toEqual([['left', 'probe'], ['right', 'probe']]);
    });
  });
});
