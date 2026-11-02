import { describe, expect, it } from 'vitest';
import type { NodePresentationStatus } from '../store/composerStore';
import type { CollapsedProgramItem } from '../utils/programCollapse';
import { projectEdgeStatuses, projectNodeStatuses } from './programGraphStatuses';

const item = (id: string, children: CollapsedProgramItem[] = []): CollapsedProgramItem => ({
  id, label: id, catalogId: 'timer', category: 'action', status: 'idle',
  invalid: false, selected: false, descendantCount: children.length, children,
});

const node = (id: string, collapsedItems: CollapsedProgramItem[] | null = null) => ({
  id,
  position: { x: 0, y: 0 },
  data: { presentation: { status: 'idle' as NodePresentationStatus, collapsedItems }, onSelect: () => id },
});

const edge = (target: string) => ({
  target, source: 'root', data: { targetStatus: 'idle' as NodePresentationStatus, edgeStyle: 'step' },
});

describe('program graph status projection', () => {
  it('retains unchanged nodes and edges, including the arrays for equivalent status maps', () => {
    const nodes = [node('root'), node('first'), node('second')];
    const edges = [edge('first'), edge('second')];
    const projectNodes = projectNodeStatuses(nodes);
    const projectEdges = projectEdgeStatuses(edges);
    const initialNodes = projectNodes({});
    const initialEdges = projectEdges({});
    const updatedNodes = projectNodes({ first: 'running' });
    const updatedEdges = projectEdges({ first: 'running' });

    expect(updatedNodes[0]).toBe(initialNodes[0]);
    expect(updatedNodes[1]).not.toBe(initialNodes[1]);
    expect(updatedNodes[1].data.presentation.status).toBe('running');
    expect(updatedNodes[1].data.onSelect).toBe(initialNodes[1].data.onSelect);
    expect(updatedNodes[2]).toBe(initialNodes[2]);
    expect(updatedEdges[0]).not.toBe(initialEdges[0]);
    expect(updatedEdges[0].data.targetStatus).toBe('running');
    expect(updatedEdges[1]).toBe(initialEdges[1]);
    expect(projectNodes({ first: 'running', absent: 'success' })).toBe(updatedNodes);
    expect(projectEdges({ first: 'running', absent: 'success' })).toBe(updatedEdges);
    expect(projectNodes({})[1].data.presentation.status).toBe('idle');
    expect(projectEdges({})[0].data.targetStatus).toBe('idle');
  });

  it('updates the visible collapsed ancestor when a hidden descendant changes', () => {
    const project = projectNodeStatuses([node('root', [item('child', [item('grandchild')])]), node('other')]);
    const initial = project({});
    const updated = project({ grandchild: 'failure' });
    expect(updated[0]).not.toBe(initial[0]);
    expect(updated[1]).toBe(initial[1]);
    expect(updated[0].data.presentation.collapsedItems?.[0].children[0].status).toBe('failure');
    expect(initial[0].data.presentation.collapsedItems?.[0].children[0].status).toBe('idle');
    expect(project({ grandchild: 'failure' })).toBe(updated);
  });

  it('takes other presentation changes from a newly constructed base graph', () => {
    const first = node('first');
    const changed = { ...first, position: { x: 40, y: 50 }, data: { ...first.data, onSelect: () => 'new' } };
    const original = projectNodeStatuses([first])({ first: 'running' });
    const next = projectNodeStatuses([changed])({ first: 'running' });
    expect(next[0]).not.toBe(original[0]);
    expect(next[0].position).toEqual({ x: 40, y: 50 });
    expect(next[0].data.onSelect()).toBe('new');
    expect(next[0].data.presentation.status).toBe('running');
  });
});
