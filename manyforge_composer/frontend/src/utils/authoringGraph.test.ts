import { describe, expect, it } from 'vitest';
import {
  buildAuthoringTree,
  layoutAuthoringGraph,
  validateAuthoringConnection,
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
