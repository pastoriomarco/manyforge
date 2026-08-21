import { describe, expect, it } from 'vitest';
import {
  authoringMoveIntent,
  buildAuthoringTree,
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

  it('rejects reparent and delete before violating the old parent minimum', () => {
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
      intent: null,
      error: "Node 'Required parent' requires at least 1 child",
    });
    expect(validateAuthoringDeletion(
      cardinalNodes,
      cardinalEdges,
      'required',
    )).toBe("Node 'Required parent' requires at least 1 child");
  });

  it('rejects unknown, root, capacity, leaf, and multiparent connections', () => {
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
    )).toContain('Root node');
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

  it('builds one ordered, connected tree from canvas geometry', () => {
    expect(buildAuthoringTree(nodes, edges, 'vertical')).toEqual({
      tree: {
        id: 'root',
        children: [
          { id: 'left', children: [] },
          { id: 'right', children: [] },
        ],
      },
      error: null,
    });
  });

  it('rejects dangling, multi-parent, and disconnected graphs', () => {
    expect(
      buildAuthoringTree(nodes, [...edges, { source: 'missing', target: 'right' }], 'vertical').error,
    ).toContain('dangling');
    expect(
      buildAuthoringTree(nodes, [...edges, { source: 'left', target: 'right' }], 'vertical').error,
    ).toContain('multiple parents');
    expect(buildAuthoringTree(nodes, [], 'vertical').error).toContain('found 3');
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
});
