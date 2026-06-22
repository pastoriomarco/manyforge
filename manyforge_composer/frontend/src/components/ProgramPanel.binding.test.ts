import { describe, expect, it } from 'vitest';
import type { Node } from '@xyflow/react';

import type { CatalogParameter, ProgramParameter } from '../types/api';
import { collectBindCandidates } from './ProgramPanel';
import { resolveProgramParameterLiteralValue } from './property/ProgramParameterBindingEditor';

function makeParam(overrides: Partial<CatalogParameter>): CatalogParameter {
  return {
    name: 'x',
    type: 'float',
    required: false,
    default: undefined,
    enumValues: [],
    enumOptions: [],
    constraints: {},
    parameterizable: true,
    profilesMin: 'v0.0',
    description: '',
    ...overrides,
  } as CatalogParameter;
}

function makeNode(id: string, catalogId: string, params: Record<string, unknown>): Node {
  return {
    id,
    position: { x: 0, y: 0 },
    data: { catalogId, label: id, params },
  } as unknown as Node;
}

const catalogIndex = new Map<string, Map<string, CatalogParameter>>([
  [
    'move',
    new Map<string, CatalogParameter>([
      ['velocity_scale', makeParam({ name: 'velocity_scale', type: 'float', parameterizable: true })],
      [
        'max_tcp_linear_speed_mps',
        makeParam({ name: 'max_tcp_linear_speed_mps', type: 'float', parameterizable: true }),
      ],
      ['move_id', makeParam({ name: 'move_id', type: 'string', parameterizable: false })],
    ]),
  ],
]);

describe('collectBindCandidates', () => {
  it('offers parameterizable, type-matching, unbound fields', () => {
    const got = collectBindCandidates([makeNode('n1', 'move', {})], catalogIndex, 'float');
    expect(got.map((c) => c.fieldName).sort()).toEqual([
      'max_tcp_linear_speed_mps',
      'velocity_scale',
    ]);
  });

  it('excludes a field already bound to a program parameter', () => {
    const got = collectBindCandidates(
      [makeNode('n1', 'move', { velocity_scale: '${param:vscale}' })],
      catalogIndex,
      'float',
    );
    expect(got.map((c) => c.fieldName)).toEqual(['max_tcp_linear_speed_mps']);
  });

  it('excludes type-mismatched and non-parameterizable fields', () => {
    // move_id is the only string field but is not parameterizable.
    const got = collectBindCandidates([makeNode('n1', 'move', {})], catalogIndex, 'string');
    expect(got).toEqual([]);
  });
});

describe('resolveProgramParameterLiteralValue (unbind revert literal)', () => {
  const programParameters: ProgramParameter[] = [
    { name: 'vscale', type: 'float', default: 0.3, constraints: {}, description: '' },
  ];

  it('reverts a bound field to the bound program parameter default', () => {
    expect(resolveProgramParameterLiteralValue('${param:vscale}', 1.0, programParameters)).toBe(0.3);
  });

  it('falls back to the catalog default when the bound parameter is gone', () => {
    expect(resolveProgramParameterLiteralValue('${param:gone}', 1.0, programParameters)).toBe(1.0);
  });

  it('returns a literal value unchanged', () => {
    expect(resolveProgramParameterLiteralValue(0.5, 1.0, programParameters)).toBe(0.5);
  });
});
