import { describe, expect, it } from 'vitest';

import type { AppliedProgramSelection } from '../types/api';
import { appliedExecutionSelectionRows } from './programExecutionGroups';


const digest = (character: string) => `sha256:${character.repeat(64)}`;

function appliedProgram(): AppliedProgramSelection {
  return {
    schemaVersion: 'manyforge.applied_program_selection.v2',
    scenarioId: 'two-robots',
    sourceDocumentDigest: digest('a'),
    programId: 'two-robots',
    programRevision: digest('b'),
    programBindingId: digest('c'),
    retainedBundleId: digest('d'),
    graphRevision: digest('e'),
    nodeEnvironmentDigest: digest('f'),
    recordDigest: digest('1'),
    sourceOwnerId: 'applied-owner',
    sourceOwnerEpoch: 'applied-owner.epoch.1',
    sourceGeneration: 1,
    assistantProjection: null,
    ownerIdentities: [{
      ownerRole: 'headless_projection',
      ownerId: 'applied-owner',
      ownerEpoch: 'applied-owner.epoch.1',
    }],
    program: {
      api_version: 'manyforge.program.v2',
      program_id: 'two-robots',
      program_revision: digest('b'),
      program_profile: 'composable-cell-v1',
      name: 'Two robots',
      catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '2.3.0' },
      component_roles: {},
      target_roles: {},
      execution_groups: { robots: { execution_policy: 'independent' } },
      tree: {
        node_id: 'parallel',
        instance_id: 'root',
        parameters: {},
        execution_groups: ['robots'],
        children: [{
          node_id: 'motion.move_to_joint_target',
          instance_id: 'left-motion',
          parameters: { target_role: 'left-arm' },
          children: [],
        }, {
          node_id: 'motion.move_to_pose',
          instance_id: 'right-motion',
          parameters: { target_role: 'right-arm' },
          children: [],
        }],
      },
    },
    programBinding: {
      api_version: 'manyforge.program_binding.v1',
      program_binding_id: digest('c'),
      program_id: 'two-robots',
      program_revision: digest('b'),
      retained_bundle_id: digest('d'),
      graph_revision: digest('e'),
      node_environment: {
        catalog_id: 'default', content_version: '2.3.0',
        catalog_digest: digest('2'), environment_digest: digest('f'),
      },
      component_role_bindings: [],
      target_role_bindings: [{
        target_role_id: 'left-arm', component_role_id: 'left-robot',
        controllable_component_id: 'left', command_target_id: 'arm',
        frame_role_bindings: [], resource_role_bindings: [],
        collision_body_role_bindings: [], capability_bindings: [],
      }, {
        target_role_id: 'right-arm', component_role_id: 'right-robot',
        controllable_component_id: 'right', command_target_id: 'arm',
        frame_role_bindings: [], resource_role_bindings: [],
        collision_body_role_bindings: [], capability_bindings: [],
      }],
      node_policies: [], contact_grasp_bindings: [],
      provenance: {
        binding_compiler_contract_digest: digest('3'), role_assignment_digest: digest('4'),
        source_records: [], resolved_output_records: [],
      },
    },
  };
}

describe('Applied Program execution-group projection', () => {
  it('provides exact persistent velocity selectors before a Program run exists', () => {
    const rows = appliedExecutionSelectionRows(appliedProgram());

    expect(rows.map((row) => row.groupId)).toEqual(['all', 'robots']);
    expect(rows[1].nodeCount).toBe(3);
    expect(rows[1].runtimeScaleUnavailableReason).toBeNull();
    expect(rows[1].runtimeScaleSelector).toEqual({
      kind: 'group',
      programBindingId: digest('c'),
      groupTargets: [
        { componentRoleId: 'left-robot', targetRoleId: 'left-arm' },
        { componentRoleId: 'right-robot', targetRoleId: 'right-arm' },
      ],
    });
  });
});
