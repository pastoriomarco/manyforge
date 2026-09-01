import { describe, expect, it } from 'vitest';
import type { ProgramBindingDocument } from '../types/api';
import {
  boundProgramTargetRoleIds,
  computeCartesianMoveDraft,
  resolveStoredProgramPose,
} from './moveTo';
import type { PoseTuple } from './poseTransforms';

const entity: PoseTuple = {
  position: [1, 2, 3],
  quaternion: [0, 0, 0, 1],
};

describe('Cartesian move authoring', () => {
  it('applies local offsets to the selected entity orientation', () => {
    expect(
      computeCartesianMoveDraft(entity, null, {
        orientationMode: 'use-entity',
        offsetXyzM: [0, 0, 0.1],
        offsetRpyRad: [0, 0, 0],
      }),
    ).toEqual({
      position: [1, 2, 3.1],
      quaternion: [0, 0, 0, 1],
    });
  });

  it('fails closed when keep-current has no observed TCP pose', () => {
    expect(
      computeCartesianMoveDraft(entity, null, {
        orientationMode: 'keep-current',
        offsetXyzM: [0, 0, 0],
        offsetRpyRad: [0, 0, 0],
      }),
    ).toBeNull();
  });
});

describe('stored Move To sources', () => {
  const binding = {
    target_role_bindings: [
      {
        target_role_id: 'arm',
        controllable_component_id: 'lite6_robot',
        command_target_id: 'arm',
      },
      {
        target_role_id: 'gripper',
        controllable_component_id: 'lite6_gripper',
        command_target_id: 'gripper',
      },
    ],
  } as unknown as ProgramBindingDocument;

  const identity: PoseTuple = {
    position: [0, 0, 0],
    quaternion: [0, 0, 0, 1],
  };
  const context = {
    programTargetRoleId: 'arm',
    baseFrameRoleId: 'base',
    sceneResourcePoses: new Map<string, PoseTuple>([
      ['graspable', {
        position: [0.18, -0.25, 0.28],
        quaternion: [0, 0, 0, 1],
      }],
    ]),
    frameRolePoses: new Map<string, PoseTuple>([['base', identity]]),
  };

  it('selects Program roles through the exact applied target binding', () => {
    expect(boundProgramTargetRoleIds(binding, 'lite6_robot', 'arm')).toEqual(['arm']);
    expect(boundProgramTargetRoleIds(binding, 'lite6_robot', 'gripper')).toEqual([]);
    expect(boundProgramTargetRoleIds(null, 'lite6_robot', 'arm')).toEqual([]);
  });

  it('accepts an absolute target only in the exact bound base frame', () => {
    const goal = {
      frame_ref: { kind: 'target_frame', target_role: 'arm', frame_role: 'base' },
      position_m: [0.16, 0.08, 0.2],
      orientation: { rpy_deg: [180, 0, 0] },
    };
    const result = resolveStoredProgramPose({ goal }, context);
    expect(result?.position).toEqual([0.16, 0.08, 0.2]);
    expect(result?.quaternion[0]).toBeCloseTo(1);
    expect(resolveStoredProgramPose({
      goal: { ...goal, frame_ref: { ...goal.frame_ref, target_role: 'other' } },
    }, context)).toBeNull();
  });

  it('resolves relative scene goals using the explicit offset-reference axes', () => {
    const result = resolveStoredProgramPose({
      target_mode: 'relative',
      target_reference: { type: 'scene_resource', scene_resource_id: 'graspable' },
      offset_reference: { type: 'frame_role', frame_role_id: 'base' },
      goal: {
        position_m: [0, 0, 0.1],
        orientation: { rpy_deg: [180, 0, 0] },
      },
    }, context);
    expect(result?.position[0]).toBeCloseTo(0.18);
    expect(result?.position[1]).toBeCloseTo(-0.25);
    expect(result?.position[2]).toBeCloseTo(0.38);
    expect(result?.quaternion[0]).toBeCloseTo(1);
  });

  it('uses the target axes when a relative goal omits offset_reference', () => {
    const rotatedContext = {
      ...context,
      frameRolePoses: new Map<string, PoseTuple>([
        ['tcp', {
          position: [1, 2, 3],
          quaternion: [0, 0, Math.SQRT1_2, Math.SQRT1_2],
        }],
      ]),
    };
    const result = resolveStoredProgramPose({
      target_mode: 'relative',
      target_reference: { type: 'frame_role', frame_role_id: 'tcp' },
      goal: {
        position_m: [0.1, 0, 0],
        orientation: { xyzw: [0, 0, 0, 1] },
      },
    }, rotatedContext);
    expect(result?.position[0]).toBeCloseTo(1);
    expect(result?.position[1]).toBeCloseTo(2.1);
    expect(result?.position[2]).toBeCloseTo(3);
  });

  it('fails closed for an unavailable or unsupported reference', () => {
    const goal = {
      position_m: [0, 0, 0],
      orientation: { xyzw: [0, 0, 0, 1] },
    };
    expect(resolveStoredProgramPose({
      target_mode: 'relative',
      target_reference: { type: 'scene_resource', scene_resource_id: 'missing' },
      goal,
    }, context)).toBeNull();
    expect(resolveStoredProgramPose({
      target_mode: 'relative',
      target_reference: { type: 'unknown' },
      goal,
    }, context)).toBeNull();
  });
});
