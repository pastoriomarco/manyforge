import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

import type { AppliedProgramSelection, ProgramTreeNode } from '../types/api';
import {
  appliedExecutionSelectionRows, executionGroupDisplay, executionGroupScopes,
  executionGroupTargetLabels, executionGroupVisibilityChange, executionGroupVisibilityUnavailableReason,
  executionSelectionIsQuiescent, HARDWARE_ACTING_NODE_IDS, shownExecutionGroupIds,
} from './programExecutionGroups';

describe('selection quiescence for Reset and Halt', () => {
  it('accepts Ready, Stopped and fully held mixtures, not moving or uncertain selections', () => {
    expect(executionSelectionIsQuiescent(undefined)).toBe(false);
    for (const state of ['ready', 'held', 'mixed'] as const) {
      expect(executionSelectionIsQuiescent({ state, nodeCount: 8, heldNodeCount: 8 })).toBe(true);
    }
    expect(executionSelectionIsQuiescent({ state: 'mixed', nodeCount: 8, heldNodeCount: 1 })).toBe(false);
    for (const state of ['running', 'stopping', 'faulted', 'unavailable'] as const) {
      expect(executionSelectionIsQuiescent({ state, nodeCount: 8, heldNodeCount: 8 })).toBe(false);
    }
  });
});


const digest = (character: string) => `sha256:${character.repeat(64)}`;

function appliedProgram(): AppliedProgramSelection {
  return {
    schemaVersion: 'manyforge.applied_program_selection.v3',
    scenarioId: 'two-robots',
    sourceDocumentDigest: digest('a'),
    projectPath: '/projects/two-robots.scenario',
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
      catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '2.4.0' },
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
      api_version: 'manyforge.program_binding.v2',
      program_binding_id: digest('c'),
      program_id: 'two-robots',
      program_revision: digest('b'),
      retained_bundle_id: digest('d'),
      graph_revision: digest('e'),
      node_environment: {
        catalog_id: 'default', content_version: '2.4.0',
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

describe('visible hardware-node coverage', () => {
  it('inherits branch membership, supports overlap and clears inheritance with All', () => {
    const tree = appliedProgram().program.tree;
    tree.execution_groups = ['left', 'shared'];
    tree.children[1].execution_groups = 'all';
    const scopes = executionGroupScopes([tree], ['left', 'shared']);
    expect(scopes.find((scope) => scope.groupId === 'left')).toMatchObject({
      nodeIds: ['root', 'left-motion'], hardwareNodeIds: ['left-motion'], targetRoleIds: ['left-arm'],
    });
    expect(scopes.find((scope) => scope.groupId === 'shared')?.hardwareNodeIds).toEqual(['left-motion']);
    expect(shownExecutionGroupIds(['all', 'left', 'shared'], undefined, scopes))
      .toEqual(['all', 'left', 'shared']);
    expect(executionGroupVisibilityUnavailableReason(scopes, ['left', 'shared'])).toContain('Show All');
    expect(executionGroupVisibilityUnavailableReason(scopes, ['all'])).toBeNull();
  });

  it('allows hiding redundant groups, but checks nodes rather than shared robot targets', () => {
    const tree = appliedProgram().program.tree;
    tree.execution_groups = ['shared'];
    tree.children[0].execution_groups = ['left', 'shared'];
    tree.children[1].execution_groups = ['right', 'shared'];
    tree.children[1].parameters.target_role = 'left-arm';
    const scopes = executionGroupScopes([tree], ['left', 'right', 'shared']);
    expect(executionGroupVisibilityUnavailableReason(scopes, ['left'])).toContain('Show All');
    expect(executionGroupVisibilityUnavailableReason(scopes, ['left', 'right'])).toBeNull();
    expect(executionGroupVisibilityUnavailableReason(scopes, ['shared'])).toBeNull();
    expect(executionGroupVisibilityUnavailableReason(scopes, [])).toContain('at least one');
  });

  it('reveals All for old saved visibility and new incomplete moves without mutating settings', () => {
    const tree = appliedProgram().program.tree;
    const hidden = ['all'];
    const groupIds = ['all', 'robots'];
    const original = executionGroupScopes([tree], ['robots']);
    expect(shownExecutionGroupIds(groupIds, hidden, original)).toEqual(['robots']);
    tree.children.push({ node_id: 'motion.move_to_pose', instance_id: 'new', parameters: {},
      execution_groups: 'all', children: [] });
    expect(shownExecutionGroupIds(groupIds, hidden, executionGroupScopes([tree], ['robots'])))
      .toEqual(['all', 'robots']);
    expect(hidden).toEqual(['all']);
    expect(shownExecutionGroupIds(groupIds, hidden, original)).toEqual(['robots']);
    expect(shownExecutionGroupIds(['all'], undefined, executionGroupScopes([tree], []))).toEqual(['all']);
  });
});

const leaf = (
  instanceId: string, nodeId: string, groups?: ProgramTreeNode['execution_groups'], target?: string,
): ProgramTreeNode => ({
  node_id: nodeId, instance_id: instanceId, parameters: target === undefined ? {} : { target_role: target },
  ...(groups === undefined ? {} : { execution_groups: groups }), children: [],
});

describe('hardware-acting node rule', () => {
  it('matches the built-in catalog: effectful robot, tool or I/O commands and Kernel controls', () => {
    const catalog = readFileSync(new URL(
      '../../../../manyforge_behavior/resources/node_catalog.yaml', import.meta.url), 'utf8');
    const derived = new Set<string>();
    for (const entry of catalog.split(/^- id: /m).slice(1)) {
      const id = entry.split('\n', 1)[0].trim();
      const owner = /^ {4}owner_api: (\S+)$/m.exec(entry)?.[1];
      const kernelControl = new Set([
        'kernel.stop', 'kernel.full_stop', 'kernel.reset', 'kernel.runtime_scaling']);
      if ((/^ {4}effectful: true$/m.test(entry)
          && (owner === 'kernel.operation' || owner === 'kernel.command_target'))
          || (owner !== undefined && kernelControl.has(owner))) derived.add(id);
    }
    expect([...HARDWARE_ACTING_NODE_IDS].sort()).toEqual([...derived].sort());
    expect(derived.has('gripper.check_status')).toBe(false);
    expect(derived.has('kernel.reset_operation')).toBe(true);
    expect(derived.has('kernel.full_stop')).toBe(true);
  });

  it('counts gripper and I/O commands and Kernel controls, not reads or waits', () => {
    const tree: ProgramTreeNode = {
      ...leaf('root', 'sequence', ['arm']),
      children: [
        leaf('move', 'motion.move_to_pose', undefined, 'arm'),
        leaf('grip', 'gripper.command_position', 'all', 'gripper'),
        leaf('out', 'io.set_output', 'all', 'io'),
        leaf('status', 'gripper.check_status', 'all', 'gripper'),
        leaf('input', 'io.wait_for_input', 'all', 'io'),
        leaf('reset', 'kernel.reset_operation', 'all', 'arm'),
      ],
    };
    const scopes = executionGroupScopes([tree], ['arm']);
    const all = scopes.find((scope) => scope.groupId === 'all')!;
    expect([...all.hardwareNodeIds].sort()).toEqual(['grip', 'move', 'out', 'reset']);
    expect(all.targetRoleIds).toEqual(['arm', 'gripper', 'io']);
    expect(all.motionTargetRoleIds).toEqual(['arm']);
    expect(executionGroupVisibilityUnavailableReason(scopes, ['arm'])).toContain('Show All');
    expect(executionGroupDisplay(['all', 'arm'], undefined, scopes))
      .toEqual({ preferred: ['arm'], shown: ['all', 'arm'], fallback: 'uncovered' });
    // Uncovered reads and waits do not force All.
    const reset = tree.children[5];
    tree.children = [tree.children[0], tree.children[3], tree.children[4]];
    expect(executionGroupDisplay(['all', 'arm'], undefined, executionGroupScopes([tree], ['arm'])).fallback)
      .toBeNull();
    // An uncovered Kernel control (here Reset) does.
    tree.children = [...tree.children, reset];
    expect(executionGroupDisplay(['all', 'arm'], undefined, executionGroupScopes([tree], ['arm'])).fallback)
      .toBe('uncovered');
  });

  it('sorts group IDs canonically by code point', () => {
    const scopes = executionGroupScopes([leaf('move', 'motion.move_to_pose', ['b', 'a_x', 'a.x'])],
      ['b', 'a_x', 'a.x']);
    expect(scopes.map((scope) => scope.groupId)).toEqual(['a.x', 'a_x', 'all', 'b']);
  });
});

describe('visibility preference', () => {
  const tree: ProgramTreeNode = {
    ...leaf('root', 'parallel'),
    children: [
      leaf('a', 'motion.move_to_pose', ['left']),
      leaf('b', 'motion.move_to_pose', ['right']),
      leaf('c', 'motion.move_to_pose', 'all'),
    ],
  };
  const groupIds = ['all', 'left', 'right'];
  const scopes = executionGroupScopes([tree], ['left', 'right']);

  it('changes only the requested group and never persists the automatic All', () => {
    const hidden = ['all', 'right'];
    expect(executionGroupDisplay(groupIds, hidden, scopes))
      .toEqual({ preferred: ['left'], shown: ['all', 'left'], fallback: 'uncovered' });
    expect(executionGroupVisibilityChange(groupIds, hidden, scopes, 'right', true))
      .toEqual({ hidden: ['all'] });
    expect(executionGroupVisibilityChange(groupIds, ['all'], scopes, 'left', false))
      .toEqual({ refused: expect.stringContaining('Show All') });
    expect(executionGroupVisibilityChange(groupIds, ['all'], scopes, 'all', true)).toEqual({ hidden: [] });
    expect(executionGroupVisibilityChange(groupIds, [], scopes, 'left', false)).toEqual({ hidden: ['left'] });
    expect(executionGroupVisibilityChange(groupIds, ['left'], scopes, 'all', false))
      .toEqual({ refused: expect.stringContaining('Show All') });
    expect(hidden).toEqual(['all', 'right']);
  });

  it('materializes the default without showing All when named groups cover every node', () => {
    const covered = executionGroupScopes([{ ...tree, children: tree.children.slice(0, 2) }], ['left', 'right']);
    expect(executionGroupDisplay(groupIds, undefined, covered).fallback).toBeNull();
    expect(executionGroupVisibilityChange(groupIds, undefined, covered, 'left', false))
      .toEqual({ refused: expect.stringContaining('Show All') });
    expect(executionGroupVisibilityChange(groupIds, undefined, covered, 'all', true)).toEqual({ hidden: [] });
  });

  it('distinguishes unavailable membership and an empty choice from uncovered nodes', () => {
    expect(executionGroupDisplay(groupIds, ['all'], []))
      .toEqual({ preferred: ['left', 'right'], shown: groupIds, fallback: 'membership_unavailable' });
    expect(executionGroupDisplay(groupIds, groupIds, scopes).fallback).toBe('empty');
    expect(executionGroupDisplay(groupIds, ['left', 'right'], []).fallback).toBeNull();
  });
});

describe('group target labels', () => {
  it('names robots only from a supplied binding, else the declared component role', () => {
    const applied = appliedProgram();
    const roles = { 'left-arm': { component_role: 'left-robot' }, 'right-arm': { component_role: 'right-robot' },
      loose: {} };
    expect(executionGroupTargetLabels(roles, null)).toEqual({
      'left-arm': 'left-arm (component role: left-robot)',
      'right-arm': 'right-arm (component role: right-robot)',
      loose: 'loose',
    });
    expect(executionGroupTargetLabels(roles, applied.programBinding)).toEqual({
      'left-arm': 'left-arm (left)', 'right-arm': 'right-arm (right)', loose: 'loose',
    });
  });
});
