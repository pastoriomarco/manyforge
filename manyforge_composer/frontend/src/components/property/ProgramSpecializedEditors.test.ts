import { describe, expect, it } from 'vitest';

import type { CatalogJsonSchema } from '../../types/api';
import {
  completeCoordinateKindTransition,
  completeFrameReferenceTransition,
  completeJointAdditionTransition,
  completeMotionPoseFrameAddTransition,
  completeMotionPoseFrameRemoveTransition,
  completeMotionReferenceTransition,
  completeOrientationTransition,
} from './ProgramSpecializedEditors';

const frameReferenceSchema: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['kind'],
  properties: {
    kind: { type: 'string', enum: ['cell_frame', 'target_frame', 'scene_resource'] },
    target_role: { type: 'string' },
    frame_role: { type: 'string' },
    scene_resource_id: { type: 'string' },
  },
  allOf: [
    {
      if: { required: ['kind'], properties: { kind: { const: 'target_frame' } } },
      then: { required: ['frame_role', 'target_role'] },
    },
    {
      if: { not: { required: ['kind'], properties: { kind: { const: 'target_frame' } } } },
      then: { not: { required: ['target_role'] } },
    },
    {
      if: { not: { required: ['kind'], properties: { kind: { const: 'target_frame' } } } },
      then: { not: { required: ['frame_role'] } },
    },
    {
      if: { required: ['kind'], properties: { kind: { const: 'scene_resource' } } },
      then: { required: ['scene_resource_id'] },
    },
  ],
};

const motionPoseSchema: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['position_m', 'orientation'],
  properties: {
    position_m: {
      type: 'array',
      minItems: 3,
      maxItems: 3,
      items: { type: 'number' },
    },
    orientation: {
      type: 'object',
      additionalProperties: false,
      properties: {
        rpy_deg: {
          type: 'array',
          minItems: 3,
          maxItems: 3,
          items: { type: 'number' },
        },
        xyzw: {
          type: 'array',
          minItems: 4,
          maxItems: 4,
          items: { type: 'number' },
        },
      },
      allOf: [{ oneOf: [{ required: ['rpy_deg'] }, { required: ['xyzw'] }] }],
    },
    frame_ref: frameReferenceSchema,
  },
};

const jointTargetSchema: CatalogJsonSchema = {
  type: 'array',
  minItems: 1,
  maxItems: 3,
  items: {
    type: 'object',
    additionalProperties: false,
    required: ['joint_name', 'position_rad'],
    properties: {
      joint_name: { type: 'string', minLength: 1 },
      position_rad: { type: 'number', minimum: -3.2, maximum: 3.2 },
    },
  },
};

const coordinateSchema: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['kind'],
  properties: {
    kind: { type: 'string', enum: ['angular_rad', 'linear_m'] },
    value_rad: { type: 'number' },
    value_m: { type: 'number' },
  },
  allOf: [
    {
      if: { required: ['kind'], properties: { kind: { const: 'angular_rad' } } },
      then: { required: ['value_rad'], not: { required: ['value_m'] } },
    },
    {
      if: { required: ['kind'], properties: { kind: { const: 'linear_m' } } },
      then: { required: ['value_m'], not: { required: ['value_rad'] } },
    },
  ],
};

const motionReferenceSchema: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['type'],
  properties: {
    type: {
      type: 'string',
      enum: ['frame_role', 'component_body_role', 'scene_resource', 'runtime_object', 'pose'],
    },
    frame_role_id: { type: 'string' },
    collision_body_role_id: { type: 'string' },
    scene_resource_id: { type: 'string' },
    runtime_object_id: { type: 'string' },
    pose: motionPoseSchema,
  },
  allOf: [
    ...[
      ['frame_role', 'frame_role_id'],
      ['component_body_role', 'collision_body_role_id'],
      ['scene_resource', 'scene_resource_id'],
      ['runtime_object', 'runtime_object_id'],
      ['pose', 'pose'],
    ].flatMap(([type, field]) => [
      {
        if: { required: ['type'], properties: { type: { const: type } } },
        then: { required: [field] },
      },
      {
        if: { not: { required: ['type'], properties: { type: { const: type } } } },
        then: { not: { required: [field] } },
      },
    ]),
  ],
};

describe('Program specialized editor transitions', () => {
  it('stages target_frame until both roles exist and normalizes every frame variant', () => {
    expect(completeFrameReferenceTransition(
      frameReferenceSchema,
      'target_frame',
      { target_role: 'arm' },
    )).toBeUndefined();
    expect(completeFrameReferenceTransition(
      frameReferenceSchema,
      'target_frame',
      { target_role: 'arm', frame_role: 'tcp', scene_resource_id: 'stale' },
    )).toEqual({
      kind: 'target_frame',
      target_role: 'arm',
      frame_role: 'tcp',
    });
    expect(completeFrameReferenceTransition(
      frameReferenceSchema,
      'cell_frame',
      { target_role: 'stale', frame_role: 'stale' },
    )).toEqual({ kind: 'cell_frame' });
    expect(completeFrameReferenceTransition(
      frameReferenceSchema,
      'scene_resource',
      { scene_resource_id: 'fixture', target_role: 'stale' },
    )).toEqual({ kind: 'scene_resource', scene_resource_id: 'fixture' });
  });

  it.each([
    ['frame_role', 'frame_role_id'],
    ['component_body_role', 'collision_body_role_id'],
    ['scene_resource', 'scene_resource_id'],
    ['runtime_object', 'runtime_object_id'],
  ])('stages and normalizes the %s motion-reference transition', (type, field) => {
    expect(completeMotionReferenceTransition(
      motionReferenceSchema,
      type,
    )).toBeUndefined();
    expect(completeMotionReferenceTransition(
      motionReferenceSchema,
      type,
      'selected-role',
    )).toEqual({ type, [field]: 'selected-role' });
  });

  it('normalizes the pose motion-reference transition to one valid pose variant', () => {
    expect(completeMotionReferenceTransition(
      motionReferenceSchema,
      'pose',
    )).toEqual({
      type: 'pose',
      pose: {
        position_m: [0, 0, 0],
        orientation: { rpy_deg: [0, 0, 0] },
      },
    });
  });

  it('adds and removes only schema-valid optional frame references', () => {
    const pose = {
      position_m: [0, 0, 0],
      orientation: { rpy_deg: [0, 0, 0] },
    };
    const added = completeMotionPoseFrameAddTransition(motionPoseSchema, pose);
    expect(added).toEqual({
      ...pose,
      frame_ref: { kind: 'cell_frame' },
    });
    expect(completeMotionPoseFrameAddTransition(
      motionPoseSchema,
      added!,
    )).toBeUndefined();
    expect(completeMotionPoseFrameRemoveTransition(
      motionPoseSchema,
      added!,
    )).toEqual(pose);

    const requiredFrameSchema = {
      ...motionPoseSchema,
      required: ['position_m', 'orientation', 'frame_ref'],
    };
    expect(completeMotionPoseFrameRemoveTransition(
      requiredFrameSchema,
      added!,
    )).toBeUndefined();

    const stagedOnlyFrameSchema = {
      ...motionPoseSchema,
      properties: {
        ...motionPoseSchema.properties,
        frame_ref: {
          ...frameReferenceSchema,
          properties: {
            ...frameReferenceSchema.properties,
            kind: { type: 'string' as const, enum: ['target_frame'] },
          },
        },
      },
    };
    expect(completeMotionPoseFrameAddTransition(
      stagedOnlyFrameSchema,
      pose,
    )).toBeUndefined();
  });

  it('replaces the orientation variant without carrying its incompatible field', () => {
    const pose = {
      position_m: [0, 0, 0],
      orientation: { rpy_deg: [10, 20, 30] },
      frame_ref: { kind: 'cell_frame' },
    };
    expect(completeOrientationTransition(
      motionPoseSchema,
      pose,
      'xyzw',
    )).toEqual({
      ...pose,
      orientation: { xyzw: [0, 0, 0, 1] },
    });
    expect(completeOrientationTransition(
      motionPoseSchema,
      pose,
      'unsupported',
    )).toBeUndefined();
  });

  it('stages joint addition without mutating existing rows until the row is complete', () => {
    const existing = [{ joint_name: 'joint_a', position_rad: 0.2 }];
    expect(completeJointAdditionTransition(
      jointTargetSchema,
      existing,
      '',
      0,
    )).toBeUndefined();
    expect(completeJointAdditionTransition(
      jointTargetSchema,
      existing,
      'joint_b',
      undefined,
    )).toBeUndefined();
    expect(completeJointAdditionTransition(
      jointTargetSchema,
      existing,
      'joint_b',
      9,
    )).toBeUndefined();
    expect(completeJointAdditionTransition(
      jointTargetSchema,
      existing,
      ' joint_b ',
      -0.4,
    )).toEqual([
      ...existing,
      { joint_name: 'joint_b', position_rad: -0.4 },
    ]);
    expect(existing).toEqual([{ joint_name: 'joint_a', position_rad: 0.2 }]);
    expect(completeJointAdditionTransition(
      jointTargetSchema,
      [
        ...existing,
        { joint_name: 'joint_b', position_rad: 0 },
        { joint_name: 'joint_c', position_rad: 0 },
      ],
      'joint_d',
      0,
    )).toBeUndefined();
  });

  it('replaces coordinate kind and omits the stale incompatible coordinate', () => {
    expect(completeCoordinateKindTransition(
      coordinateSchema,
      'linear_m',
    )).toEqual({ kind: 'linear_m', value_m: 0 });
    expect(completeCoordinateKindTransition(
      coordinateSchema,
      'unsupported',
    )).toBeUndefined();
  });
});
