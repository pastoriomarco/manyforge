import { describe, expect, it } from 'vitest';

import type { ProgramTreeNode } from '../types/api';
import {
  collapsedProgramItems,
  collapsibleProgramNodeIds,
  hiddenProgramNodeIds,
  programDescendantCount,
} from './programCollapse';

function node(id: string, children: ProgramTreeNode[] = []): ProgramTreeNode {
  return { node_id: 'sequence', instance_id: id, parameters: {}, children } as ProgramTreeNode;
}

const tree = node('root', [
  node('a'),
  node('pick', [node('p1'), node('grip', [node('g1'), node('g2')])]),
]);
const detached = node('loose', [node('l1')]);

describe('collapsed Program subtrees', () => {
  it('hides every descendant of a collapsed node, at any depth', () => {
    expect([...hiddenProgramNodeIds([tree], new Set(['pick']))].sort())
      .toEqual(['g1', 'g2', 'grip', 'p1']);
  });

  it('hides the whole tree below a collapsed root and keeps other roots', () => {
    const hidden = hiddenProgramNodeIds([tree, detached], new Set(['root']));
    expect(hidden.has('root')).toBe(false);
    expect(hidden.has('g2')).toBe(true);
    expect(hidden.has('loose')).toBe(false);
    expect(hidden.has('l1')).toBe(false);
  });

  it('lists only nodes with children as collapsible, roots included', () => {
    expect([...collapsibleProgramNodeIds([tree, detached])].sort())
      .toEqual(['grip', 'loose', 'pick', 'root']);
  });

  it('nests the collapsed list like the tree and counts descendants', () => {
    const items = collapsedProgramItems(tree, (child) => ({
      id: child.instance_id,
      label: child.instance_id,
      catalogId: child.node_id,
      category: 'control_flow',
      status: child.instance_id === 'g1' ? 'running' : 'idle',
      invalid: false,
      selected: false,
    }));
    expect(items.map((item) => item.id)).toEqual(['a', 'pick']);
    expect(items[1].descendantCount).toBe(4);
    expect(items[1].children[1].children.map((item) => [item.id, item.status]))
      .toEqual([['g1', 'running'], ['g2', 'idle']]);
    expect(programDescendantCount(tree)).toBe(6);
  });
});
