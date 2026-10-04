import { describe, expect, it } from 'vitest';

import type { CatalogEntry, ProgramTreeNode } from '../types/api';
import {
  authoringMoveIntent,
  type AuthoringGraphEdge,
  type AuthoringGraphNode,
} from './authoringGraph';
import {
  programCanvasKeyIntent,
  programChangeKindMutation,
  programCopyInstanceId,
  programSubtreeCopy,
  programSubtreeInsertionMutation,
  programUnwrapMutation,
  programDeleteMutation,
  programDetachedInsertionMutation,
  programInsertionMutation,
  programMoveMutation,
  programPaletteClickIntent,
  programPaletteDragData,
  programPaletteDropPosition,
  programWrapMutation,
  readProgramPaletteDrag,
} from './programCanvasInteraction';

const leaf: CatalogEntry = {
  id: 'move',
  name: 'Move',
  category: 'motion',
  description: '',
  stability: 'stable',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'program' },
  runners: { py_trees: 'move_v1' },
  tags: [],
  minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const graphNodes: AuthoringGraphNode[] = [
  { id: 'root', label: 'Root', position: { x: 0, y: 0 }, childrenMax: 3 },
  { id: 'left', label: 'Left', position: { x: 0, y: 100 }, childrenMax: 0 },
  { id: 'right', label: 'Right', position: { x: 200, y: 100 }, childrenMax: 2 },
  { id: 'leaf', label: 'Leaf', position: { x: 200, y: 200 }, childrenMax: 0 },
];

const graphEdges: AuthoringGraphEdge[] = [
  { source: 'root', target: 'left' },
  { source: 'root', target: 'right' },
  { source: 'right', target: 'leaf' },
];

describe('Program canvas interaction intents', () => {
  it('maps one native palette activation and preserves drag transfer metadata', () => {
    expect(programPaletteClickIntent(leaf)).toEqual({ catalogId: 'move' });
    expect(programPaletteDragData(leaf)).toEqual({
      'application/manyforge-node': 'move',
      'text/plain': 'move',
    });
    expect(readProgramPaletteDrag((format) => (
      format === 'text/plain' ? 'move' : ''
    ))).toBe('move');
  });

  it('maps palette activation or free-space drop to one detached-node mutation', () => {
    expect(programDetachedInsertionMutation(leaf, ['move'], 2)).toEqual({
      mutation: 'tree_draft_insert_node',
      args: {
        index: 2,
        node: {
          node_id: 'move',
          instance_id: 'move_2',
          parameters: {},
          children: [],
        },
      },
    });
    expect(programDetachedInsertionMutation(
      leaf,
      [],
      0,
      { x: 12, y: 34 },
    ).args.position).toEqual({ x: 12, y: 34 });
  });

  it('centres a palette drop on the pointer, where the drag preview was', () => {
    expect(programPaletteDropPosition({ x: 300, y: 200 }, { width: 200, height: 70 }))
      .toEqual({ x: 200, y: 165 });
  });

  it('maps explicit child, sibling, wrapper, and compatible-kind choices atomically', () => {
    expect(programInsertionMutation(leaf, [], {
      kind: 'child', parentInstanceId: 'sequence', index: 2,
    }).args).toMatchObject({
      parent_instance_id: 'sequence',
      index: 2,
      node: { node_id: 'move', instance_id: 'move' },
    });
    expect(programInsertionMutation(leaf, ['move'], {
      kind: 'after', siblingInstanceId: 'wait',
    }).args).toMatchObject({
      after_instance_id: 'wait',
      node: { node_id: 'move', instance_id: 'move_2' },
    });
    expect(programWrapMutation({ ...leaf, id: 'retry' }, [], 'move')).toEqual({
      mutation: 'tree_draft_wrap_node',
      args: {
        target_instance_id: 'move',
        wrapper: {
          node_id: 'retry', instance_id: 'retry', parameters: {},
        },
      },
    });
    expect(programChangeKindMutation(leaf, 'wait', ['duration_s'])).toEqual({
      mutation: 'tree_draft_change_node_kind',
      args: {
        instance_id: 'wait',
        replacement: { node_id: 'move', parameters: {} },
        parameters_mode: 'strict',
      },
    });
  });

  it('maps a valid connection to one exact atomic move', () => {
    const result = authoringMoveIntent(
      graphNodes,
      graphEdges,
      'root',
      'leaf',
    );
    expect(result.error).toBeNull();
    expect(result.intent).not.toBeNull();
    const intent = result.intent;
    if (!intent) return;
    expect(programMoveMutation(
      intent.instance_id,
      intent.new_parent_instance_id,
      intent.index,
    )).toEqual({
      mutation: 'tree_draft_move_node',
      args: {
        instance_id: 'leaf',
        new_parent_instance_id: 'root',
        index: 2,
      },
    });
  });

  it('maps invalid connections zero times', () => {
    const result = authoringMoveIntent(
      graphNodes,
      graphEdges,
      'left',
      'root',
    );
    const mutations = result.intent ? [programMoveMutation(
      result.intent.instance_id,
      result.intent.new_parent_instance_id,
      result.intent.index,
    )] : [];
    expect(result.error).toContain('does not allow child');
    expect(mutations).toHaveLength(0);
  });

  it('maps reorder, reparent, and delete to exact owner arguments', () => {
    expect(programMoveMutation('leaf', 'right', 0)).toEqual({
      mutation: 'tree_draft_move_node',
      args: {
        instance_id: 'leaf',
        new_parent_instance_id: 'right',
        index: 0,
      },
    });
    expect(programMoveMutation('leaf', 'root', 2)).toEqual({
      mutation: 'tree_draft_move_node',
      args: {
        instance_id: 'leaf',
        new_parent_instance_id: 'root',
        index: 2,
      },
    });
    expect(programMoveMutation('leaf', null, 2, {
      root: { x: 20, y: 40 },
      leaf: { x: 80, y: 120 },
    })).toEqual({
      mutation: 'tree_draft_move_node',
      args: {
        instance_id: 'leaf',
        new_parent_instance_id: null,
        index: 2,
        positions: {
          root: { x: 20, y: 40 },
          leaf: { x: 80, y: 120 },
        },
      },
    });
    expect(programDeleteMutation('leaf')).toEqual({
      mutation: 'tree_draft_delete_node',
      args: { instance_id: 'leaf' },
    });
    expect(programDeleteMutation('root')).toEqual({
      mutation: 'tree_draft_delete_node',
      args: { instance_id: 'root' },
    });
  });

  it('ignores destructive keyboard commands in form controls', () => {
    expect(programCanvasKeyIntent('Delete', 'form')).toBeNull();
    expect(programCanvasKeyIntent('Backspace', 'form')).toBeNull();
    expect(programCanvasKeyIntent('Delete', 'node')).toEqual({ type: 'delete' });
    expect(programCanvasKeyIntent('ArrowDown', 'node')).toEqual({
      type: 'navigate',
      key: 'ArrowDown',
    });
  });

});

describe('Program canvas subtree edits', () => {
  const subtree: ProgramTreeNode = {
    node_id: 'retry',
    instance_id: 'retry_pick',
    parameters: { num_attempts: 3 },
    children: [{
      node_id: 'move',
      instance_id: 'pick_approach',
      parameters: { speed: 0.2 },
      value_sources: { target: { kind: 'program_parameter', reference: 'pick_pose' } },
      children: [],
    }],
  };

  it('names a copy after the original', () => {
    expect(programCopyInstanceId('pick_approach', new Set())).toBe('pick_approach_2');
    expect(programCopyInstanceId('pick_approach_2', new Set(['pick_approach_2'])))
      .toBe('pick_approach_3');
    expect(programCopyInstanceId('moveToPose', new Set(['moveToPose_2']))).toBe('moveToPose_3');
  });

  it('copies a whole subtree with fresh ids and unchanged content', () => {
    const copy = programSubtreeCopy(subtree, ['retry_pick', 'pick_approach']);
    expect(copy.instance_id).toBe('retry_pick_2');
    expect(copy.children[0]?.instance_id).toBe('pick_approach_2');
    expect(copy.parameters).toEqual({ num_attempts: 3 });
    expect(copy.children[0]?.value_sources).toEqual(subtree.children[0]?.value_sources);
    copy.children[0]!.parameters.speed = 1;
    expect(subtree.children[0]?.parameters.speed).toBe(0.2);
  });

  it('inserts a subtree in one edit at the chosen place', () => {
    const copy = programSubtreeCopy(subtree, ['retry_pick', 'pick_approach']);
    expect(programSubtreeInsertionMutation(copy, { kind: 'after', siblingInstanceId: 'retry_pick' }))
      .toEqual({
        mutation: 'tree_draft_insert_node',
        args: { after_instance_id: 'retry_pick', node: copy },
      });
    expect(programSubtreeInsertionMutation(copy, { kind: 'root', index: 2, position: { x: 10, y: 20 } }).args)
      .toEqual({ index: 2, position: { x: 10, y: 20 }, node: copy });
  });

  it('unwraps a single-child node by replacing it with its child', () => {
    expect(programUnwrapMutation(subtree)).toEqual({
      mutation: 'tree_draft_replace_subtree',
      args: { instance_id: 'retry_pick', subtree: subtree.children[0] },
    });
    expect(() => programUnwrapMutation(subtree.children[0]!)).toThrow();
  });
});
