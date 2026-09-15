import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  BlackboardKeyDeclaration,
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
import { NodePaletteItem } from './NodePaletteItem';
import {
  ProgramNodePalette,
  defaultCollapsedCatalogCategories,
} from './ProgramNodePalette';
import {
  buildProgramParameterMutation,
  ProgramPropertyEditor,
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
  runtime_mutability: 'before_run',
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
      content_version: '2.3.0',
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

  it('binds either source kind without losing unrelated sources or literals', () => {
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
      parameters: {
        coordinate: mutationNode.parameters.coordinate,
        retained_literal: 7,
      },
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
      parameters: {
        pose: mutationNode.parameters.pose,
        retained_literal: 7,
      },
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
    expect(markup).toContain('program_parameter:wait_time');
    expect(markup).toContain('The literal is absent while this exact source is bound.');
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

  it('does not offer an empty Literal transition for a required blackboard binding', () => {
    const requiredTree: ProgramTreeNode = {
      node_id: 'timer',
      instance_id: 'required-source',
      parameters: {},
      value_sources: {
        duration_s: { kind: 'blackboard_key', reference: 'duration' },
      },
      children: [],
    };
    const markup = renderToStaticMarkup(
      <ProgramPropertyEditor
        roots={[requiredTree]}
        selectedId="required-source"
        catalogEntries={[timer]}
        program={authoringProgram([requiredTree], [], [{
          key: 'duration',
          type: 'float64',
          access: 'read',
          persistence: 'run',
        }])}
        busy={false}
        onMutate={vi.fn()}
        onUpdateNodePolicies={vi.fn()}
      />,
    );

    expect(markup).toContain('<option value="" disabled="">Literal value</option>');
    expect(markup).toContain(
      'A required parameter cannot become literal until a schema-valid fallback exists.',
    );
  });

  it('reports declaration usage and prevents deleting a referenced parameter', () => {
    const markup = renderToStaticMarkup(
      <ProgramDeclarationsEditor
        roots={[tree]}
        parameters={[parameter]}
        namespaces={[]}
        keys={[]}
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
    expect(markup).toContain('Add child');
    expect(markup).toContain('Add after');
    expect(markup).toContain('>Wrap</button>');
    expect(markup).toContain('Change type');
    expect(elapsedMs).toBeLessThan(2_000);
  });

  it('keeps the complete authoring geometry when no Draft is open', () => {
    const controller = {
      mutationAvailable: true,
      mutationUnavailableReason: null,
      admissionObserved: true,
      state: {
        status: 'ready',
        requestGeneration: 1,
        pendingAction: null,
        workspace: {
          schemaVersion: 'manyforge.composer_program_workspace.v2',
          draft: {
            draftId: 'empty-draft',
            draftRevision: 0,
            programLoaded: false,
            authoring: null,
            candidateAvailable: false,
            programRevision: null,
            program: null,
            diagnostics: [],
          },
          applicability: {
            applyReady: true,
            affectedInstanceIds: [],
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
      newDraft: vi.fn(),
      openDraft: vi.fn(),
      save: vi.fn(),
      saveAs: vi.fn(),
      apply: vi.fn(),
      discard: vi.fn(),
      revert: vi.fn(),
      loadScenario: vi.fn(),
      updateNodePolicies: vi.fn(),
      clearFeedback: vi.fn(),
    } satisfies ProgramWorkspaceController;
    const runtime = {
      state: {
        catalog: null,
        applied: null,
        currentScenario: null,
        run: null,
        controls: null,
        selectedScenarioId: null,
        runnerAvailable: false,
        loading: false,
      observationError: null,
      pending: null,
      controlPending: null,
        error: null,
      },
      cycleState: null,
      cycleFailure: null,
      active: false,
      commandAllowed: vi.fn(() => false),
      refresh: vi.fn(),
      selectScenario: vi.fn(),
      clearScenarioSelection: vi.fn(),
      dismissError: vi.fn(),
      run: vi.fn(),
      stopMotion: vi.fn(),
      resumeMotion: vi.fn(),
      fullStop: vi.fn(),
      resumeFullStop: vi.fn(),
      stop: vi.fn(),
      reset: vi.fn(),
      applyRecoveryAction: vi.fn(),
      stageObservationOverride: vi.fn(),
      activateRuntimeOverride: vi.fn(),
      clearRuntimeOverride: vi.fn(),
      promoteRuntimeOverride: vi.fn(),
      materializePromotion: vi.fn(),
      promotionMaterializationReason: vi.fn(() => null),
      writeRuntimeBlackboard: vi.fn(),
      stageManualResult: vi.fn(),
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

  it('keeps editing available and gates the final Apply for an imported skill', () => {
    const blockedTree: ProgramTreeNode = {
      ...tree,
      children: [{
        ...tree.children[0]!,
        recovery_policy: {
          allow_intervention: true,
          allowed_recovery_actions: ['intervention'],
          max_retries: 0,
          allowed_skills: ['legacy.inspect'],
          persistence_ceiling: 'session_only',
        },
      }],
    };
    const blockedWorkspace: ProgramWorkspaceResponse = {
      schemaVersion: 'manyforge.composer_program_workspace.v2',
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
      },
      applicability: {
        applyReady: false,
        affectedInstanceIds: ['settle'],
        unavailableReason: {
          code: 'composer_policy_skill_referent_unavailable',
          message: 'Allowed recovery skills are unavailable: each skill requires an exact revision-qualified retained referent. Leave Allowed skills empty.',
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
      mutationUnavailableReason: null,
      admissionObserved: true,
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
      newDraft: vi.fn(),
      openDraft: vi.fn(),
      save: vi.fn(),
      saveAs: vi.fn(),
      apply: vi.fn(),
      discard: vi.fn(),
      revert: vi.fn(),
      loadScenario: vi.fn(),
      updateNodePolicies: vi.fn(),
      clearFeedback: vi.fn(),
    } satisfies ProgramWorkspaceController;
    const runtime = {
      state: {
        catalog: {
          apiVersion: 'manyforge.scenario_catalog.v1',
          scenarioCatalogRevision: `sha256:${'c'.repeat(64)}`,
          scenarios: [{
            scenarioId: 'qualified',
            description: null,
            aliases: [],
            deploymentArtifactId: 'deployment',
            deploymentRevision: `sha256:${'d'.repeat(64)}`,
            retainedBundleId: `sha256:${'e'.repeat(64)}`,
            programBindingId: `sha256:${'f'.repeat(64)}`,
            assistantMode: 'optional',
          }],
        },
        applied: null,
        currentScenario: null,
        run: null,
        controls: null,
        selectedScenarioId: 'qualified',
        runnerAvailable: true,
        loading: false,
      observationError: null,
      pending: null,
      controlPending: null,
        error: null,
      },
      cycleState: 'idle_after_reset',
      cycleFailure: null,
      active: false,
      commandAllowed: vi.fn(() => false),
      refresh: vi.fn(),
      selectScenario: vi.fn(),
      clearScenarioSelection: vi.fn(),
      dismissError: vi.fn(),
      run: vi.fn(),
      stopMotion: vi.fn(),
      resumeMotion: vi.fn(),
      fullStop: vi.fn(),
      resumeFullStop: vi.fn(),
      stop: vi.fn(),
      reset: vi.fn(),
      applyRecoveryAction: vi.fn(),
      stageObservationOverride: vi.fn(),
      activateRuntimeOverride: vi.fn(),
      clearRuntimeOverride: vi.fn(),
      promoteRuntimeOverride: vi.fn(),
      materializePromotion: vi.fn(),
      promotionMaterializationReason: vi.fn(() => null),
      writeRuntimeBlackboard: vi.fn(),
      stageManualResult: vi.fn(),
    } satisfies ProgramRuntimeController;
    const markup = renderToStaticMarkup(
      <div>
        <ProgramDocumentControls controller={controller} runtime={runtime} />
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
    expect(markup).toContain('Save');
    expect(controlsSource).toContain('!workspace?.applicability.applyReady');
    expect(controlsSource).toContain('Affected nodes:');
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
