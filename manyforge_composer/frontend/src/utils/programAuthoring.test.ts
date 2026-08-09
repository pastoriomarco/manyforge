import { describe, expect, it } from 'vitest';

import type {
  CatalogEntry,
  CatalogParameter,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import {
  collectProgramBindingCandidates,
  collectProgramParameterUsages,
  nextProgramInstanceId,
  resolveProgramParameterLiteralValue,
} from './programAuthoring';

function parameter(
  id: string,
  type: string,
  semanticType?: string,
): CatalogParameter {
  return {
    id,
    required: false,
    schema: {
      type,
      ...(semanticType ? { 'x-manyforge-semantic-type': semanticType } : {}),
    },
    schemaHash: `sha256:${id}`,
    hasDefault: false,
  };
}

const entry: CatalogEntry = {
  id: 'move',
  name: 'Move',
  category: 'motion',
  description: '',
  stability: 'stable',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [
    parameter('velocity_scale', 'number'),
    parameter('max_tcp_linear_speed_mps', 'number'),
    parameter('move_id', 'string', 'target_role'),
  ],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'kernel.operation' },
  runners: { py_trees: 'move_v1' },
  tags: [],
  minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: 'manyforge.program.node',
};

const declaration: ProgramParameterDeclaration = {
  parameter_id: 'vscale',
  type: 'float64',
  default: 0.3,
  runtime_mutability: 'before_run',
};

function tree(overrides: Partial<ProgramTreeNode> = {}): ProgramTreeNode {
  return {
    node_id: 'move',
    instance_id: 'n1',
    parameters: {},
    children: [],
    ...overrides,
  };
}

describe('Program parameter binding continuity', () => {
  it('offers type-compatible, dynamically safe, unbound fields', () => {
    const got = collectProgramBindingCandidates(tree(), [entry], declaration);
    expect(got.map((candidate) => candidate.fieldName)).toEqual([
      'max_tcp_linear_speed_mps',
      'velocity_scale',
    ]);
  });

  it('excludes a field already bound through exact value_sources', () => {
    const got = collectProgramBindingCandidates(tree({
      value_sources: {
        velocity_scale: { kind: 'program_parameter', reference: 'vscale' },
      },
    }), [entry], declaration);
    expect(got.map((candidate) => candidate.fieldName)).toEqual([
      'max_tcp_linear_speed_mps',
    ]);
  });

  it('excludes type-mismatched and semantic selector fields', () => {
    const stringDeclaration = { ...declaration, type: 'string' };
    expect(collectProgramBindingCandidates(tree(), [entry], stringDeclaration)).toEqual([]);
  });

  it('retains the v1 declared-scene-resource value-source exception', () => {
    const sceneEntry = {
      ...entry,
      parameters: [parameter('scene_resource_id', 'string', 'scene_resource')],
    };
    const sceneDeclaration = {
      ...declaration,
      parameter_id: 'selected_object',
      type: 'scene_resource',
      default: 'workpiece',
    };

    expect(collectProgramBindingCandidates(
      tree(),
      [sceneEntry],
      sceneDeclaration,
    )).toEqual([{
      nodeId: 'n1',
      nodeLabel: 'n1',
      fieldName: 'scene_resource_id',
    }]);
  });

  it('offers stored poses only to an exact pose-shaped object destination', () => {
    const objectEntry = {
      ...entry,
      parameters: [{
        ...parameter('payload', 'object'),
        schema: {
          type: 'object',
          properties: {
            position_m: { type: 'array' },
            orientation: { type: 'object' },
          },
        },
      }, parameter('other_object', 'object')],
    };
    const poseDeclaration = {
      ...declaration,
      parameter_id: 'home',
      type: 'stored_pose',
      default: {
        frame_ref: { kind: 'target_frame', target_role_id: 'arm', frame_role: 'tcp' },
        position_m: [0, 0, 0],
        orientation: { xyzw: [0, 0, 0, 1] },
      },
    };

    expect(collectProgramBindingCandidates(
      tree(),
      [objectEntry],
      poseDeclaration,
    ).map((candidate) => candidate.fieldName)).toEqual(['payload']);
  });

  it('reverts a bound field to the Program parameter default', () => {
    expect(resolveProgramParameterLiteralValue(
      { kind: 'program_parameter', reference: 'vscale' },
      undefined,
      1,
      [declaration],
    )).toBe(0.3);
  });

  it('falls back to the catalog default when the declaration is gone', () => {
    expect(resolveProgramParameterLiteralValue(
      { kind: 'program_parameter', reference: 'gone' },
      undefined,
      1,
      [declaration],
    )).toBe(1);
  });

  it('keeps an existing literal unchanged', () => {
    expect(resolveProgramParameterLiteralValue(undefined, 0.5, 1, [declaration])).toBe(0.5);
  });

  it('reports exact value_sources usages and deterministic suffixes', () => {
    const root = tree({
      value_sources: {
        velocity_scale: { kind: 'program_parameter', reference: 'vscale' },
      },
    });
    expect(collectProgramParameterUsages(root).vscale).toEqual([{
      nodeId: 'n1',
      nodeLabel: 'n1',
      fieldName: 'velocity_scale',
    }]);
    expect(nextProgramInstanceId('motion.move', ['motion.move', 'motion.move_2'])).toBe(
      'motion.move_3',
    );
  });
});
