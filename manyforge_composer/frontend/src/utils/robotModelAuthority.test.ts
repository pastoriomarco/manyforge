import { describe, expect, it } from 'vitest';

import type { SceneUrdfManifest } from '../types/api';
import {
  robotControlJointNamesForGroup,
  robotControlJointsForGroup,
} from './robotModelAuthority';

type ManifestJoint = NonNullable<SceneUrdfManifest['joints']>[number];

function joint(name: string, options: Partial<ManifestJoint> = {}): ManifestJoint {
  return {
    name,
    type: 'revolute',
    axis: [1, 0, 0],
    parent_link: 'base',
    child_link: `${name}_link`,
    limit_lower: -1,
    limit_upper: 1,
    effort_limit: 1,
    velocity_limit: 1,
    ...options,
  };
}

describe('Robot Model Authority UI helpers', () => {
  it('limits Robot Control jogging to the active arm group command joints', () => {
    const manifest: SceneUrdfManifest = {
      robot_id: 'lite6',
      root_link: 'link_base',
      links: [],
      joints: [
        joint('joint1'),
        joint('joint2'),
        joint('joint3'),
        joint('joint4'),
        joint('joint5'),
        joint('joint6'),
        joint('right_finger_joint', {
          type: 'prismatic',
          is_mimic_master: true,
        }),
        joint('left_finger_joint', {
          type: 'prismatic',
          mimic: { master: 'right_finger_joint', multiplier: 1, offset: 0 },
        }),
      ],
      authority: {
        group_commandable_joint_names: {
          lite6: ['joint1', 'joint2', 'joint3', 'joint4', 'joint5', 'joint6'],
          lite_gripper: ['right_finger_joint'],
        },
      },
    };

    expect(robotControlJointNamesForGroup(manifest, 'lite6')).toEqual([
      'joint1',
      'joint2',
      'joint3',
      'joint4',
      'joint5',
      'joint6',
    ]);
    expect(robotControlJointNamesForGroup(manifest, 'lite_gripper')).toEqual([
      'right_finger_joint',
    ]);
  });

  it('falls back to hiding mimic chains when no authority metadata is present', () => {
    const manifest: SceneUrdfManifest = {
      robot_id: 'legacy',
      root_link: 'base',
      links: [],
      joints: [
        joint('shoulder_pan_joint'),
        joint('finger_joint', { is_mimic_master: true }),
        joint('left_inner_finger_joint', {
          mimic: { master: 'finger_joint', multiplier: 1, offset: 0 },
        }),
      ],
    };

    expect(robotControlJointsForGroup(manifest, 'arm').map((j) => j.name)).toEqual([
      'shoulder_pan_joint',
    ]);
  });
});
