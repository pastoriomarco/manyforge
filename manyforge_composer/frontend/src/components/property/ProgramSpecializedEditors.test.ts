import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import type { CatalogJsonSchema } from '../../types/api';
import {
  completeCoordinateKindTransition,
  completeFrameReferenceTransition,
  completeJointTargetTransition,
  completeMotionPoseFrameAddTransition,
  completeMotionPoseFrameRemoveTransition,
  completeMotionReferenceTransition,
  completeOrientationTransition,
  jointTargetEditorRows,
  jointTargetTransitionMessage,
  ProgramSpecializedEditor,
  toggleJointTargetRow,
} from './ProgramSpecializedEditors';
import { firstCatalogSchemaValidationMessage } from '../../utils/catalogSchema';

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

const namedCoordinateSchema: CatalogJsonSchema = {
  ...coordinateSchema,
  properties: {
    ...coordinateSchema.properties,
    kind: { type: 'string', enum: ['angular_rad', 'linear_m', 'named_preset'] },
    name: { type: 'string', enum: ['open', 'closed'] },
  },
  allOf: [
    ...coordinateSchema.allOf!,
    {
      if: { required: ['kind'], properties: { kind: { const: 'named_preset' } } },
      then: { required: ['name'], not: { anyOf: [{ required: ['value_rad'] }, { required: ['value_m'] }] } },
    },
    {
      if: { not: { required: ['kind'], properties: { kind: { const: 'named_preset' } } } },
      then: { not: { required: ['name'] } },
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

  it('converts the orientation variant without carrying its incompatible field', () => {
    const pose = {
      position_m: [0, 0, 0],
      orientation: { rpy_deg: [90, 0, 90] },
      frame_ref: { kind: 'cell_frame' },
    };
    const converted = completeOrientationTransition(motionPoseSchema, pose, 'xyzw');
    expect(converted).toMatchObject({ position_m: [0, 0, 0], frame_ref: { kind: 'cell_frame' } });
    expect(Object.keys(converted?.orientation as object)).toEqual(['xyzw']);
    ((converted?.orientation as { xyzw: number[] }).xyzw).forEach((component) =>
      expect(component).toBeCloseTo(0.5, 15));
    expect(completeOrientationTransition(
      motionPoseSchema,
      pose,
      'unsupported',
    )).toBeUndefined();
  });

  it('keeps a measured quaternion orientation when switching to RPY and back', () => {
    const measured = [0.0183412, -0.7071123, 0.0192817, 0.7066541];
    const pose = { position_m: [0.4, -0.1, 0.3], orientation: { xyzw: measured } };
    const asRpy = completeOrientationTransition(motionPoseSchema, pose, 'rpy_deg');
    const rpy = (asRpy?.orientation as { rpy_deg: number[] }).rpy_deg;
    expect(Object.keys(asRpy?.orientation as object)).toEqual(['rpy_deg']);
    // Backend manyforge_behavior.geometry.xyzw_to_rpy_deg on the same quaternion.
    [-114.450387380201, -89.91523304804716, 117.49915800287467].forEach((angle, index) =>
      expect(rpy[index]).toBeCloseTo(angle, 6));
    const back = completeOrientationTransition(motionPoseSchema, asRpy ?? pose, 'xyzw');
    const xyzw = (back?.orientation as { xyzw: number[] }).xyzw;
    const norm = Math.hypot(...measured);
    const dot = xyzw.reduce((sum, component, index) => sum + component * measured[index] / norm, 0);
    expect(Math.abs(dot)).toBeCloseTo(1, 12);
  });

  it('shows the other orientation representation read-only under the edited one', () => {
    const render = (orientation: Record<string, number[]>) => renderToStaticMarkup(createElement(
      ProgramSpecializedEditor,
      {
        schema: motionPoseSchema,
        value: { position_m: [0, 0, 0], orientation },
        disabled: false,
        onCommit: () => undefined,
      },
    ));
    expect(render({ rpy_deg: [90, 0, 90] }))
      .toContain('Quaternion xyzw (read-only): x 0.5, y 0.5, z 0.5, w 0.5');
    expect(render({ xyzw: [0.5, 0.5, 0.5, 0.5] }))
      .toContain('RPY degrees (read-only): r 90, p 0, y 90');
  });

  it('shows every commandable joint and preserves sparse authored order', () => {
    const rows = jointTargetEditorRows(
      ['joint_a', 'joint_b', 'joint_c'],
      [
        { joint_name: 'joint_c', position_rad: 0.2 },
        { joint_name: 'joint_a', position_rad: -0.1 },
      ],
    );
    expect(rows).toEqual([
      { jointName: 'joint_c', positionText: '0.2', selected: true, available: true },
      { jointName: 'joint_a', positionText: '-0.1', selected: true, available: true },
      { jointName: 'joint_b', positionText: '', selected: false, available: true },
    ]);

    const selected = toggleJointTargetRow(rows, 'joint_b', true, [
      'joint_a', 'joint_b', 'joint_c',
    ]);
    expect(selected.map((row) => [row.jointName, row.selected, row.positionText])).toEqual([
      ['joint_c', true, '0.2'],
      ['joint_a', true, '-0.1'],
      ['joint_b', true, ''],
    ]);
    expect(jointTargetTransitionMessage(jointTargetSchema, selected)).toBe(
      'Enter a target position for joint_b.',
    );
    expect(completeJointTargetTransition(jointTargetSchema, selected)).toBeUndefined();

    const completed = selected.map((row) => row.jointName === 'joint_b'
      ? { ...row, positionText: '-0.4' }
      : row);
    expect(completeJointTargetTransition(jointTargetSchema, completed)).toEqual([
      { joint_name: 'joint_c', position_rad: 0.2 },
      { joint_name: 'joint_a', position_rad: -0.1 },
      { joint_name: 'joint_b', position_rad: -0.4 },
    ]);

    const removed = toggleJointTargetRow(completed, 'joint_a', false, [
      'joint_a', 'joint_b', 'joint_c',
    ]);
    expect(completeJointTargetTransition(jointTargetSchema, removed)).toEqual([
      { joint_name: 'joint_c', position_rad: 0.2 },
      { joint_name: 'joint_b', position_rad: -0.4 },
    ]);
    expect(removed.at(-1)).toEqual({
      jointName: 'joint_a', positionText: '', selected: false, available: true,
    });
  });

  it('retains an unavailable authored joint visibly and rejects an empty goal', () => {
    expect(jointTargetEditorRows(
      ['joint_a'],
      [{ joint_name: 'legacy_joint', position_rad: 0.3 }],
    )).toEqual([
      { jointName: 'legacy_joint', positionText: '0.3', selected: true, available: false },
      { jointName: 'joint_a', positionText: '', selected: false, available: true },
    ]);
    expect(jointTargetTransitionMessage(jointTargetSchema, [
      { jointName: 'joint_a', positionText: '', selected: false, available: true },
    ])).toBe('Select at least 1 joint.');
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

  it('keeps open and closed as symbolic preset names and preserves numeric transitions', () => {
    expect(completeCoordinateKindTransition(namedCoordinateSchema, 'named_preset'))
      .toEqual({ kind: 'named_preset', name: 'open' });
    for (const name of ['open', 'closed']) {
      expect(completeCoordinateKindTransition(namedCoordinateSchema, 'named_preset', name))
        .toEqual({ kind: 'named_preset', name });
    }
    expect(completeCoordinateKindTransition(namedCoordinateSchema, 'angular_rad'))
      .toEqual({ kind: 'angular_rad', value_rad: 0 });
    expect(completeCoordinateKindTransition(namedCoordinateSchema, 'linear_m'))
      .toEqual({ kind: 'linear_m', value_m: 0 });
    expect(completeCoordinateKindTransition(namedCoordinateSchema, 'named_preset', 'vendor_open'))
      .toBeUndefined();
    expect(completeCoordinateKindTransition(coordinateSchema, 'named_preset'))
      .toBeUndefined();
  });

  it.each([
    { kind: 'named_preset' },
    { kind: 'named_preset', name: 'vendor_open' },
    { kind: 'named_preset', name: 'open', value_m: 0.01 },
    { kind: 'named_preset', name: 'closed', value_rad: 0.2 },
    { kind: 'linear_m', value_m: 0.01, name: 'open' },
  ])('rejects mixed or unsupported named-preset grammar: %j', (value) => {
    expect(firstCatalogSchemaValidationMessage(namedCoordinateSchema, value, 'position'))
      .not.toBeNull();
  });

  it.each(['open', 'closed'])('displays the stored %s preset without a numeric input', (name) => {
    const markup = renderToStaticMarkup(createElement(ProgramSpecializedEditor, {
      schema: namedCoordinateSchema, value: { kind: 'named_preset', name },
      disabled: false, onCommit: () => undefined,
    }));
    expect(markup).toContain('aria-label="Gripper preset"');
    expect(markup).toContain(`value="${name}" selected=""`);
    expect(markup).toContain('bound gripper when the node runs');
    expect(markup).not.toContain('type="number"');
  });
});
