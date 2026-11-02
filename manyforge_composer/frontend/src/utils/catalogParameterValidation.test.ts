import { describe, expect, it } from 'vitest';
import type { CatalogParameter } from '../types/api';
import { firstCatalogParameterValidationMessage } from './catalogParameterValidation';

function parameter(overrides: Partial<CatalogParameter> = {}): CatalogParameter {
  return {
    id: 'speed',
    required: true,
    schema: { type: 'number', exclusiveMinimum: 0 },
    schemaHash: 'sha256:test',
    hasDefault: false,
    ...overrides,
  };
}

describe('firstCatalogParameterValidationMessage', () => {
  it('enforces required values unless the catalog supplies a default', () => {
    expect(firstCatalogParameterValidationMessage(parameter(), undefined)).toBe(
      'speed is required',
    );
    expect(
      firstCatalogParameterValidationMessage(
        parameter({ hasDefault: true, default: 0.1 }),
        undefined,
      ),
    ).toBeNull();
  });

  it('uses the retained JSON Schema for present values', () => {
    expect(firstCatalogParameterValidationMessage(parameter(), 0.2)).toBeNull();
    expect(firstCatalogParameterValidationMessage(parameter(), 0)).toBe(
      'speed must be > 0',
    );
  });

  it('does not recreate old blackboard binding rules', () => {
    const binding = parameter({
      id: 'object_id',
      schema: {
        anyOf: [
          { type: 'string' },
          {
            type: 'object',
            required: ['key_ref'],
            properties: { key_ref: { type: 'string' } },
          },
        ],
      },
    });
    expect(
      firstCatalogParameterValidationMessage(binding, { key_ref: 'target_object' }),
    ).toBeNull();
  });
});
