import { describe, expect, it } from 'vitest';

import type {
  CatalogEntry,
  CatalogParameter,
  ProgramAuthoringDocument,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import {
  collectProgramBindingCandidates,
  collectProgramParameterUsages,
  collectComponentRoleUsages,
  deriveProgramMotionConfiguration,
  nextProgramInstanceId,
  programDiagnosticNodeId,
  programSemanticChoices,
  programSemanticExpectedAccess,
  programSemanticExpectedType,
  removeGuardTitle,
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

  it('reports every Draft reference that keeps a component role', () => {
    const grip: CatalogEntry = {
      ...entry,
      id: 'grip',
      parameters: [parameter('component_id', 'string', 'component_role')],
    };
    const program: ProgramAuthoringDocument = {
      api_version: 'manyforge.program.v2',
      program_id: 'role_usage',
      program_profile: 'composable-cell-v1',
      name: 'Role usage',
      catalog: {
        api_version: 'manyforge.node_catalog.v2',
        content_version: '2.4.0',
      },
      component_roles: { robot: {}, tool: {}, spare: {} },
      target_roles: { arm: { component_role: 'robot', required_capabilities: [] } },
      scene: [{
        scene_resource_id: 'table',
        shape: { type: 'box', box_dimensions_m: [1, 1, 0.1] },
        pose: {
          frame_ref: {
            kind: 'component_frame',
            component_role: 'tool',
            model_role: 'tool_model',
            frame_role: 'tcp',
          },
          position_m: [0, 0, 0],
          orientation: { xyzw: [0, 0, 0, 1] },
        },
      }],
      schema_version: 'manyforge.composer_authoring_forest.v1',
      forest: [
        tree({ node_id: 'grip', parameters: { component_id: 'robot' } }),
        tree({ node_id: 'grip', instance_id: 'n2', parameters: { component_id: 'robot' } }),
      ],
      positions: {},
      contact_policies: [{
        api_version: 'manyforge.contact_policy.v1',
        policy_id: 'grasp',
        mode: 'contact_grasp',
        zones: [{
          zone_id: 'pad',
          body_pairs: [{
            body_a: { kind: 'component_model_body', component_role_id: 'spare', collision_body_role_id: 'pad' },
            body_b: { kind: 'runtime_object', runtime_object_id: 'cube' },
          }],
        }],
      }],
    };

    const usages = collectComponentRoleUsages(program, [grip]);
    expect(usages.get('robot')).toEqual(['node n1', 'node n2', 'target role arm']);
    expect(usages.get('tool')).toEqual(['scene frame table']);
    // A contact policy still naming the role keeps it.
    expect(usages.get('spare')).toEqual(['contact policy grasp']);
    expect(usages.get('free')).toBeUndefined();
    expect(removeGuardTitle(usages.get('robot') ?? [], 'Remove component role'))
      .toBe('node n1, node n2, target role arm');
    expect(removeGuardTitle([], 'Remove component role')).toBe('Remove component role');
  });

  it('projects backend diagnostic paths onto retained node identities', () => {
    const root = tree({
      children: [{
        node_id: 'move',
        instance_id: 'child',
        parameters: {},
        children: [],
      }],
    });
    expect(programDiagnosticNodeId(
      [root],
      'forest[0].children[0].parameters.velocity_scale',
    )).toBe('child');
    expect(programDiagnosticNodeId([root], 'program.compatibility')).toBeNull();
  });

  it('derives compatible semantic choices from only the current Draft and declared scope', () => {
    const semanticEntry: CatalogEntry = {
      ...entry,
      parameters: [
        parameter('target_role', 'string', 'target_role'),
        {
          ...parameter('touch_body_roles', 'array'),
          schema: {
            type: 'array',
            items: {
              type: 'string',
              'x-manyforge-semantic-type': 'collision_body_role',
              'x-manyforge-semantic-scope-parameter': 'target_role',
            },
          },
        },
        parameter('output_key', 'string', 'blackboard_key'),
      ],
      blackboard: {
        reads: [],
        writes: [{ keyParameter: 'output_key', valueType: 'scene_selection_result' }],
      },
    };
    const selected = tree({ parameters: { target_role: 'gripper' } });
    const program: ProgramAuthoringDocument = {
      api_version: 'manyforge.program.v2',
      program_id: 'semantic_test',
      program_profile: 'composable-cell-v1',
      name: 'Semantic test',
      catalog: {
        api_version: 'manyforge.node_catalog.v2',
        content_version: '2.4.0',
      },
      component_roles: { robot: {}, tool: {} },
      target_roles: {
        arm: { required_collision_body_roles: ['arm_collision'] },
        gripper: { required_collision_body_roles: ['finger', 'gripper_collision'] },
      },
      blackboard: {
        namespaces: [],
        keys: [
          { key: 'selection', type: 'scene_selection_result', access: 'read_write', persistence: 'run' },
          { key: 'input_only', type: 'scene_selection_result', access: 'read', persistence: 'run' },
          { key: 'wrong_type', type: 'bool', access: 'read_write', persistence: 'run' },
        ],
      },
      schema_version: 'manyforge.composer_authoring_forest.v1',
      forest: [selected],
      positions: {},
    };

    expect(programSemanticChoices(
      semanticEntry.parameters[1]!.schema.items!,
      selected,
      semanticEntry,
      'touch_body_roles',
      program,
    ).map((choice) => choice.value)).toEqual(['finger', 'gripper_collision']);
    expect(programSemanticChoices(
      semanticEntry.parameters[2]!.schema,
      selected,
      semanticEntry,
      'output_key',
      program,
    ).map((choice) => choice.value)).toEqual(['selection']);
    expect(programSemanticExpectedType(
      semanticEntry,
      'output_key',
      'blackboard_key',
    )).toBe('scene_selection_result');
    expect(programSemanticExpectedAccess(semanticEntry, 'output_key')).toBe('write');
  });

  it('offers the attach node grasp-lost key as a writable bool key', () => {
    const attach: CatalogEntry = {
      ...entry,
      id: 'scene.attach_carried_object',
      parameters: [parameter('on_grasp_lost', 'string', 'blackboard_key')],
    };
    const selected = tree({
      node_id: 'scene.attach_carried_object',
      parameters: { on_grasp_lost: { raise_alarm: false, stop_motion: false, set_key: null, detach: true } },
    });
    const program: ProgramAuthoringDocument = {
      api_version: 'manyforge.program.v2',
      program_id: 'grasp_lost_test',
      program_profile: 'composable-cell-v1',
      name: 'Grasp lost test',
      catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '2.7.0' },
      component_roles: {},
      target_roles: {},
      blackboard: {
        namespaces: [],
        keys: [
          { key: 'computed_lost', type: 'bool', access: 'read', persistence: 'run', expression: 'true' },
          { key: 'count', type: 'int64', access: 'read_write', persistence: 'run' },
          { key: 'part_lost', type: 'bool', access: 'read_write', persistence: 'run' },
          { key: 'read_only', type: 'bool', access: 'read', persistence: 'run' },
          { key: 'sink', type: 'bool', access: 'write', persistence: 'run' },
        ],
      },
      schema_version: 'manyforge.composer_authoring_forest.v1',
      forest: [selected],
      positions: {},
    };
    expect(programSemanticChoices(
      attach.parameters[0]!.schema,
      selected,
      attach,
      'on_grasp_lost',
      program,
    ).map((choice) => choice.value)).toEqual(['part_lost', 'sink']);
    expect(programSemanticExpectedType(attach, 'on_grasp_lost', 'blackboard_key')).toBe('bool');
    expect(programSemanticExpectedAccess(attach, 'on_grasp_lost')).toBe('read_write');
  });

  it('derives motion settings from catalog-field bindings with arbitrary parameter names', () => {
    const motionEntry: CatalogEntry = {
      ...entry,
      parameters: [
        parameter('velocity_scale', 'number'),
        parameter('acceleration_scale', 'number'),
        parameter('translation_tol_m', 'number'),
        parameter('blend_radius_m', 'number'),
      ],
    };
    const program = {
      api_version: 'manyforge.program.v2',
      program_id: 'bound_motion',
      program_profile: 'composable-cell-v1',
      name: 'Bound motion',
      catalog: {
        api_version: 'manyforge.node_catalog.v2',
        content_version: '2.4.0',
      },
      component_roles: {},
      target_roles: {},
      parameters: [
        { ...declaration, parameter_id: 'cycle_velocity', default: 0.35 },
        { ...declaration, parameter_id: 'cycle_acceleration', default: 0.7 },
        { ...declaration, parameter_id: 'path_tolerance', default: 0.005 },
        { ...declaration, parameter_id: 'path_blend', default: 0.01 },
      ],
      schema_version: 'manyforge.composer_authoring_forest.v1',
      forest: [tree({
        value_sources: {
          velocity_scale: {
            kind: 'program_parameter',
            reference: 'cycle_velocity',
          },
          acceleration_scale: {
            kind: 'program_parameter',
            reference: 'cycle_acceleration',
          },
          translation_tol_m: {
            kind: 'program_parameter',
            reference: 'path_tolerance',
          },
          blend_radius_m: {
            kind: 'program_parameter',
            reference: 'path_blend',
          },
        },
      })],
      positions: {},
    } satisfies ProgramAuthoringDocument;

    expect(deriveProgramMotionConfiguration(program, [motionEntry])).toEqual({
      available: true,
      configuration: {
        bindings: {
          velocity_scale: 'cycle_velocity',
          acceleration_scale: 'cycle_acceleration',
          translation_tol_m: 'path_tolerance',
          blend_radius_m: 'path_blend',
        },
        values: {
          velocity_scale: 0.35,
          acceleration_scale: 0.7,
          translation_tol_m: 0.005,
          blend_radius_m: 0.01,
        },
        usageCounts: {
          velocity_scale: 1,
          acceleration_scale: 1,
          translation_tol_m: 1,
          blend_radius_m: 1,
        },
        applicableCounts: {
          velocity_scale: 1,
          acceleration_scale: 1,
          translation_tol_m: 1,
          blend_radius_m: 1,
        },
      },
    });
  });

  it('reports a missing binding instead of inventing motion defaults', () => {
    const motionEntry: CatalogEntry = {
      ...entry,
      parameters: [
        parameter('velocity_scale', 'number'),
        parameter('acceleration_scale', 'number'),
        parameter('max_tcp_linear_speed_mps', 'number'),
        parameter('translation_tol_m', 'number'),
        parameter('blend_radius_m', 'number'),
      ],
    };
    const program = {
      api_version: 'manyforge.program.v2',
      program_id: 'incomplete_motion',
      program_profile: 'composable-cell-v1',
      name: 'Incomplete motion',
      catalog: {
        api_version: 'manyforge.node_catalog.v2',
        content_version: '2.4.0',
      },
      component_roles: {},
      target_roles: {},
      parameters: [declaration],
      schema_version: 'manyforge.composer_authoring_forest.v1',
      forest: [tree({
        value_sources: {
          velocity_scale: { kind: 'program_parameter', reference: 'vscale' },
        },
      })],
      positions: {},
    } satisfies ProgramAuthoringDocument;

    expect(deriveProgramMotionConfiguration(program, [motionEntry])).toEqual({
      available: false,
      reason: 'No acceleration_scale field is bound to a Program parameter.',
    });
  });

  it('retains a declared Cartesian default when this Program has no Cartesian node', () => {
    const motionEntry: CatalogEntry = {
      ...entry,
      parameters: [
        parameter('velocity_scale', 'number'),
        parameter('acceleration_scale', 'number'),
        parameter('max_tcp_linear_speed_mps', 'number'),
        parameter('translation_tol_m', 'number'),
      ],
    };
    const fields = {
      velocity_scale: 0.35,
      acceleration_scale: 0.7,
      max_tcp_linear_speed_mps: 0.12,
      translation_tol_m: 0.005,
    };
    const program = {
      api_version: 'manyforge.program.v2',
      program_id: 'joint_only_motion',
      program_profile: 'composable-cell-v1',
      name: 'Joint-only motion',
      catalog: {
        api_version: 'manyforge.node_catalog.v2',
        content_version: '2.4.0',
      },
      component_roles: {},
      target_roles: {},
      parameters: [
        ...Object.entries(fields).map(([parameter_id, defaultValue]) => ({
          ...declaration, parameter_id, default: defaultValue,
        })),
        { ...declaration, parameter_id: 'blend_radius_m', default: 0.01 },
      ],
      schema_version: 'manyforge.composer_authoring_forest.v1',
      forest: [tree({
        value_sources: Object.fromEntries(Object.keys(fields).map((field) => [field, {
          kind: 'program_parameter' as const,
          reference: field,
        }])),
      })],
      positions: {},
    } satisfies ProgramAuthoringDocument;

    const result = deriveProgramMotionConfiguration(program, [motionEntry]);
    expect(result.available).toBe(true);
    if (!result.available) return;
    expect(result.configuration.bindings.blend_radius_m)
      .toBe('blend_radius_m');
    expect(result.configuration.values.blend_radius_m).toBe(0.01);
    expect(result.configuration.applicableCounts.blend_radius_m).toBe(0);
  });
});
