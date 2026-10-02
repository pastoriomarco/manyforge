import { describe, expect, it } from 'vitest';
import { Euler, Quaternion } from 'three';
import type { ProgramBindingDocument } from '../types/api';
import {
  boundProgramTargetRoleIds,
  canonicalProgramPose,
  computeCartesianMoveDraft,
  cartesianMoveOffsetsFromTarget,
  resolveStoredProgramPose,
} from './moveTo';
import type { PoseTuple } from './poseTransforms';
import { composePose, relativePose, samePose } from './poseTransforms';

const entity: PoseTuple = {
  position: [1, 2, 3],
  quaternion: [0, 0, 0, 1],
};

describe('Cartesian move authoring', () => {
  it('keeps translation in source axes independently of target orientation', () => {
    const source = { ...entity, quaternion: [0, 0, Math.SQRT1_2, Math.SQRT1_2] as PoseTuple['quaternion'] };
    const options = { orientationMode: 'keep-current' as const, offsetXyzM: [0.1, 0, 0] as [number, number, number], offsetRpyRad: [0, 0, 0] as [number, number, number] };
    const target = computeCartesianMoveDraft(source, entity, options)!;
    expect(target.position).toEqual([1, 2.1, 3]);
    const rotated = computeCartesianMoveDraft(source, entity, { ...options, offsetRpyRad: [0, 0, Math.PI / 2] })!;
    expect(rotated.position).toEqual(target.position);
    expect(rotated.quaternion).not.toEqual(target.quaternion);
  });

  it('round-trips all orientation policies, rotated bases, wrapped and singular offsets without drift', () => {
    const source = { ...entity, quaternion: new Quaternion().setFromEuler(new Euler(0.2, 0.3, -0.6)).toArray() };
    const base = { position: [2, -1, 0.4] as [number, number, number], quaternion: new Quaternion().setFromEuler(new Euler(-0.1, 0.4, 1.3)).toArray() };
    for (const orientationMode of ['keep-current', 'use-entity', 'face-down'] as const) {
      for (const pitch of [0.8, Math.PI / 2 - 1e-7, Math.PI / 2, -Math.PI / 2]) {
        let options = { orientationMode, offsetXyzM: [0.12, -0.04, 0.03] as [number, number, number], offsetRpyRad: [3.2, pitch, -3.3] as [number, number, number] };
        const expected = computeCartesianMoveDraft(source, entity, options)!;
        for (let count = 0; count < 8; count += 1) {
          const target = relativePose(composePose(base, computeCartesianMoveDraft(source, entity, options)!), base);
          options = cartesianMoveOffsetsFromTarget(source, entity, options, target)!;
        }
        expect(samePose(expected, computeCartesianMoveDraft(source, entity, options)!)).toBe(true);
        expect(Math.abs(options.offsetRpyRad[0] - 3.2)).toBeLessThan(1e-7);
      }
    }
  });

  it('rejects invalid inverse targets without an inferred reference', () => {
    const options = { orientationMode: 'keep-current' as const, offsetXyzM: [0, 0, 0] as [number, number, number], offsetRpyRad: [0, 0, 0] as [number, number, number] };
    expect(cartesianMoveOffsetsFromTarget(entity, null, options, entity)).toBeNull();
    expect(cartesianMoveOffsetsFromTarget(entity, entity, options, { ...entity, quaternion: [0, 0, 0, 0] })).toBeNull();
  });
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

  it('reads stored RPY with the backend intrinsic ZYX convention', () => {
    // Backend geometry.rpy_deg_to_xyzw([90, 0, 90]) = [0.5, 0.5, 0.5, 0.5].
    const pose = canonicalProgramPose({
      position_m: [0, 0, 0],
      orientation: { rpy_deg: [90, 0, 90] },
    });
    for (const [index, expected] of [0.5, 0.5, 0.5, 0.5].entries()) {
      expect(pose?.quaternion[index]).toBeCloseTo(expected);
    }
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
