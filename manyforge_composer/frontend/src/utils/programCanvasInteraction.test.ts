import { describe, expect, it } from 'vitest';

import type { CatalogEntry } from '../types/api';
import {
  authoringMoveIntent,
  type AuthoringGraphEdge,
  type AuthoringGraphNode,
} from './authoringGraph';
import {
  programCanvasKeyIntent,
  programChangeKindMutation,
  programDeleteMutation,
  programDetachedInsertionMutation,
  programInsertionMutation,
  programMoveMutation,
  programPaletteClickIntent,
  programPaletteDragData,
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
