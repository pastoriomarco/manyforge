import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  CatalogEntry,
  NodeEnvironmentResponse,
  ProgramParameterDeclaration,
  ProgramTreeNode,
  ProgramWorkspaceResponse,
} from '../types/api';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import { ProgramNodePalette } from './ProgramNodePalette';
import {
  buildProgramParameterMutation,
  ProgramPropertyEditor,
} from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';
import { ProgramWorkspace } from './ProgramWorkspace';

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
        root={tree}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="root_sequence"
        busy={false}
        onMutate={vi.fn()}
      />,
    );

    expect(markup).toContain('Authoring only · execution gate N7');
    expect(markup).toContain('draggable="true"');
    expect(markup).not.toContain('Authoring is currently unavailable');
  });

  it('renders one deterministic selected tree and exact v1 value source', () => {
    const markup = renderToStaticMarkup(
      <div>
        <ProgramTreeCanvas
          root={tree}
          catalogEntries={[sequence, timer]}
          environment={environment}
          selectedId="settle"
          busy={false}
          onSelect={vi.fn()}
          onMutate={vi.fn()}
        />
        <ProgramPropertyEditor
          root={tree}
          selectedId="settle"
          catalogEntries={[sequence, timer]}
          programParameters={[parameter]}
          blackboardKeys={[]}
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
        root={invalidTree}
        selectedId="invalid"
        catalogEntries={[variantSchemas]}
        programParameters={[]}
        blackboardKeys={[]}
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
        root={requiredTree}
        selectedId="required-source"
        catalogEntries={[timer]}
        programParameters={[]}
        blackboardKeys={[{
          key: 'duration',
          type: 'float64',
          access: 'read',
          persistence: 'run',
        }]}
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
        root={tree}
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
        root={broadTree}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="timer_511"
        busy={false}
        onSelect={vi.fn()}
        onMutate={vi.fn()}
      />,
    );
    const elapsedMs = performance.now() - started;

    expect(markup.match(/data-node-id=/g)).toHaveLength(513);
    expect(elapsedMs).toBeLessThan(2_000);
  });

  it('keeps the complete authoring geometry when no Draft is open', () => {
    const controller = {
      mutationAvailable: true,
      state: {
        status: 'ready',
        requestGeneration: 1,
        pendingAction: null,
        workspace: {
          schemaVersion: 'manyforge.composer_program_workspace.v1',
          draft: {
            draftId: 'empty-draft',
            draftRevision: 0,
            programLoaded: false,
            programRevision: null,
            program: null,
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
      load: vi.fn(),
      updateNodePolicies: vi.fn(),
      clearFeedback: vi.fn(),
    } satisfies ProgramWorkspaceController;
    const runtime = {
      state: {
        catalog: null,
        applied: null,
        run: null,
        selectedScenarioId: null,
        runnerAvailable: false,
        loading: false,
        pending: null,
        error: null,
      },
      cycleState: null,
      active: false,
      refresh: vi.fn(),
      selectScenario: vi.fn(),
      clearScenarioSelection: vi.fn(),
      run: vi.fn(),
      stop: vi.fn(),
      reset: vi.fn(),
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
        selectedProgramPath={null}
        onRetryCatalog={vi.fn()}
        onRetryEnvironment={vi.fn()}
      />,
    );

    expect(markup).toContain('program-authoring-empty');
    expect(markup).toContain('Open a Draft to use the palette');
    expect(markup).toContain('No Program Draft is open');
    expect(markup).toContain('Select a node to inspect properties');
    expect(markup).toContain(
      'Program actions · New · Open · Save As · Apply · Revert · Discard · Load',
    );
    expect(markup).not.toContain('program-tree-node');
  });

  it('disables only global Apply for an imported skill on another node', () => {
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
      schemaVersion: 'manyforge.composer_program_workspace.v1',
      draft: {
        draftId: 'draft-blocked',
        draftRevision: 2,
        programLoaded: true,
        programRevision: `sha256:${'a'.repeat(64)}`,
        program: {
          api_version: 'manyforge.program.v1',
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
      state: {
        status: 'ready',
        requestGeneration: 1,
        pendingAction: null,
        workspace: blockedWorkspace,
        message: null,
        transitionStatus: null,
        error: null,
        comparisonStatus: 'observed',
        refreshWarning: null,
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
      load: vi.fn(),
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
        run: null,
        selectedScenarioId: 'qualified',
        runnerAvailable: true,
        loading: false,
        pending: null,
        error: null,
      },
      cycleState: 'idle_after_reset',
      active: false,
      refresh: vi.fn(),
      selectScenario: vi.fn(),
      clearScenarioSelection: vi.fn(),
      run: vi.fn(),
      stop: vi.fn(),
      reset: vi.fn(),
    } satisfies ProgramRuntimeController;
    const markup = renderToStaticMarkup(
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
        selectedProgramPath={null}
        onRetryCatalog={vi.fn()}
        onRetryEnvironment={vi.fn()}
      />,
    );

    expect(markup).toMatch(/<button type="button" disabled="">Apply Draft<\/button>/);
    expect(markup).toContain('Affected node instances: settle.');
    expect(markup).toContain(
      'Allowed recovery skills are unavailable: each skill requires an exact revision-qualified retained referent. Leave Allowed skills empty.',
    );
    expect(markup).toContain('Save');
  });
});
