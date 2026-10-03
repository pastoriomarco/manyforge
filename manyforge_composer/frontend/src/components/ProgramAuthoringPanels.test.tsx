import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import nativeBindingFixture from '../../../../manyforge_deployment/resources/program_binding_lite6_native.fixture.json';
import { parseProgramBindingTransport } from '../api/programBindingTransport.generated';
import { statusFixture } from '../testFixtures/robotControl';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  BlackboardKeyDeclaration,
  DeploymentWorkspaceResponse,
  CatalogEntry,
  NodeEnvironmentResponse,
  ProgramAuthoringDocument,
  ProgramParameterDeclaration,
  ProgramTreeNode,
  ProgramWorkspaceResponse,
} from '../types/api';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import { ProgramContactPolicyEditor } from './ProgramContactPolicyEditor';
import { ProgramDocumentControls } from './ProgramDocumentControls';
import { ScenarioDocumentCommands } from './ScenarioDocumentCommands';
import { NodePaletteItem } from './NodePaletteItem';
import {
  ProgramNodePalette,
  defaultCollapsedCatalogCategories,
} from './ProgramNodePalette';
import {
  buildProgramParameterMutation,
  ProgramPropertyEditor,
  typeValuesTransition,
} from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';
import { ProgramWorkspace } from './ProgramWorkspace';
import { ProgramSchemaValueEditor } from './property/ProgramSchemaValueEditor';

const sequence: CatalogEntry = {
  id: 'sequence',
  name: 'Sequence',
  category: 'control_flow',
  description: 'Run children in order.',
  stability: 'stable',
  kind: 'composite',
  childrenMin: 1,
  childrenMax: -1,
  parameters: [],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'program' },
  runners: { py_trees: 'sequence_v1' },
  tags: ['flow'],
  minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const timer: CatalogEntry = {
  ...sequence,
  id: 'timer',
  name: 'Timer',
  category: 'logic',
  description: 'Wait for a duration.',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [{
    id: 'duration_s',
    required: true,
    schema: { type: 'number', minimum: 0 },
    schemaHash: 'sha256:duration',
    hasDefault: false,
  }],
  tags: ['time'],
};

const parameter: ProgramParameterDeclaration = {
  parameter_id: 'wait_time',
  type: 'float64',
  default: 0.5,
};

const tree: ProgramTreeNode = {
  node_id: 'sequence',
  instance_id: 'root_sequence',
  parameters: {},
  children: [{
    node_id: 'timer',
    instance_id: 'settle',
    parameters: {},
    value_sources: {
      duration_s: { kind: 'program_parameter', reference: 'wait_time' },
    },
    children: [],
  }],
};

const environment: NodeEnvironmentResponse = {
  environmentDigest: 'sha256:environment',
  implementations: [sequence, timer].map((entry) => ({
    nodeId: entry.id,
    engineId: 'py_trees',
    implementationId: `${entry.id}_v1`,
    executablePackageId: 'manyforge_behavior',
    authoringAvailable: true,
    executionAvailable: false,
    reasonCodes: ['native_program_product_unavailable'],
    restorationGates: ['N7'],
  })),
  portableAuthoringAvailable: true,
  executionAvailable: false,
  reasonCodes: ['native_program_product_unavailable'],
  restorationGates: ['N7'],
  active: null,
};

function authoringProgram(
  roots: ProgramTreeNode[],
  parameters: ProgramParameterDeclaration[] = [],
  keys: BlackboardKeyDeclaration[] = [],
): ProgramAuthoringDocument {
  return {
    api_version: 'manyforge.program.v2',
    program_id: 'test_program',
    program_profile: 'composable-cell-v1',
    name: 'Test Program',
    catalog: {
      api_version: 'manyforge.node_catalog.v2',
      content_version: '2.4.0',
    },
    component_roles: { robot: {} },
    target_roles: {},
    parameters,
    blackboard: { namespaces: [], keys },
    schema_version: 'manyforge.composer_authoring_forest.v1',
    forest: roots,
    positions: {},
  };
}

describe('Program authoring panels', () => {
  const mutationNode: ProgramTreeNode = {
    node_id: 'specialized-node',
    instance_id: 'specialized-instance',
    parameters: {
      pose: {
        position_m: [0, 0, 0],
        orientation: { rpy_deg: [0, 0, 0] },
      },
      coordinate: { kind: 'angular_rad', value_rad: 0.1 },
      retained_literal: 7,
    },
    value_sources: {
      program_bound: { kind: 'program_parameter', reference: 'speed' },
      blackboard_bound: { kind: 'blackboard_key', reference: 'target' },
    },
    children: [],
  };

  it('builds exact full-replacement specialized motion and gripper literal mutations', () => {
    const pose = {
      position_m: [0.2, 0.3, 0.4],
      orientation: { xyzw: [0, 0, 0, 1] },
    };
    expect(buildProgramParameterMutation(
      mutationNode,
      'pose',
      { kind: 'literal', value: pose },
    )).toEqual({
      mutation: 'tree_draft_update_node_params',
      args: {
        instance_id: 'specialized-instance',
        expected_node_id: 'specialized-node',
        merge: false,
        parameters: {
          ...mutationNode.parameters,
          pose,
        },
        value_sources: mutationNode.value_sources,
      },
    });

    const coordinate = { kind: 'linear_m', value_m: 0.025 };
    expect(buildProgramParameterMutation(
      mutationNode,
      'coordinate',
      { kind: 'literal', value: coordinate },
    ).args).toEqual({
      instance_id: 'specialized-instance',
      expected_node_id: 'specialized-node',
      merge: false,
      parameters: {
        ...mutationNode.parameters,
        coordinate,
      },
      value_sources: mutationNode.value_sources,
    });
  });

  it('binds either source kind, keeping the literal to restore and unrelated sources', () => {
    expect(buildProgramParameterMutation(
      mutationNode,
      'pose',
      {
        kind: 'source',
        source: { kind: 'program_parameter', reference: 'target_pose' },
      },
    ).args).toEqual({
      instance_id: 'specialized-instance',
      expected_node_id: 'specialized-node',
      merge: false,
      parameters: mutationNode.parameters,
      value_sources: {
        ...mutationNode.value_sources,
        pose: { kind: 'program_parameter', reference: 'target_pose' },
      },
    });
    expect(buildProgramParameterMutation(
      mutationNode,
      'coordinate',
      {
        kind: 'source',
        source: { kind: 'blackboard_field', reference: 'tool', field: 'opening' },
      },
    ).args).toEqual({
      instance_id: 'specialized-instance',
      expected_node_id: 'specialized-node',
      merge: false,
      parameters: mutationNode.parameters,
      value_sources: {
        ...mutationNode.value_sources,
        coordinate: { kind: 'blackboard_field', reference: 'tool', field: 'opening' },
      },
    });
  });

  it('returns one bound field to literal and clears only one optional counterpart', () => {
    expect(buildProgramParameterMutation(
      mutationNode,
      'program_bound',
      { kind: 'literal', value: 0.75 },
    ).args).toEqual({
      instance_id: 'specialized-instance',
      expected_node_id: 'specialized-node',
      merge: false,
      parameters: {
        ...mutationNode.parameters,
        program_bound: 0.75,
      },
      value_sources: {
        blackboard_bound: { kind: 'blackboard_key', reference: 'target' },
      },
    });
    expect(buildProgramParameterMutation(
      mutationNode,
      'blackboard_bound',
      { kind: 'clear' },
    ).args).toEqual({
      instance_id: 'specialized-instance',
      expected_node_id: 'specialized-node',
      merge: false,
      parameters: mutationNode.parameters,
      value_sources: {
        program_bound: { kind: 'program_parameter', reference: 'speed' },
      },
    });
  });

  it('keeps catalog authoring available while execution is separately gated', () => {
    const markup = renderToStaticMarkup(
      <ProgramNodePalette
        roots={[tree]}
        catalogEntries={[sequence, timer]}
        catalogIdentity={null}
        environment={environment}
        busy={false}
        onMutate={vi.fn()}
      />,
    );

    expect(defaultCollapsedCatalogCategories([sequence, timer], environment))
      .toEqual(new Set(['control_flow', 'logic']));
    expect(markup).toContain('aria-label="Expand Control Flow nodes"');
    expect(markup).not.toContain('node-palette-row');

    const unavailableItem = renderToStaticMarkup(
      <NodePaletteItem
        entry={sequence}
        executionAvailable={false}
        executionNote="Authorable · unavailable until N7"
        allowDrag
      />,
    );
    expect(unavailableItem).not.toContain('Authorable · unavailable until N7');
    expect(unavailableItem).toContain('node-info-button execution-unavailable');
    expect(unavailableItem).toContain('execution unavailable');
    expect(unavailableItem).toContain('draggable="true"');
    expect(markup).toContain('<strong>Control Flow</strong>');
    expect(unavailableItem).toContain('Sequence; double-click or drag to add');
    expect(unavailableItem).not.toContain('sequence · composite');
    expect(unavailableItem).not.toContain('Authoring is currently unavailable');
  });

  it('does not build parameter details while palette information is closed', () => {
    const readParameters = vi.fn(() => sequence.parameters);
    const entry = { ...sequence, get parameters() { return readParameters(); } };
    const markup = renderToStaticMarkup(<NodePaletteItem entry={entry} />);

    expect(markup).toContain('Information about Sequence');
    expect(readParameters).not.toHaveBeenCalled();
  });

  it('marks executable catalog information without an inline status sentence', () => {
    const executableEnvironment: NodeEnvironmentResponse = {
      ...environment,
      executionAvailable: true,
      reasonCodes: [],
      restorationGates: [],
      implementations: environment.implementations.map((row) => ({
        ...row,
        executionAvailable: true,
        reasonCodes: [],
        restorationGates: [],
      })),
    };
    const markup = renderToStaticMarkup(
      <ProgramNodePalette
        roots={[tree]}
        catalogEntries={[sequence, timer]}
        catalogIdentity={null}
        environment={executableEnvironment}
        busy={false}
        onMutate={vi.fn()}
      />,
    );

    expect(defaultCollapsedCatalogCategories([
      sequence,
      { ...timer, category: sequence.category },
    ], {
      ...executableEnvironment,
      implementations: executableEnvironment.implementations.map((row) => (
        row.nodeId === timer.id ? { ...row, executionAvailable: false } : row
      )),
    })).toEqual(new Set());
    expect(markup).toContain('node-info-button execution-available');
    expect(markup).toContain('execution available');
    expect(markup).not.toContain('Execution available for the current binding');
    expect(markup).not.toContain('Executable when configured');
  });

  it('exposes templates and contact policy authoring as Draft-only surfaces', () => {
    const program: ProgramAuthoringDocument = {
      ...authoringProgram([tree], [], [{
        key: 'selected_object',
        type: 'scene_resource',
        access: 'read_write',
        persistence: 'run',
      }]),
      scene: [{
        scene_resource_id: 'part',
        shape: { type: 'box', box_dimensions_m: [0.1, 0.1, 0.1] },
        pose: {
          frame_ref: { kind: 'cell_frame' },
          position_m: [0, 0, 0],
          orientation: { xyzw: [0, 0, 0, 1] },
        },
      }],
      target_roles: {
        arm: {
          component_role: 'robot',
          required_capabilities: [{
            capability_id: 'planning.motion.pose_goal',
            parameters: {},
            disposition: 'required',
          }],
          required_frame_roles: ['base', 'tcp'],
          required_collision_body_roles: ['arm_collision'],
        },
        gripper: {
          component_role: 'tool',
          required_capabilities: [{
            capability_id: 'tool.gripper.command',
            parameters: {},
            disposition: 'required',
          }],
          required_frame_roles: ['tcp'],
          required_collision_body_roles: ['gripper_collision'],
        },
      },
      contact_policies: [],
    };
    const markup = renderToStaticMarkup(<div>
      <ProgramNodePalette
        roots={[tree]}
        program={program}
        catalogEntries={[sequence, timer]}
        catalogIdentity={null}
        environment={environment}
        busy={false}
        onMutate={vi.fn()}
      />
      <ProgramContactPolicyEditor
        program={program}
        draftRevision={1}
        busy={false}
        mutate={vi.fn() as ProgramWorkspaceController['mutate']}
      />
    </div>);

    expect(markup).toContain('Pick subtree');
    expect(markup).toContain('Place subtree');
    expect(markup).toContain('Contact policies');
    expect(markup).toContain('Object support contacts');
    expect(markup).toContain('Runtime object key');
  });

  it('renders one deterministic selected tree and exact v1 value source', () => {
    const markup = renderToStaticMarkup(
      <div>
        <ProgramTreeCanvas
          roots={[tree]}
          retainedPositions={{}}
          catalogEntries={[sequence, timer]}
          environment={environment}
          selectedId="settle"
          invalidNodeIds={new Set()}
          busy={false}
          onSelect={vi.fn()}
          onMutate={vi.fn()}
        />
        <ProgramPropertyEditor
          roots={[tree]}
          selectedId="settle"
          catalogEntries={[sequence, timer]}
          program={authoringProgram([tree], [parameter])}
          busy={false}
          onMutate={vi.fn()}
          onUpdateNodePolicies={vi.fn()}
        />
      </div>,
    );

    expect(markup).toContain('aria-pressed="true"');
    // A bound number shows its source in the field's own box; no separate dropdown.
    expect(markup).toMatch(/<span class="program-value-field sourced"><input readOnly=""[^>]*value="param.wait_time"/);
    expect(markup).toContain('title="param.wait_time = 0.5 (parameter default)"');
    expect(markup).not.toContain('Value source');
  });

  it('offers the current robot pose or joints for a move goal', () => {
    const poseEntry: CatalogEntry = { ...timer, id: 'motion.move_to_pose', parameters: [
      { id: 'target_role', required: true, schema: { type: 'string' },
        schemaHash: 'sha256:role', hasDefault: false },
      { id: 'goal', required: true, schema: { type: 'object', properties: {
        position_m: { type: 'array' }, orientation: { type: 'object' } } },
        schemaHash: 'sha256:goal', hasDefault: false },
    ] };
    const render = (parameters: ProgramTreeNode['parameters']) => renderToStaticMarkup(
      <ProgramPropertyEditor
        roots={[{ node_id: 'motion.move_to_pose', instance_id: 'move', children: [], parameters }]}
        selectedId="move" catalogEntries={[poseEntry]}
        program={authoringProgram([], [])}
        busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()}
      />,
    );
    const goal = { frame_ref: { kind: 'cell_frame' }, position_m: [0, 0, 0],
      orientation: { xyzw: [0, 0, 0, 1] } };
    const bound = render({ target_role: 'lite6_arm', target_mode: 'relative', goal });
    expect(bound).toContain('Use current robot pose');
    expect(bound).toContain('This switches the move to absolute.');
    const unbound = render({ goal });
    expect(unbound).toMatch(/disabled=""[^>]*title="Choose the target role first\."/);
  });

  it('explains backend TCP capping without rewriting literals or parameter references', () => {
    const binding = parseProgramBindingTransport(nativeBindingFixture, 'binding');
    const target = binding.target_role_bindings.find((row) => row.target_role_id === 'arm')!;
    const descriptor = { ...statusFixture().directControl!.targetDescriptor,
      controllableComponentId: target.controllable_component_id,
      commandTargetId: target.command_target_id,
      operationalLimit: { maximumTcpLinearSpeedMps: 0.15 },
    };
    const deployment: DeploymentWorkspaceResponse = {
      schemaVersion: 'manyforge.composer_deployment_workspace.v1',
      authoringWorkspace: { workspaceEpoch: 'workspace', workspaceRevision: 1,
        programDraftId: 'program', programDraftRevision: 1,
        deploymentDraftId: 'deployment', deploymentDraftRevision: 1 },
      draft: { id: 'deployment', revision: 1, document: null,
        deploymentArtifactId: null, deploymentRevision: null, graphRevision: null,
        differsFromSaved: false },
      saved: { path: null, deploymentRevision: null },
      managerApplied: { deploymentRevision: null, scenarioId: null },
      runAdmission: { ready: false, reason: 'not_applied' },
      binding, scenarioCandidates: [], savedScenarioCandidates: [], unavailableReasons: [],
    };
    const catalog: CatalogEntry = { ...timer, parameters: [{
      id: 'max_tcp_linear_speed_mps', required: false,
      schema: { type: 'number', exclusiveMinimum: 0 },
      schemaHash: 'sha256:tcp-speed', hasDefault: false,
    }] };
    for (const referenced of [false, true]) {
      const node: ProgramTreeNode = {
        node_id: 'timer', instance_id: 'motion', children: [],
        parameters: referenced ? { target_role: 'arm' }
          : { target_role: 'arm', max_tcp_linear_speed_mps: 0.25 },
        ...(referenced ? { value_sources: {
          max_tcp_linear_speed_mps: { kind: 'program_parameter' as const, reference: 'speed' },
        } } : {}),
      };
      const before = JSON.stringify(node);
      const markup = renderToStaticMarkup(<ProgramPropertyEditor
        roots={[node]} selectedId="motion" catalogEntries={[catalog]}
        deploymentWorkspace={deployment} motionDescriptors={[descriptor]}
        program={authoringProgram([node], [{ ...parameter, parameter_id: 'speed', default: 0.25 }])}
        busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()}
      />);
      expect(markup).toContain('TCP speed ceiling');
      expect(markup).toContain('effective ceiling is 0.15 m/s');
      expect(markup).toContain(referenced ? 'parameter default' : 'With request');
      expect(markup).not.toContain('exceeds Deployment');
      expect(JSON.stringify(node)).toBe(before);
      if (referenced) expect(markup).toContain('value="param.speed"');
    }
  });

  it('keeps schema-invalid motion and gripper variants losslessly in JSON repair mode', () => {
    const variantSchemas: CatalogEntry = {
      ...timer,
      id: 'variant-node',
      parameters: [
        {
          id: 'pose',
          required: true,
          schema: {
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
                  rpy_deg: { type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } },
                  xyzw: { type: 'array', minItems: 4, maxItems: 4, items: { type: 'number' } },
                },
                allOf: [{ oneOf: [{ required: ['rpy_deg'] }, { required: ['xyzw'] }] }],
              },
            },
          },
          schemaHash: 'sha256:pose',
          hasDefault: false,
        },
        {
          id: 'position',
          required: true,
          schema: {
            type: 'object',
            additionalProperties: false,
            required: ['kind'],
            properties: {
              kind: { type: 'string', enum: ['angular_rad', 'linear_m'] },
              value_rad: { type: 'number' },
              value_m: { type: 'number' },
            },
            allOf: [{
              oneOf: [{ required: ['value_rad'] }, { required: ['value_m'] }],
            }],
          },
          schemaHash: 'sha256:position',
          hasDefault: false,
        },
      ],
    };
    const invalidTree: ProgramTreeNode = {
      node_id: 'variant-node',
      instance_id: 'invalid',
      parameters: {
        pose: {
          position_m: [0, 0, 0],
          orientation: {
            rpy_deg: [0, 0, 0],
            xyzw: [0, 0, 0, 1],
          },
        },
        position: {
          kind: 'angular_rad',
          value_rad: 0.2,
          value_m: 0.01,
        },
      },
      children: [],
    };

    const markup = renderToStaticMarkup(
      <ProgramPropertyEditor
        roots={[invalidTree]}
        selectedId="invalid"
        catalogEntries={[variantSchemas]}
        program={authoringProgram([invalidTree])}
        busy={false}
        onMutate={vi.fn()}
        onUpdateNodePolicies={vi.fn()}
      />,
    );

    expect(markup.match(/Catalog-invalid value:/g)).toHaveLength(2);
    expect(markup).toContain('&quot;rpy_deg&quot;');
    expect(markup).toContain('&quot;xyzw&quot;');
    expect(markup).toContain('&quot;value_rad&quot;');
    expect(markup).toContain('&quot;value_m&quot;');
    expect(markup).not.toContain('data-editor="motion-pose"');
    expect(markup).not.toContain('data-editor="gripper-coordinate"');
  });

  it('renders source joints as checked sparse positions plus empty disabled choices', () => {
    const markup = renderToStaticMarkup(
      <ProgramSchemaValueEditor
        schema={{
          type: 'array',
          minItems: 1,
          maxItems: 6,
          items: {
            type: 'object',
            additionalProperties: false,
            required: ['joint_name', 'position_rad'],
            properties: {
              joint_name: {
                type: 'string',
                'x-manyforge-semantic-type': 'source_joint',
              },
              position_rad: { type: 'number' },
            },
          },
        }}
        value={[
          { joint_name: 'joint3', position_rad: 0.75 },
          { joint_name: 'joint1', position_rad: 0 },
        ]}
        disabled={false}
        context={{
          entry: timer,
          node: tree.children[0]!,
          parameterId: 'goal',
          program: authoringProgram([tree]),
          sourceJointInventory: {
            jointNames: ['joint1', 'joint2', 'joint3'],
            targetRole: 'arm',
            unavailableReason: null,
          },
        }}
        onCommit={vi.fn()}
      />,
    );

    expect(markup.match(/type="checkbox"/g)).toHaveLength(3);
    expect(markup).toContain('<code>joint3</code>');
    expect(markup).toContain('<code>joint2</code>');
    expect(markup).toContain('value="0.75"');
    expect(markup).toContain('aria-label="joint2 target position in radians"');
    expect(markup).toContain('disabled=""');
    expect(markup).not.toContain('Add joint');
    expect(markup).not.toContain('Remove joint');
    expect(markup).not.toContain('Joint name');
  });

  it('returns to typed values only with a valid literal to restore', () => {
    const field = timer.parameters[0]!;
    const bound: ProgramTreeNode = {
      node_id: 'timer',
      instance_id: 'required-source',
      parameters: {},
      value_sources: { duration_s: { kind: 'blackboard_key', reference: 'duration' } },
      children: [],
    };
    // A required value bound before it had a literal has nothing to restore.
    expect(typeValuesTransition(bound, 'duration_s', field, [])).toEqual({
      error: 'duration_s has no valid literal fallback',
    });
    // A parameter binding restores the parameter's default.
    expect(typeValuesTransition({
      ...bound,
      value_sources: { duration_s: { kind: 'program_parameter', reference: 'wait_time' } },
    }, 'duration_s', field, [parameter])).toEqual({
      transition: { kind: 'type_values', literal: 0.5 },
    });
    // The literal kept beside the source is restored as it is.
    const kept = { ...bound, parameters: { duration_s: 2 } };
    expect(typeValuesTransition(kept, 'duration_s', field, [])).toEqual({
      transition: { kind: 'type_values' },
    });
    expect(buildProgramParameterMutation(kept, 'duration_s', { kind: 'type_values' }).args)
      .toMatchObject({ parameters: { duration_s: 2 }, value_sources: {} });
  });

  it('keeps a map entry supplied only by a source and types it with the entry default', () => {
    const query: ProgramTreeNode = {
      node_id: 'assistant.query_model',
      instance_id: 'query',
      parameters: { template_variables: { count: '1' } },
      value_sources: { 'template_variables.label': { kind: 'program_parameter', reference: 'label' } },
      children: [],
    };
    // Typing another entry keeps the sourced one; removing the map's literal entry would not.
    const edited = buildProgramParameterMutation(query, 'template_variables', {
      kind: 'literal',
      value: { count: '2', extra: 'x' },
    }).args;
    expect(edited.value_sources).toEqual(query.value_sources);
    const typed = { ...query, parameters: { template_variables: { count: '1', label: 'tray' } } };
    expect(buildProgramParameterMutation(typed, 'template_variables', {
      kind: 'literal',
      value: { count: '1' },
    }).args.value_sources).toEqual({});
    // Type values gives the sourced entry its typed default.
    const field = { id: 'template_variables', required: true, schema: {}, schemaHash: 'sha256:m', hasDefault: false };
    const result = typeValuesTransition(query, 'template_variables.label', field, [], '');
    expect(result).toEqual({ transition: { kind: 'type_values', literal: '' } });
    expect(buildProgramParameterMutation(query, 'template_variables.label', {
      kind: 'type_values',
      literal: '',
    }).args).toMatchObject({
      parameters: { template_variables: { count: '1', label: '' } },
      value_sources: {},
    });
  });

  it('sets and removes a formula at a nested path without touching the pose literal', () => {
    const withFormula = buildProgramParameterMutation(mutationNode, 'pose.position_m[2]', {
      kind: 'source',
      source: { kind: 'expression', expression: '-param.tube_length / 2' },
    }).args;
    expect(withFormula.parameters).toEqual(mutationNode.parameters);
    expect(withFormula.value_sources).toEqual({
      ...mutationNode.value_sources,
      'pose.position_m[2]': { kind: 'expression', expression: '-param.tube_length / 2' },
    });
    // Binding the whole pose replaces the entries inside it.
    const node = { ...mutationNode, value_sources: withFormula.value_sources as ProgramTreeNode['value_sources'] };
    expect(buildProgramParameterMutation(node, 'pose', {
      kind: 'source',
      source: { kind: 'blackboard_key', reference: 'place_pose' },
    }).args.value_sources).toEqual({
      ...mutationNode.value_sources,
      pose: { kind: 'blackboard_key', reference: 'place_pose' },
    });
    // Typing a new pose keeps the formula while its value still exists.
    const typed = buildProgramParameterMutation(node, 'pose', {
      kind: 'literal',
      value: { position_m: [1, 2, 3], orientation: { rpy_deg: [0, 0, 0] } },
    }).args.value_sources as Record<string, unknown>;
    expect(typed['pose.position_m[2]']).toEqual({ kind: 'expression', expression: '-param.tube_length / 2' });
  });

  it('shows a formula in its field with the f(x) marker and a whole pose key as one chip', () => {
    const poseEntry: CatalogEntry = { ...timer, id: 'mover', parameters: [{
      id: 'goal',
      required: true,
      schema: {
        type: 'object',
        required: ['position_m', 'orientation'],
        properties: {
          position_m: { type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } },
          orientation: {
            type: 'object',
            properties: {
              rpy_deg: { type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } },
              xyzw: { type: 'array', minItems: 4, maxItems: 4, items: { type: 'number' } },
            },
          },
        },
      },
      schemaHash: 'sha256:goal',
      hasDefault: false,
    }] };
    const render = (node: ProgramTreeNode) => renderToStaticMarkup(
      <ProgramPropertyEditor
        roots={[node]}
        selectedId="move"
        catalogEntries={[poseEntry]}
        program={authoringProgram([node], [], [{
          key: 'place_pose', type: 'stored_pose', access: 'read', persistence: 'run',
        }])}
        busy={false}
        onMutate={vi.fn()}
        onUpdateNodePolicies={vi.fn()}
      />,
    );
    const goal = { position_m: [0.3, 0.1, 0], orientation: { rpy_deg: [180, 0, 0] } };
    const formula = render({
      node_id: 'mover', instance_id: 'move', children: [], parameters: { goal },
      value_sources: { 'goal.position_m[2]': { kind: 'expression', expression: 'param.z + 0.02' } },
    });
    expect(formula).toMatch(/<span class="program-value-field sourced"><input readOnly="" aria-label="Position z: formula param.z \+ 0.02"[^>]*value="param.z \+ 0.02"/);
    expect(formula).toContain('title="f(x) = param.z + 0.02"');
    expect(formula).toContain('>f(x)</button>');
    // The other coordinates stay typed, each in its own marked field.
    expect(formula.match(/class="program-value-field"/g)).toHaveLength(5);
    // Roll/pitch/yaw with a formula cannot switch to a quaternion that would drop it.
    const angle = render({
      node_id: 'mover', instance_id: 'move', children: [], parameters: { goal },
      value_sources: { 'goal.orientation.rpy_deg[2]': { kind: 'expression', expression: 'key.yaw' } },
    });
    expect(angle).toMatch(/aria-label="Orientation representation" disabled=""/);

    const chip = render({
      node_id: 'mover', instance_id: 'move', children: [], parameters: { goal },
      value_sources: { goal: { kind: 'blackboard_key', reference: 'place_pose' } },
    });
    expect(chip).toContain('class="program-value-chip" title="key.place_pose is empty until written.">key.place_pose</button>');
    expect(chip).not.toContain('Orientation representation');
  });

  it('reports declaration usage and prevents deleting a referenced parameter', () => {
    const markup = renderToStaticMarkup(
      <ProgramDeclarationsEditor
        roots={[tree]}
        parameters={[parameter]}
        namespaces={[]}
        keys={[]}
        formulaReferences={{ computedKeys: {}, nodes: {} }}
        busy={false}
        onMutate={vi.fn()}
      />,
    );

    expect(markup).toContain('1 binding');
    expect(markup).toContain('title="settle.duration_s"');
    expect(markup).toContain('disabled=""');
  });

  it('lays out a broad supported tree within a practical authoring budget', () => {
    const broadTree: ProgramTreeNode = {
      ...tree,
      children: Array.from({ length: 512 }, (_, index) => ({
        node_id: 'timer',
        instance_id: `timer_${index}`,
        parameters: { duration_s: 0.1 },
        children: [],
      })),
    };
    const started = performance.now();
    const markup = renderToStaticMarkup(
      <ProgramTreeCanvas
        roots={[broadTree]}
        retainedPositions={{}}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="timer_511"
        invalidNodeIds={new Set()}
        busy={false}
        onSelect={vi.fn()}
        onMutate={vi.fn()}
      />,
    );
    const elapsedMs = performance.now() - started;

    expect(markup.match(/data-node-id=/g)).toHaveLength(513);
    expect(markup).toContain('Node type');
    expect(markup).toContain('Move earlier');
    expect(markup).toContain('Move later');
    expect(markup).not.toContain('Add child');
    expect(markup).not.toContain('Change type');
    expect(elapsedMs).toBeLessThan(2_000);
  });

  it('keeps the complete authoring geometry when no Draft is open', () => {
    const controller = {
      mutationAvailable: true,
      acceptedApply: null,
      mutationUnavailableReason: null,
      documentUnavailableReason: null,
      admissionObserved: true,
      fenceDraft: null,
      refreshApplied: vi.fn(),
      state: {
        status: 'ready',
        requestGeneration: 1,
        pendingAction: null,
        workspace: {
          schemaVersion: 'manyforge.composer_program_workspace.v2',
          authoringWorkspace: {
            workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
            programDraftId: 'empty-draft', programDraftRevision: 0,
            deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
          },
          draft: {
            draftId: 'empty-draft',
            draftRevision: 0,
            programLoaded: false,
            authoring: null,
            candidateAvailable: false,
            programRevision: null,
            program: null,
            diagnostics: [],
            formulaReferences: { computedKeys: {}, nodes: {} },
          },
          applicability: {
            applyReady: true,
            unavailableReason: null,
          },
          saved: {
            available: false,
            matchesDraft: false,
            path: null,
            programRevision: null,
          },
          applied: {
            available: false,
            matchesDraft: null,
            programRevision: null,
            unavailableReason: null,
          },
          hasUnsavedChanges: false,
          hasUnappliedChanges: null,
        },
        message: null,
        transitionStatus: null,
        error: null,
      },
      refresh: vi.fn(),
      mutate: vi.fn(),
      newScenarioDocument: vi.fn(),
      openDraft: vi.fn(),
      save: vi.fn(),
      saveAs: vi.fn(),
      apply: vi.fn(),
      discard: vi.fn(),
      revert: vi.fn(),
      saveScenarioDocument: vi.fn(),
      exportScenarioDocument: vi.fn(),
      loadScenarioDocument: vi.fn(),
      scenarioDocumentPath: null,
      scenarioDocument: { id: null, name: null, description: null },
      scenarioSourceDiagnostics: [],
      updateNodePolicies: vi.fn(),
      clearFeedback: vi.fn(),
    } satisfies ProgramWorkspaceController;
    const runtime = {
      state: {
        applied: null,
        currentScenario: null,
        run: null,
        controls: null,
        nonExecutable: null,
        runnerAvailable: false,
        loading: false,
      observationError: null,
      pending: null,
      selectionPending: [],
      controlPending: null,
        error: null,
      },
      cycleState: null,
      cycleFailure: null,
      active: false,
      commandAllowed: vi.fn(() => false),
      commandDisabledReason: vi.fn(() => null),
      draftUnappliedReason: null,
      refresh: vi.fn(),
      dismissError: vi.fn(),
      run: vi.fn(),
      stopMotion: vi.fn(),
      startMotion: vi.fn(),
      resumeMotion: vi.fn(),
      groupRuntimeScales: {},
      setGroupRuntimeScale: vi.fn(),
      commitGroupRuntimeScale: vi.fn(),
      fullStop: vi.fn(),
      resumeFullStop: vi.fn(),
      reset: vi.fn(),
      applyRecoveryAction: vi.fn(),
      writeRuntimeBlackboard: vi.fn(),
      persistentValues: { read: vi.fn(), edit: vi.fn(), clear: vi.fn() },
      stageManualResult: vi.fn(),
      motionAttentionBlock: vi.fn(() => null),
    } satisfies ProgramRuntimeController;
    const markup = renderToStaticMarkup(
      <ProgramWorkspace
        controller={controller}
        runtime={runtime}
        catalog={{
          status: 'ready',
          data: {
            apiVersion: 'manyforge.node_catalog.v2',
            contentVersion: 'v2',
            catalogId: 'test-catalog',
            entries: [sequence, timer],
            catalogDigest: 'sha256:catalog',
            environmentDigest: environment.environmentDigest,
          },
        }}
        environment={{ status: 'ready', data: environment }}
        sourceModel={{ status: 'loading' }}
        onRetryCatalog={vi.fn()}
        onRetryEnvironment={vi.fn()}
      />,
    );

    expect(markup).toContain('program-authoring-empty');
    expect(markup).toContain('Open a Draft to use the palette');
    expect(markup).toContain('No Program Draft is open');
    expect(markup).toContain('Params');
    expect(markup).toContain('Resize Program tools pane');
    expect(markup).not.toContain('Resize Program inspector pane');
    expect(markup).not.toContain('Program actions ·');
    expect(markup).not.toContain('program-tree-node');
  });

  it('keeps editing available and gates the final Apply on the owner applicability reason', () => {
    const blockedTree: ProgramTreeNode = {
      ...tree,
      children: [{
        ...tree.children[0]!,
        recovery_policy: { allow_intervention: true, max_retries: 0 },
      }],
    };
    const blockedWorkspace: ProgramWorkspaceResponse = {
      schemaVersion: 'manyforge.composer_program_workspace.v2',
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
        programDraftId: 'draft-blocked', programDraftRevision: 2,
        deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
      },
      draft: {
        draftId: 'draft-blocked',
        draftRevision: 2,
        programLoaded: true,
        authoring: {
          schema_version: 'manyforge.composer_authoring_forest.v1',
          api_version: 'manyforge.program.v2',
          program_id: 'program-blocked',
          program_profile: 'composable-cell-v1',
          name: 'Blocked',
          catalog: {
            api_version: 'manyforge.node_catalog.v2',
            content_version: 'fixture',
          },
          component_roles: {},
          target_roles: {},
          forest: [blockedTree],
          positions: {},
        },
        candidateAvailable: true,
        programRevision: `sha256:${'a'.repeat(64)}`,
        program: {
          api_version: 'manyforge.program.v2',
          program_id: 'program-blocked',
          program_revision: `sha256:${'a'.repeat(64)}`,
          program_profile: 'composable-cell-v1',
          name: 'Blocked',
          catalog: {
            api_version: 'manyforge.node_catalog.v2',
            content_version: 'fixture',
          },
          component_roles: {},
          target_roles: {},
          tree: blockedTree,
        },
        diagnostics: [],
        formulaReferences: { computedKeys: {}, nodes: {} },
      },
      applicability: {
        applyReady: false,
        unavailableReason: {
          code: 'composer_authoring_draft_incomplete',
          message: 'Program requires exactly one root; found 0.',
          restorationGates: [],
        },
      },
      saved: {
        available: true,
        matchesDraft: false,
        path: '/workspace/blocked.program.yaml',
        programRevision: `sha256:${'b'.repeat(64)}`,
      },
      applied: {
        available: false,
        matchesDraft: null,
        programRevision: null,
        unavailableReason: null,
      },
      hasUnsavedChanges: true,
      hasUnappliedChanges: null,
    };
    const controller = {
      mutationAvailable: true,
      acceptedApply: null,
      mutationUnavailableReason: null,
      documentUnavailableReason: null,
      admissionObserved: true,
      fenceDraft: null,
      refreshApplied: vi.fn(),
      state: {
        status: 'ready',
        requestGeneration: 1,
        pendingAction: null,
        workspace: blockedWorkspace,
        message: null,
        transitionStatus: null,
        error: null,
      },
      refresh: vi.fn(),
      mutate: vi.fn(),
      newScenarioDocument: vi.fn(),
      openDraft: vi.fn(),
      save: vi.fn(),
      saveAs: vi.fn(),
      apply: vi.fn(),
      discard: vi.fn(),
      revert: vi.fn(),
      saveScenarioDocument: vi.fn(),
      exportScenarioDocument: vi.fn(),
      loadScenarioDocument: vi.fn(),
      scenarioDocumentPath: null,
      scenarioDocument: { id: null, name: null, description: null },
      scenarioSourceDiagnostics: [],
      updateNodePolicies: vi.fn(),
      clearFeedback: vi.fn(),
    } satisfies ProgramWorkspaceController;
    const runtime = {
      state: {
        applied: null,
        currentScenario: null,
        run: null,
        controls: null,
        nonExecutable: null,
        runnerAvailable: true,
        loading: false,
      observationError: null,
      pending: null,
      selectionPending: [],
      controlPending: null,
        error: null,
      },
      cycleState: 'idle_after_reset',
      cycleFailure: null,
      active: false,
      commandAllowed: vi.fn(() => false),
      commandDisabledReason: vi.fn(() => null),
      draftUnappliedReason: null,
      refresh: vi.fn(),
      dismissError: vi.fn(),
      run: vi.fn(),
      stopMotion: vi.fn(),
      startMotion: vi.fn(),
      resumeMotion: vi.fn(),
      groupRuntimeScales: {},
      setGroupRuntimeScale: vi.fn(),
      commitGroupRuntimeScale: vi.fn(),
      fullStop: vi.fn(),
      resumeFullStop: vi.fn(),
      reset: vi.fn(),
      applyRecoveryAction: vi.fn(),
      writeRuntimeBlackboard: vi.fn(),
      persistentValues: { read: vi.fn(), edit: vi.fn(), clear: vi.fn() },
      stageManualResult: vi.fn(),
      motionAttentionBlock: vi.fn(() => null),
    } satisfies ProgramRuntimeController;
    const markup = renderToStaticMarkup(
      <div>
        <ProgramDocumentControls
          controller={controller}
          runtime={runtime}
          scenarioCommands={<ScenarioDocumentCommands controller={controller} />}
        />
        <ProgramWorkspace
          controller={controller}
          runtime={runtime}
          catalog={{
            status: 'ready',
            data: {
              apiVersion: 'manyforge.node_catalog.v2',
              contentVersion: 'fixture',
              catalogId: 'catalog',
              entries: [sequence, timer],
              catalogDigest: 'sha256:catalog',
              environmentDigest: environment.environmentDigest,
            },
          }}
          environment={{ status: 'ready', data: environment }}
          sourceModel={{ status: 'loading' }}
          onRetryCatalog={vi.fn()}
          onRetryEnvironment={vi.fn()}
        />
      </div>,
    );
    const controlsSource = readFileSync(
      new URL('./ProgramDocumentControls.tsx', import.meta.url),
      'utf8',
    );

    expect(markup).toContain('>Apply</button>');
    expect(markup).toContain('aria-label="Save Scenario (choose a project folder)"');
    expect(markup).toContain('aria-label="Load Scenario"');
    expect(markup).not.toContain('>Open</button>');
    expect(controlsSource).toContain('!workspace?.applicability.applyReady');
    expect(controlsSource).not.toContain('Affected nodes:');
    expect(controlsSource).toContain('workspace?.applicability.unavailableReason?.message');
    expect(controlsSource).toContain(
      "compact={action === 'apply' || action === 'discard' || action === 'revert'}",
    );
    expect(controlsSource).toContain('Apply this Program Draft to');
    expect(controlsSource).toContain('Its Deployment will not change.');
    expect(controlsSource).toContain('Replace the Applied Program and Draft with the Saved Program?');
    expect(controlsSource).not.toContain('className="program-current-scenario"');
    expect(controlsSource).toContain('Your Saved Program will not change.');
    const settingsSource = readFileSync(
      new URL('./ProgramSettingsEditor.tsx', import.meta.url),
      'utf8',
    );
    expect(settingsSource).toContain(
      'currentScenario.programBindingId === appliedBinding.program_binding_id',
    );
  });
});
