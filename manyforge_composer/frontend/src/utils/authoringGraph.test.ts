import { describe, expect, it } from 'vitest';
import {
  authoringMoveIntent,
  authoringReorderedIds,
  authoringSiblingInsertionIndex,
  layoutAuthoringGraph,
  validateAuthoringConnection,
  validateAuthoringDeletion,
  type AuthoringGraphNode,
} from './authoringGraph';

const nodes: AuthoringGraphNode[] = [
  { id: 'root', label: 'Root', position: { x: 100, y: 0 } },
  { id: 'left', label: 'Left', position: { x: 0, y: 100 } },
  { id: 'right', label: 'Right', position: { x: 200, y: 100 } },
];
const edges = [
  { source: 'root', target: 'left' },
  { source: 'root', target: 'right' },
];

describe('authoring graph mechanics', () => {
  it('prevents invalid interactive connections before graph mutation', () => {
    const bounded = nodes.map((node) => ({
      ...node,
      childrenMax: node.id === 'left' ? 0 : node.id === 'root' ? 2 : undefined,
    }));
    expect(validateAuthoringConnection(bounded, edges, 'left', 'root')).toContain(
      'does not allow child',
    );
    expect(validateAuthoringConnection(bounded, edges, 'right', 'root')).toContain(
      'create a cycle',
    );
    expect(validateAuthoringConnection(bounded, edges, 'root', 'right')).toContain(
      'already exists',
    );
  });

  it('validates reparenting as one atomic owner move', () => {
    const reparentNodes: AuthoringGraphNode[] = [
      { id: 'root', label: 'Root', position: { x: 0, y: 0 }, childrenMax: 3 },
      { id: 'left', label: 'Left', position: { x: 0, y: 100 }, childrenMax: 0 },
      { id: 'right', label: 'Right', position: { x: 200, y: 100 }, childrenMax: 2 },
      { id: 'leaf', label: 'Leaf', position: { x: 200, y: 200 }, childrenMax: 0 },
    ];
    const reparentEdges = [
      { source: 'root', target: 'left' },
      { source: 'root', target: 'right' },
      { source: 'right', target: 'leaf' },
    ];
    expect(authoringMoveIntent(
      reparentNodes,
      reparentEdges,
      'root',
      'leaf',
    )).toEqual({
      intent: {
        instance_id: 'leaf',
        new_parent_instance_id: 'root',
        index: 2,
      },
      error: null,
    });
    expect(validateAuthoringConnection(
      reparentNodes,
      reparentEdges,
      'root',
      'right',
    )).toContain('already exists');
  });

  it('maps either displayed ordering axis to one explicit sibling slot', () => {
    const orderedNodes: AuthoringGraphNode[] = [
      { id: 'root', label: 'Root', position: { x: 0, y: 100 } },
      { id: 'first', label: 'First', position: { x: 200, y: 0 } },
      { id: 'middle', label: 'Middle', position: { x: 200, y: 100 } },
      { id: 'last', label: 'Last', position: { x: 200, y: 200 } },
      { id: 'detached', label: 'Detached', position: { x: 200, y: 50 } },
    ];
    const orderedEdges = [
      { source: 'root', target: 'first' },
      { source: 'root', target: 'middle' },
      { source: 'root', target: 'last' },
    ];

    expect(authoringSiblingInsertionIndex(
      orderedNodes.map((node) => node.id === 'last'
        ? { ...node, position: { x: 200, y: 25 } }
        : node),
      orderedEdges,
      'root',
      'last',
      'horizontal',
    )).toBe(1);
    expect(authoringSiblingInsertionIndex(
      orderedNodes,
      orderedEdges,
      'root',
      'detached',
      'horizontal',
    )).toBe(1);

    const verticalNodes = orderedNodes.map((node) => ({
      ...node,
      position: { x: node.position.y, y: node.position.x },
    }));
    expect(authoringSiblingInsertionIndex(
      verticalNodes,
      orderedEdges,
      'root',
      'detached',
      'vertical',
    )).toBe(1);
  });

  it('projects button sibling order without moving unrelated nodes', () => {
    expect(authoringReorderedIds([
      { id: 'root', parentId: null },
      { id: 'first', parentId: 'root' },
      { id: 'nested', parentId: 'first' },
      { id: 'middle', parentId: 'root' },
      { id: 'last', parentId: 'root' },
    ], 'root', 'last', 1)).toEqual([
      'root', 'first', 'nested', 'last', 'middle',
    ]);
  });

  it('allows temporarily incomplete parents while retaining a valid forest', () => {
    const cardinalNodes: AuthoringGraphNode[] = [
      { id: 'root', label: 'Root', position: { x: 0, y: 0 }, childrenMin: 1, childrenMax: -1 },
      { id: 'required-parent', label: 'Required parent', position: { x: 0, y: 100 }, childrenMin: 1, childrenMax: -1 },
      { id: 'other', label: 'Other', position: { x: 200, y: 100 }, childrenMin: 0, childrenMax: -1 },
      { id: 'required', label: 'Required', position: { x: 0, y: 200 }, childrenMax: 0 },
    ];
    const cardinalEdges = [
      { source: 'root', target: 'required-parent' },
      { source: 'root', target: 'other' },
      { source: 'required-parent', target: 'required' },
    ];

    expect(authoringMoveIntent(
      cardinalNodes,
      cardinalEdges,
      'other',
      'required',
    )).toEqual({
      intent: {
        instance_id: 'required',
        new_parent_instance_id: 'other',
        index: 0,
      },
      error: null,
    });
    expect(validateAuthoringDeletion(
      cardinalNodes,
      'required',
    )).toBeNull();
  });

  it('rejects unknown, capacity, leaf, and multiparent connections', () => {
    const bounded = nodes.map((node) => ({
      ...node,
      childrenMax: node.id === 'root' ? 2 : node.id === 'left' ? 0 : 1,
    }));
    expect(validateAuthoringConnection(bounded, edges, 'missing', 'left'))
      .toContain('unknown');
    expect(validateAuthoringConnection(bounded, edges, 'left', 'root'))
      .toContain('does not allow child');
    expect(validateAuthoringConnection(
      [{ id: 'root', label: 'Root', position: { x: 0, y: 0 }, childrenMax: 2 },
        { id: 'other', label: 'Other', position: { x: 0, y: 100 }, childrenMax: 1 }],
      [],
      'other',
      'root',
    )).toBeNull();
    expect(validateAuthoringConnection(
      [...bounded, { id: 'new', label: 'New', position: { x: 400, y: 100 } }],
      edges,
      'root',
      'new',
    ))
      .toContain('at most');
    expect(validateAuthoringConnection(
      [...bounded, { id: 'other', label: 'Other', position: { x: 500, y: 100 } }],
      [...edges, { source: 'right', target: 'left' }],
      'other',
      'left',
    )).toContain('multiple parents');
  });

  it('lays out children without overlap in either direction', () => {
    const vertical = layoutAuthoringGraph(nodes, edges, 'vertical');
    const left = vertical.find((node) => node.id === 'left');
    const right = vertical.find((node) => node.id === 'right');
    expect(left?.position.x).toBeLessThan(right?.position.x ?? 0);
    expect(left?.position.y).toBeGreaterThan(
      vertical.find((node) => node.id === 'root')?.position.y ?? Infinity,
    );

    const horizontal = layoutAuthoringGraph(nodes, edges, 'horizontal');
    expect(horizontal.find((node) => node.id === 'left')?.position.x).toBeGreaterThan(
      horizontal.find((node) => node.id === 'root')?.position.x ?? Infinity,
    );
  });

  it('uses measured node bounds on both layout axes', () => {
    const measured: AuthoringGraphNode[] = [
      { id: 'root', label: 'Root', width: 250, height: 96, position: { x: 0, y: 0 } },
      { id: 'left', label: 'Left', width: 140, height: 70, position: { x: 0, y: 100 } },
      { id: 'right', label: 'Right', width: 310, height: 120, position: { x: 300, y: 100 } },
    ];
    for (const direction of ['vertical', 'horizontal'] as const) {
      const layout = layoutAuthoringGraph(measured, edges, direction);
      const left = layout.find((node) => node.id === 'left')!;
      const right = layout.find((node) => node.id === 'right')!;
      if (direction === 'vertical') {
        expect(left.position.x + (left.width ?? 0)).toBeLessThan(right.position.x);
      } else {
        expect(left.position.y + (left.height ?? 0)).toBeLessThan(right.position.y);
      }
    }
  });
});
