import { describe, expect, it } from 'vitest';

import type {
  AppliedProgramSelection,
  ProgramAuthoringDocument,
  ProgramDocument,
  ProgramTreeNode,
  SourceModelProjection,
} from '../types/api';
import { programSourceJointInventory } from './programSourceJoints';

const targetRole = {
  component_role: 'robot',
  required_capabilities: [],
  required_frame_roles: ['base', 'tcp'],
};

const node: ProgramTreeNode = {
  node_id: 'motion.move_to_joint_target',
  instance_id: 'move_home',
  parameters: {
    target_role: 'arm',
    goal: [{ joint_name: 'joint1', position_rad: 0 }],
  },
  children: [],
};

const authoring: ProgramAuthoringDocument = {
  api_version: 'manyforge.program.v2',
  program_id: 'pick',
  program_profile: 'composable-cell-v1',
  name: 'Pick',
  catalog: {
    api_version: 'manyforge.node_catalog.v2',
    content_version: '2.4.0',
  },
  component_roles: { robot: {} },
  target_roles: { arm: targetRole },
  schema_version: 'manyforge.composer_authoring_forest.v1',
  forest: [node],
  positions: {},
};

const program: ProgramDocument = {
  api_version: 'manyforge.program.v2',
  program_id: 'pick',
  program_revision: 'sha256:program',
  program_profile: 'composable-cell-v1',
  name: 'Pick',
  catalog: {
    api_version: 'manyforge.node_catalog.v2',
    content_version: '2.4.0',
  },
  component_roles: { robot: {} },
  target_roles: { arm: targetRole },
  tree: node,
};

const applied: AppliedProgramSelection = {
  schemaVersion: 'manyforge.applied_program_selection.v3',
  scenarioId: 'lite6',
  sourceDocumentDigest: 'sha256:catalog',
  projectPath: '/projects/lite6.scenario',
  programId: 'pick',
  programRevision: 'sha256:program',
  programBindingId: 'sha256:binding',
  retainedBundleId: 'sha256:bundle',
  graphRevision: 'sha256:graph',
  nodeEnvironmentDigest: 'sha256:environment',
  recordDigest: 'sha256:record',
  sourceOwnerId: 'program-owner',
  sourceOwnerEpoch: 'epoch',
  sourceGeneration: 1,
  assistantProjection: null,
  ownerIdentities: [{
    ownerRole: 'headless_projection',
    ownerId: 'program-owner',
    ownerEpoch: 'epoch',
  }],
  program,
  programBinding: {
    api_version: 'manyforge.program_binding.v2',
    program_binding_id: 'sha256:binding',
    program_id: 'pick',
    program_revision: 'sha256:program',
    retained_bundle_id: 'sha256:bundle',
    graph_revision: 'sha256:graph',
    node_environment: {
      catalog_id: 'manyforge_builtin',
      content_version: '2.4.0',
      catalog_digest: 'sha256:catalog',
      environment_digest: 'sha256:environment',
    },
    component_role_bindings: [],
    target_role_bindings: [{
      target_role_id: 'arm',
      component_role_id: 'robot',
      controllable_component_id: 'lite6',
      command_target_id: 'lite6_arm',
      frame_role_bindings: [],
      resource_role_bindings: [],
      collision_body_role_bindings: [],
      capability_bindings: [],
    }],
    node_policies: [],
    contact_grasp_bindings: [],
    provenance: {
      binding_compiler_contract_digest: 'sha256:contract',
      role_assignment_digest: 'sha256:roles',
      source_records: [],
      resolved_output_records: [],
    },
  },
};

const sourceModel: SourceModelProjection = {
  schema_version: 'manyforge.source_model_projection.v2',
  cell_id: 'cell',
  retained_bundle_id: 'sha256:bundle',
  graph_revision: 'sha256:graph',
  model_map_contract: null,
  model_map_product_id: null,
  occurrences: [],
  targets: [{
    target_id: 'lite6_arm',
    component_id: 'lite6',
    collision_bodies: [],
    frames: [],
    model_groups: [{
      model_group_role_id: 'arm',
      model_instance_id: 'lite6-model',
      source_group_name: 'lite6',
      source_joint_names: ['joint1', 'fixed_joint', 'joint2'],
      commandable_source_joint_names: ['joint1', 'joint2'],
      source_link_names: [],
    }, {
      model_group_role_id: 'overlap',
      model_instance_id: 'lite6-model',
      source_group_name: 'overlap',
      source_joint_names: ['joint2', 'joint3'],
      commandable_source_joint_names: ['joint2', 'joint3'],
      source_link_names: [],
    }],
  }],
  source: {
    available: true,
    freshness: 'fresh',
    known_gaps: [],
    projection_generation: 1,
    projection_owner_epoch: 'projection-epoch',
    receiver_age_ns: 0,
    selection_projection_generation: 1,
    selection_source_content_digest: 'sha256:selection-content',
    selection_source_generation: 1,
    selection_source_owner_epoch: 'selection-epoch',
    selection_source_owner_id: 'manager',
    source_content_digest: 'sha256:source-content',
    source_generation: 1,
    source_owner_epoch: 'source-epoch',
    source_owner_id: 'manager',
    source_owner_kind: 'source_model_projection',
  },
};

describe('Program source-joint inventory', () => {
  it('joins the exact target binding to ordered unique commandable source joints', () => {
    expect(programSourceJointInventory(
      authoring,
      node,
      applied,
      sourceModel,
    )).toEqual({
      jointNames: ['joint1', 'joint2', 'joint3'],
      targetRole: 'arm',
      unavailableReason: null,
    });
  });

  it('refuses a stale source model or a target role changed beyond its binding', () => {
    expect(programSourceJointInventory(
      authoring,
      node,
      applied,
      { ...sourceModel, graph_revision: 'sha256:other' },
    ).unavailableReason).toContain('do not match');

    expect(programSourceJointInventory(
      {
        ...authoring,
        target_roles: {
          arm: { ...targetRole, required_frame_roles: ['different'] },
        },
      },
      node,
      applied,
      sourceModel,
    ).unavailableReason).toContain('differs from the current Scenario binding');
  });

  it('does not borrow another Program binding or invent joints without owner facts', () => {
    expect(programSourceJointInventory(
      { ...authoring, program_id: 'other-program' },
      node,
      applied,
      sourceModel,
    ).unavailableReason).toContain('current Scenario binding');

    expect(programSourceJointInventory(
      authoring,
      node,
      applied,
      null,
      'Source model is still loading.',
    )).toEqual({
      jointNames: [],
      targetRole: 'arm',
      unavailableReason: 'Source model is still loading.',
    });

    expect(programSourceJointInventory(
      authoring,
      node,
      applied,
      {
        ...sourceModel,
        targets: [{
          ...sourceModel.targets[0],
          component_id: 'different-component',
        }],
      },
    ).unavailableReason).toContain('absent from the source-model projection');
  });
});
