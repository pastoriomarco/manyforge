import { describe, expect, it } from 'vitest';
import type { CatalogJsonSchema } from '../types/api';
import { validateCatalogJsonSchemaValue } from './catalogSchema';

const shapeSchema: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['type'],
  properties: {
    type: { type: 'string', enum: ['box', 'sphere'] },
    box_dimensions_m: {
      type: 'array',
      items: { type: 'number', exclusiveMinimum: 0 },
      minItems: 3,
      maxItems: 3,
    },
    sphere_radius_m: { type: 'number', exclusiveMinimum: 0 },
  },
  allOf: [
    {
      if: {
        required: ['type'],
        properties: { type: { const: 'box' } },
      },
      then: { required: ['box_dimensions_m'] },
    },
    {
      if: {
        required: ['type'],
        properties: { type: { const: 'sphere' } },
      },
      then: { required: ['sphere_radius_m'] },
    },
    {
      if: {
        not: {
          required: ['type'],
          properties: { type: { const: 'box' } },
        },
      },
      then: { not: { required: ['box_dimensions_m'] } },
    },
    {
      if: {
        not: {
          required: ['type'],
          properties: { type: { const: 'sphere' } },
        },
      },
      then: { not: { required: ['sphere_radius_m'] } },
    },
  ],
};

describe('validateCatalogJsonSchemaValue', () => {
  it('accepts valid conditional shape variants', () => {
    expect(
      validateCatalogJsonSchemaValue(
        shapeSchema,
        { type: 'box', box_dimensions_m: [0.1, 0.2, 0.3] },
        'shape',
      ),
    ).toEqual([]);
    expect(
      validateCatalogJsonSchemaValue(
        shapeSchema,
        { type: 'sphere', sphere_radius_m: 0.05 },
        'shape',
      ),
    ).toEqual([]);
  });

  it('rejects missing and stale conditional shape fields', () => {
    const missing = validateCatalogJsonSchemaValue(shapeSchema, { type: 'sphere' }, 'shape');
    expect(missing.map((issue) => issue.path)).toContain('shape.sphere_radius_m');

    const stale = validateCatalogJsonSchemaValue(
      shapeSchema,
      {
        type: 'sphere',
        sphere_radius_m: 0.05,
        box_dimensions_m: [0.1, 0.2, 0.3],
      },
      'shape',
    );
    expect(stale.map((issue) => issue.path)).toContain('shape.box_dimensions_m');
  });

  it('validates assistant-projected anyOf binding forms', () => {
    const schema: CatalogJsonSchema = {
      anyOf: [
        { type: 'string' },
        {
          type: 'object',
          additionalProperties: false,
          required: ['key_ref'],
          properties: { key_ref: { type: 'string' } },
        },
      ],
    };

    expect(validateCatalogJsonSchemaValue(schema, 'object_1', 'object_id')).toEqual([]);
    expect(validateCatalogJsonSchemaValue(schema, { key_ref: 'target_object' }, 'object_id')).toEqual([]);
    expect(validateCatalogJsonSchemaValue(schema, { field_ref: 'target_object' }, 'object_id')).toEqual([
      {
        path: 'object_id',
        message: 'object_id must match at least one allowed schema',
      },
    ]);
  });
});
