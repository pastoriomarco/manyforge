import { describe, expect, it } from 'vitest';
import type {
  BlackboardKey,
  BlackboardNamespace,
  CatalogJsonSchema,
  CatalogParameter,
} from '../types/api';
import { firstCatalogParameterValidationMessage } from './catalogParameterValidation';

function makeParam(overrides: Partial<CatalogParameter>): CatalogParameter {
  return {
    name: 'value',
    type: 'string',
    required: true,
    default: undefined,
    enumValues: [],
    enumOptions: [],
    constraints: {},
    parameterizable: false,
    profilesMin: 'v0.0',
    description: '',
    schema: { type: 'string' },
    ...overrides,
  } as CatalogParameter;
}

const poseSchema: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['frame_id', 'position_m'],
  properties: {
    frame_id: { type: 'string' },
    position_m: {
      type: 'array',
      minItems: 3,
      maxItems: 3,
      items: { type: 'number' },
    },
  },
};

const namespaces: BlackboardNamespace[] = [
  {
    id: 'pick_plan',
    contract: 'pick_plan_v1',
    prefix: 'planning/pick_plan',
    description: '',
    fields: [
      { name: 'selected_object_id', type: 'string', description: '' },
      { name: 'approach_pose', type: 'pose', description: '' },
    ],
  },
];

const keys: BlackboardKey[] = [
  {
    id: 'target_object',
    type: 'string',
    key: 'runtime/target_object',
    description: '',
  },
];

describe('firstCatalogParameterValidationMessage', () => {
  it('accepts namespace_ref values for namespace writer parameters', () => {
    const param = makeParam({
      name: 'output_key_prefix',
      blackboardRole: 'namespace_writer',
      blackboardContract: 'pick_plan_v1',
    });

    expect(
      firstCatalogParameterValidationMessage(
        param,
        { namespace_ref: 'pick_plan' },
        { namespaces, keys },
      ),
    ).toBeNull();
  });

  it('rejects namespace refs with the wrong contract', () => {
    const param = makeParam({
      name: 'output_key_prefix',
      blackboardRole: 'namespace_writer',
      blackboardContract: 'query_model_output_v1',
    });

    expect(
      firstCatalogParameterValidationMessage(
        param,
        { namespace_ref: 'pick_plan' },
        { namespaces, keys },
      ),
    ).toBe(
      "output_key_prefix namespace_ref 'pick_plan' is declared as contract 'pick_plan_v1', expected 'query_model_output_v1'",
    );
  });

  it('accepts pose field_ref values instead of validating them as inline poses', () => {
    const param = makeParam({
      name: 'pose_goal',
      type: 'dict',
      schema: poseSchema,
      blackboardRole: 'field_reader',
      blackboardValueType: 'pose',
    });

    expect(
      firstCatalogParameterValidationMessage(
        param,
        { field_ref: { namespace: 'pick_plan', field: 'approach_pose' } },
        { namespaces, keys },
      ),
    ).toBeNull();
  });

  it('still validates inline pose values through the catalog schema', () => {
    const param = makeParam({
      name: 'pose_goal',
      type: 'dict',
      schema: poseSchema,
      blackboardRole: 'field_reader',
      blackboardValueType: 'pose',
    });

    expect(
      firstCatalogParameterValidationMessage(param, { position_m: [0, 0, 0] }, { namespaces, keys }),
    ).toBe('pose_goal.frame_id is required');
  });

  it('accepts string field_ref values for literal-or-key reader parameters', () => {
    const param = makeParam({
      name: 'object_id',
      blackboardRole: 'literal_or_key_reader',
      blackboardValueType: 'string',
    });

    expect(
      firstCatalogParameterValidationMessage(
        param,
        { field_ref: { namespace: 'pick_plan', field: 'selected_object_id' } },
        { namespaces, keys },
      ),
    ).toBeNull();
  });

  it('accepts key_ref values for literal-or-key reader parameters', () => {
    const param = makeParam({
      name: 'object_id',
      blackboardRole: 'literal_or_key_reader',
      blackboardValueType: 'string',
    });

    expect(
      firstCatalogParameterValidationMessage(param, { key_ref: 'target_object' }, { namespaces, keys }),
    ).toBeNull();
  });

  it('requires key writer parameters to use declared key_ref bindings', () => {
    const param = makeParam({
      name: 'output_key',
      blackboardRole: 'key_writer',
      blackboardValueType: 'string',
    });

    expect(
      firstCatalogParameterValidationMessage(param, 'runtime/target_object', { namespaces, keys }),
    ).toBe('output_key must be a key_ref binding');
  });

  it('requires key reader parameters to use declared key_ref or field_ref bindings', () => {
    const param = makeParam({
      name: 'key',
      blackboardRole: 'key_reader',
      blackboardValueType: 'string',
    });

    expect(
      firstCatalogParameterValidationMessage(param, { key_ref: 'target_object' }, { namespaces, keys }),
    ).toBeNull();
    expect(
      firstCatalogParameterValidationMessage(param, 'runtime/target_object', { namespaces, keys }),
    ).toBe('key must be a field_ref binding');
  });

  it('rejects field refs with the wrong declared type', () => {
    const param = makeParam({
      name: 'object_id',
      blackboardRole: 'literal_or_key_reader',
      blackboardValueType: 'string',
    });

    expect(
      firstCatalogParameterValidationMessage(
        param,
        { field_ref: { namespace: 'pick_plan', field: 'approach_pose' } },
        { namespaces, keys },
      ),
    ).toBe(
      "object_id field_ref 'pick_plan.approach_pose' is declared as type 'pose', expected 'string'",
    );
  });
});
