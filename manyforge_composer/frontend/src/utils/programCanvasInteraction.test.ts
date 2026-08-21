import { describe, expect, it } from 'vitest';

import type { CatalogEntry, ProgramTreeNode } from '../types/api';
import {
  authoringMoveIntent,
  type AuthoringGraphEdge,
  type AuthoringGraphNode,
} from './authoringGraph';
import {
  programCanvasKeyIntent,
  programDeleteMutation,
  programInsertionMutation,
  programMoveMutation,
  programPaletteClickIntent,
  programPaletteDragData,
  readProgramPaletteDrag,
  stageProgramCanvasDrop,
  validProgramInsertionPlacements,
  type ProgramInsertionNode,
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

const sequence: CatalogEntry = {
  ...leaf,
  id: 'sequence',
  name: 'Sequence',
  kind: 'composite',
  childrenMin: 1,
  childrenMax: -1,
};

const root: ProgramTreeNode = {
  node_id: 'sequence',
  instance_id: 'root',
  parameters: {},
  children: [{
    node_id: 'move',
    instance_id: 'child',
    parameters: {},
    children: [],
  }],
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

const insertionNode: ProgramInsertionNode = {
  node_id: 'move',
  instance_id: 'move_2',
  parameters: {},
  children: [],
};

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

  it('stages a drop without producing a Draft mutation', () => {
    const staged = stageProgramCanvasDrop(
      root,
      [sequence, leaf],
      'move',
      'root',
      7,
    );
    expect(staged).toEqual({
      request: {
        requestId: 7,
        catalogId: 'move',
        targetId: 'root',
        placement: 'child',
      },
      error: null,
    });
    expect(staged).not.toHaveProperty('mutation');
  });

  it('allows an unbounded non-leaf wrapper placement', () => {
    expect(validProgramInsertionPlacements(
      root,
      [sequence, leaf],
      'child',
      sequence,
    )).toContain('wrapper');
  });

  it('maps explicit insertion confirmation exactly once for each placement', () => {
    expect(programInsertionMutation('root', 'child', 1, insertionNode)).toEqual({
      mutation: 'tree_draft_insert_node',
      args: {
        parent_instance_id: 'root',
        index: 1,
        node: insertionNode,
      },
    });
    expect(programInsertionMutation('child', 'sibling', 0, insertionNode)).toEqual({
      mutation: 'tree_draft_insert_node',
      args: { after_instance_id: 'child', node: insertionNode },
    });
    expect(programInsertionMutation('child', 'wrapper', 0, insertionNode)).toEqual({
      mutation: 'tree_draft_wrap_node',
      args: {
        target_instance_id: 'child',
        wrapper: {
          node_id: 'move',
          instance_id: 'move_2',
          parameters: {},
        },
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
    expect(programDeleteMutation('leaf', false)).toEqual({
      mutation: 'tree_draft_delete_node',
      args: { instance_id: 'leaf' },
    });
    expect(programDeleteMutation('root', true)).toBeNull();
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
