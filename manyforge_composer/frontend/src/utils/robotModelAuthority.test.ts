import { describe, expect, it } from 'vitest';
import type { SceneUrdfJoint, SceneUrdfManifest } from '../types/api';
import { modelJointsForQualifiedTarget } from './robotModelAuthority';

function joint(name: string, type: string): SceneUrdfJoint {
  return {
    name,
    type,
    axis: [1, 0, 0],
    parent_link: 'base',
    child_link: `${name}_link`,
    origin: {
      relative_to: 'base',
      position_m: [0, 0, 0],
      orientation_rpy_rad: [0, 0, 0],
      orientation_xyzw: [0, 0, 0, 1],
    },
    coordinate_unit: type === 'prismatic' ? 'm' : type === 'fixed' ? null : 'rad',
    neutral_position: type === 'fixed' ? null : 0,
    limit_lower: null,
    limit_upper: null,
    effort_limit: null,
    velocity_limit: null,
    mimic: null,
    is_mimic_master: false,
  };
}

function manifest(): SceneUrdfManifest {
  return {
    robot_id: 'robot-1',
    model_key: 'sha256:model',
    root_link: 'base',
    projection: {
      kind: 'authored_neutral',
      source: 'authored_urdf',
      root_frame: 'base',
      position_unit: 'm',
      angle_unit: 'rad',
      quaternion_order: 'xyzw',
      joint_coordinate_policy: 'zero_independent_with_authored_mimic',
      operational_validity: 'not_asserted',
      runtime_state_used: false,
    },
    links: [],
    joints: [joint('joint_1', 'revolute'), joint('finger_slide', 'prismatic'), joint('fixed', 'fixed')],
  };
}

describe('qualified model target selection', () => {
  it('preserves explicit ordered revolute/prismatic target membership', () => {
    expect(
      modelJointsForQualifiedTarget(manifest(), ['joint_1', 'finger_slide'])
        ?.map((value) => [value.name, value.coordinate_unit]),
    ).toEqual([['joint_1', 'rad'], ['finger_slide', 'm']]);
  });

  it('does not invent legacy group or mimic fallbacks', () => {
    expect(modelJointsForQualifiedTarget(manifest(), null)).toBeNull();
    expect(modelJointsForQualifiedTarget(manifest(), ['missing'])).toBeNull();
    expect(modelJointsForQualifiedTarget(manifest(), ['fixed'])).toBeNull();
  });
});
