import type { ProgramTreeNode } from '../types/api';

/**
 * Collapsed subtrees: pure view state of the Program canvas. A collapsed
 * composite keeps its place on the canvas and lists its whole subtree inside
 * itself; its descendants leave the canvas. Nothing here edits the Program.
 */

/** Nodes hidden because an ancestor is collapsed. */
export function hiddenProgramNodeIds(
  roots: readonly ProgramTreeNode[],
  collapsed: ReadonlySet<string>,
): Set<string> {
  const hidden = new Set<string>();
  const walk = (node: ProgramTreeNode, underCollapsed: boolean) => {
    if (underCollapsed) hidden.add(node.instance_id);
    const hideChildren = underCollapsed || collapsed.has(node.instance_id);
    node.children.forEach((child) => walk(child, hideChildren));
  };
  roots.forEach((root) => walk(root, false));
  return hidden;
}

/** Every node that can collapse: the ones with children, roots included. */
export function collapsibleProgramNodeIds(roots: readonly ProgramTreeNode[]): Set<string> {
  const ids = new Set<string>();
  const walk = (node: ProgramTreeNode) => {
    if (node.children.length > 0) ids.add(node.instance_id);
    node.children.forEach(walk);
  };
  roots.forEach(walk);
  return ids;
}

/** Number of nodes below `node`, at any depth. */
export function programDescendantCount(node: ProgramTreeNode): number {
  return node.children.reduce(
    (count, child) => count + 1 + programDescendantCount(child),
    0,
  );
}

/** One entry of a collapsed node's list; composites enclose their children. */
export interface CollapsedProgramItem {
  id: string;
  label: string;
  catalogId: string;
  category: string;
  status: 'idle' | 'running' | 'held' | 'success' | 'failure' | 'interrupted';
  invalid: boolean;
  selected: boolean;
  descendantCount: number;
  children: CollapsedProgramItem[];
}

export function collapsedProgramItems(
  node: ProgramTreeNode,
  describe: (node: ProgramTreeNode) => Omit<CollapsedProgramItem, 'children' | 'descendantCount'>,
): CollapsedProgramItem[] {
  return node.children.map((child) => ({
    ...describe(child),
    descendantCount: programDescendantCount(child),
    children: collapsedProgramItems(child, describe),
  }));
}
