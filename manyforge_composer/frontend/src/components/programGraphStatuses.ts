import type { NodePresentationStatus } from '../store/composerStore';
import type { CollapsedProgramItem } from '../utils/programCollapse';

type Statuses = Record<string, NodePresentationStatus>;
type NodeWithPresentation = {
  id: string;
  data: { presentation: { status: NodePresentationStatus; collapsedItems?: CollapsedProgramItem[] | null } };
};
type EdgeWithStatus = {
  target: string;
  data?: { targetStatus: NodePresentationStatus };
};

function itemStatuses(items: CollapsedProgramItem[], statuses: Statuses, result: NodePresentationStatus[]) {
  for (const item of items) {
    result.push(statuses[item.id] ?? 'idle');
    itemStatuses(item.children, statuses, result);
  }
}

function applyItemStatuses(items: CollapsedProgramItem[], statuses: Statuses): CollapsedProgramItem[] {
  return items.map((item) => ({
    ...item,
    status: statuses[item.id] ?? 'idle',
    children: applyItemStatuses(item.children, statuses),
  }));
}

// Each projector belongs to one stable graph. Its cache only reuses objects
// whose complete displayed status values match the current input.
export function projectNodeStatuses<T extends NodeWithPresentation>(base: T[]): (statuses: Statuses) => T[] {
  let previous: T[] = base;
  let previousStatuses = base.map((node) => {
    const values: NodePresentationStatus[] = ['idle'];
    itemStatuses(node.data.presentation.collapsedItems ?? [], {}, values);
    return values;
  });
  return (statuses) => {
    let changed = false;
    const nextStatuses: NodePresentationStatus[][] = [];
    const next = base.map((node, index) => {
      const values: NodePresentationStatus[] = [statuses[node.id] ?? 'idle'];
      const items = node.data.presentation.collapsedItems;
      itemStatuses(items ?? [], statuses, values);
      nextStatuses.push(values);
      if (values.every((value, i) => value === previousStatuses[index][i])) return previous[index];
      changed = true;
      return {
        ...node,
        data: {
          ...node.data,
          presentation: {
            ...node.data.presentation,
            status: values[0],
            collapsedItems: items ? applyItemStatuses(items, statuses) : items,
          },
        },
      } as T;
    });
    if (changed) previous = next;
    previousStatuses = nextStatuses;
    return previous;
  };
}

export function projectEdgeStatuses<T extends EdgeWithStatus>(base: T[]): (statuses: Statuses) => T[] {
  let previous: T[] = base;
  return (statuses) => {
    let changed = false;
    const next = base.map((edge, index) => {
      const status = statuses[edge.target] ?? 'idle';
      if (previous[index].data?.targetStatus === status) return previous[index];
      changed = true;
      return { ...edge, data: { ...edge.data, targetStatus: status } } as T;
    });
    if (changed) previous = next;
    return previous;
  };
}
