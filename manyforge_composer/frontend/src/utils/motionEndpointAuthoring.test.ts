import { describe, expect, it } from 'vitest';

import type { ProgramAuthoringDocument, ProgramTreeNode } from '../types/api';
import {
  buildMotionEndpointMutation,
  motionEndpointChoiceKey,
  motionEndpointKeyOfNode,
  programMotionEndpointChoices,
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

  it('reads only an explicit target/frame endpoint', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.move_to_pose',
      instance_id: 'move',
      parameters: { tcp_endpoint: { target_role: 'arm', frame_role: 'tcp' } },
      children: [],
    };

    expect(motionEndpointKeyOfNode(node)).toBe(motionEndpointChoiceKey('arm', 'tcp'));
    expect(node.parameters).toEqual({ tcp_endpoint: { target_role: 'arm', frame_role: 'tcp' } });
    expect(motionEndpointKeyOfNode({
      ...node,
      parameters: { target_role: 'arm' },
    })).toBeNull();
  });

  it('replaces the endpoint atomically and preserves unrelated values and sources', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.move_to_pose',
      instance_id: 'move',
      parameters: {
        target_role: 'arm',
        tcp_endpoint: { target_role: 'arm', frame_role: 'tcp' },
        goal: { position_m: [0, 0, 0] },
      },
      value_sources: {
        velocity_scale: { kind: 'program_parameter', reference: 'motion_scale' },
      },
      children: [],
    };
    const mutation = buildMotionEndpointMutation(node, {
      target_role: 'tool', frame_role: 'jaw_contact',
    });

    expect(mutation.args.parameters).toEqual({
      target_role: 'arm',
      goal: { position_m: [0, 0, 0] },
      tcp_endpoint: { target_role: 'tool', frame_role: 'jaw_contact' },
    });
    expect(mutation.args.value_sources).toEqual(node.value_sources);
    expect(node.parameters.tcp_endpoint).toEqual({ target_role: 'arm', frame_role: 'tcp' });
  });
});
