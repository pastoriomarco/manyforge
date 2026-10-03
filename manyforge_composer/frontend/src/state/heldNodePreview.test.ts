import { describe, expect, it } from 'vitest';

import {
  heldAuthoring,
  heldCatalog,
  heldProgram,
} from '../testFixtures/heldRun';
import type {
  ProgramAuthoringDocument,
  ProgramBindingDocument,
  ProgramFormulaReferences,
  ProgramNodeCorrectionPreview,
} from '../types/api';
import type { DirectControlMotionStatus, MotionPreviewReceipt } from '../types/robotControl';
import {
  findProgramNode,
  heldNodeGhostSource,
  heldNodePreviewInputs,
} from './heldNodePreview';

const digest = (fill: string) => `sha256:${fill.repeat(64)}`;
const NO_FORMULAS: ProgramFormulaReferences = { computedKeys: {}, nodes: {} };
const FORMULAS: ProgramFormulaReferences = {
  computedKeys: { offset: ['param.approach_height'] },
  nodes: { move: { clearance: ['key.offset', 'param.approach_height'] } },
};

function edited(change: (draft: ProgramAuthoringDocument) => void): ProgramAuthoringDocument {
  const draft = heldAuthoring();
  change(draft);
  return draft;
}

function move(draft: ProgramAuthoringDocument) {
  const node = findProgramNode(draft.forest, 'move');
  if (node === null) throw new Error('move expected');
  return node;
}

const inputs = (
  draft: ProgramAuthoringDocument,
  formulas = NO_FORMULAS,
  captured = heldProgram(),
) => heldNodePreviewInputs(captured, draft, 'move', heldCatalog[0], formulas);

describe('held node preview inputs (Draft vs captured Program)', () => {
  it('previews nothing while the Draft gives the node its captured inputs', () => {
    expect(inputs(heldAuthoring())).toEqual({ kind: 'unchanged' });
    // Another node's edit is not this node's preview.
    expect(inputs(edited((draft) => {
      findProgramNode(draft.forest, 'settle')!.parameters = { extra: 1 };
    }))).toEqual({ kind: 'unchanged' });
  });

  it('replaces exactly the node fields the Draft changes', () => {
    expect(inputs(edited((draft) => { move(draft).parameters.speed = 0.5; })))
      .toEqual({ kind: 'inputs', inputs: [{ path: 'speed', value: 0.5 }] });
    const goal = { position_m: [0.3, 0, 0.4] };
    expect(inputs(edited((draft) => { move(draft).parameters.goal = goal; })))
      .toEqual({ kind: 'inputs', inputs: [{ path: 'goal', value: goal }] });
  });

  it('previews only the edited leaves beneath a source and refuses a shadowed literal', () => {
    const captured = heldProgram();
    const node = findProgramNode([captured.tree], 'move')!;
    node.parameters.goal = { position_m: [0.3, 0, 0.9], frame: 'base' };
    node.value_sources = {
      ...node.value_sources, 'goal.position_m': { kind: 'blackboard_key', reference: 'pick_pose' },
    };
    const draft = heldAuthoring(structuredClone(captured));
    move(draft).parameters.goal = { position_m: [0.3, 0, 0.9], frame: 'tool' };
    expect(inputs(draft, NO_FORMULAS, captured))
      .toEqual({ kind: 'inputs', inputs: [{ path: 'goal.frame', value: 'tool' }] });
    move(draft).parameters.goal = { position_m: [0.3, 0, 0.1], frame: 'base' };
    expect(inputs(draft, NO_FORMULAS, captured)).toMatchObject({
      kind: 'unavailable', reason: expect.stringContaining('goal.position_m comes from runtime key pick_pose'),
    });
  });

  it('replaces every input a changed Program parameter feeds, or says why it cannot', () => {
    const draft = edited((authoring) => { authoring.parameters![0].default = 0.25; });
    expect(inputs(draft)).toEqual({ kind: 'inputs', inputs: [{ path: 'approach', value: 0.25 }] });
    expect(inputs(draft, FORMULAS)).toMatchObject({
      kind: 'unavailable', reason: expect.stringContaining('through a formula or computed key'),
    });
  });

  it('reports a changed source, node type or a removed node as unavailable, never approximated', () => {
    expect(inputs(edited((draft) => {
      move(draft).value_sources = { ...move(draft).value_sources, speed: { kind: 'program_parameter', reference: 'approach_height' } };
    }))).toMatchObject({ kind: 'unavailable', reason: expect.stringContaining('where this node') });
    expect(inputs(edited((draft) => { move(draft).node_id = 'motion.move_to_joint_target'; })))
      .toMatchObject({ kind: 'unavailable', reason: expect.stringContaining('different node type') });
    expect(inputs(edited((draft) => { draft.forest[0].children.splice(0, 1); })))
      .toMatchObject({ kind: 'unavailable', reason: 'The Draft no longer has this node.' });
  });
});

describe('held node preview ghost', () => {
  const binding = {
    program_binding_id: digest('c'),
    program_revision: digest('a'),
    retained_bundle_id: 'bundle.1',
    graph_revision: digest('9'),
    target_role_bindings: [{
      target_role_id: 'arm', component_role_id: 'picker',
      controllable_component_id: 'lite6', command_target_id: 'lite6.arm',
    }],
  } as unknown as ProgramBindingDocument;
  const receipt = {
    programBindingId: digest('c'), programRevision: digest('a'),
    retainedBundleId: 'bundle.1', graphRevision: digest('9'),
    componentRoleId: 'picker', targetRoleId: 'arm',
    sourceJointNames: ['j1', 'j2'], finalPositionsRad: [0.1, 0.2],
  } as unknown as MotionPreviewReceipt;
  const result = {
    programBindingId: digest('c'), programRevision: digest('a'),
    target: {
      targetRoleId: 'arm', componentRoleId: 'picker',
      controllableComponentId: 'lite6', commandTargetId: 'lite6.arm',
    },
    unavailableReason: null,
    preview: { ok: true, receipt },
  } as unknown as ProgramNodeCorrectionPreview;
  // Manual control uses its own binding and role names for the same robot.
  const manual = [{
    programBindingId: digest('m'), componentRoleId: 'manual_arm', targetRoleId: 'manual',
    targetDescriptor: {
      controllableComponentId: 'lite6', commandTargetId: 'lite6.arm',
      componentRoleId: 'manual_arm', targetRoleId: 'manual',
      joints: [{ sourceJointName: 'j1' }, { sourceJointName: 'j2' }],
    },
  }] as unknown as DirectControlMotionStatus[];

  it('resolves a Program move through the Applied binding to the physical target model', () => {
    expect(heldNodeGhostSource(result, binding, manual)).toEqual({
      ghost: { receipt, descriptor: manual[0].targetDescriptor },
    });
    expect(heldNodeGhostSource(null, binding, manual)).toBeNull();
    expect(heldNodeGhostSource({ ...result, preview: null }, binding, manual)).toBeNull();
  });

  it('shows no ghost, with a reason, for a stale or foreign binding or target', () => {
    const reason = (value: ReturnType<typeof heldNodeGhostSource>) => (
      value !== null && 'reason' in value ? value.reason : null
    );
    expect(reason(heldNodeGhostSource(result, null, manual))).toContain('Applied');
    expect(reason(heldNodeGhostSource(
      result, { ...binding, program_binding_id: digest('d') }, manual,
    ))).toContain('Applied');
    expect(reason(heldNodeGhostSource(result, {
      ...binding,
      target_role_bindings: [{ ...binding.target_role_bindings[0], command_target_id: 'other.arm' }],
    }, manual))).toContain('does not bind picker/arm');
    expect(reason(heldNodeGhostSource(result, binding, []))).toContain('No robot model');
    expect(reason(heldNodeGhostSource(result, binding, [{
      ...manual[0],
      targetDescriptor: { ...manual[0].targetDescriptor, joints: [{ sourceJointName: 'j1' }] },
    }] as unknown as DirectControlMotionStatus[]))).toContain('joints');
  });
});
