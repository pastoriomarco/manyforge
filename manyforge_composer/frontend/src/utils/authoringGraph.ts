import type { LayoutDirection } from '../store/composerStore';

const DEFAULT_NODE_WIDTH = 180;
const DEFAULT_NODE_HEIGHT = 64;
const PRIMARY_GAP: Record<LayoutDirection, number> = {
  vertical: 72,
  horizontal: 88,
};
const DEPTH_GAP: Record<LayoutDirection, number> = {
  vertical: 84,
  horizontal: 112,
};
const ROOT_LAYOUT_CENTER: Record<
  LayoutDirection,
  { primary: number; depth: number }
> = {
  vertical: { primary: 420, depth: 72 },
  horizontal: { primary: 320, depth: 124 },
};

export interface AuthoringGraphNode {
  id: string;
  label: string;
  position: { x: number; y: number };
  width?: number;
  height?: number;
  childrenMax?: number;
}

export function validateAuthoringConnection(
  nodes: AuthoringGraphNode[],
  edges: AuthoringGraphEdge[],
  sourceId: string | null | undefined,
  targetId: string | null | undefined,
): string | null {
  if (!sourceId || !targetId) {
    return 'Connections must include both a parent and a child node';
  }
  if (sourceId === targetId) return 'A node cannot be its own parent';
  const nodeMap = new Map(nodes.map((node) => [node.id, node]));
  const source = nodeMap.get(sourceId);
  const target = nodeMap.get(targetId);
  if (!source || !target) return 'Connection references an unknown node';
  if (edges.some((edge) => edge.source === sourceId && edge.target === targetId)) {
    return 'That link already exists';
  }
  if (edges.some((edge) => edge.target === targetId)) {
    return `Node '${target.label}' already has a parent`;
  }
  const childCount = edges.filter((edge) => edge.source === sourceId).length;
  if (source.childrenMax === 0) {
    return `Node '${source.label}' does not allow child nodes`;
  }
  if (
    source.childrenMax !== undefined
    && source.childrenMax >= 0
    && childCount >= source.childrenMax
  ) {
    return `Node '${source.label}' accepts at most ${source.childrenMax} child${source.childrenMax === 1 ? '' : 'ren'}`;
  }
  const adjacency = new Map<string, string[]>();
  for (const edge of edges) {
    adjacency.set(edge.source, [...(adjacency.get(edge.source) ?? []), edge.target]);
  }
  const stack = [targetId];
  const seen = new Set<string>();
  while (stack.length > 0) {
    const nodeId = stack.pop();
    if (nodeId === sourceId) return 'That connection would create a cycle';
    if (!nodeId || seen.has(nodeId)) continue;
    seen.add(nodeId);
    stack.push(...(adjacency.get(nodeId) ?? []));
  }
  return null;
}

export interface AuthoringGraphEdge {
  source: string;
  target: string;
}

export interface AuthoringTreeNode {
  id: string;
  children: AuthoringTreeNode[];
}

export type AuthoringTreeBuildResult =
  | { tree: AuthoringTreeNode; error: null }
  | { tree: null; error: string };

function nodeWidth(node: AuthoringGraphNode): number {
  return typeof node.width === 'number' && Number.isFinite(node.width)
    ? node.width
    : DEFAULT_NODE_WIDTH;
}

function nodeHeight(node: AuthoringGraphNode): number {
  return typeof node.height === 'number' && Number.isFinite(node.height)
    ? node.height
    : DEFAULT_NODE_HEIGHT;
}

function primarySize(node: AuthoringGraphNode, direction: LayoutDirection): number {
  return direction === 'vertical' ? nodeWidth(node) : nodeHeight(node);
}

function depthSize(node: AuthoringGraphNode, direction: LayoutDirection): number {
  return direction === 'vertical' ? nodeHeight(node) : nodeWidth(node);
}

function primaryCoordinate(
  node: AuthoringGraphNode,
  direction: LayoutDirection,
): number {
  return direction === 'vertical'
    ? node.position.x + nodeWidth(node) / 2
    : node.position.y + nodeHeight(node) / 2;
}

function positionFromCenters(
  primaryCenter: number,
  depthCenter: number,
  direction: LayoutDirection,
  node: AuthoringGraphNode,
): { x: number; y: number } {
  return direction === 'vertical'
    ? { x: primaryCenter - nodeWidth(node) / 2, y: depthCenter - nodeHeight(node) / 2 }
    : { x: depthCenter - nodeWidth(node) / 2, y: primaryCenter - nodeHeight(node) / 2 };
}

function graphRelations(
  nodes: AuthoringGraphNode[],
  edges: AuthoringGraphEdge[],
): {
  nodeMap: Map<string, AuthoringGraphNode>;
  childrenMap: Map<string, string[]>;
  parentMap: Map<string, string>;
  error: string | null;
} {
  const nodeMap = new Map<string, AuthoringGraphNode>();
  for (const node of nodes) {
    if (nodeMap.has(node.id)) {
      return {
        nodeMap,
        childrenMap: new Map(),
        parentMap: new Map(),
        error: `Draft tree contains duplicate node '${node.label}'`,
      };
    }
    nodeMap.set(node.id, node);
  }
  const childrenMap = new Map<string, string[]>();
  const parentMap = new Map<string, string>();
  for (const edge of edges) {
    if (!nodeMap.has(edge.source) || !nodeMap.has(edge.target)) {
      return { nodeMap, childrenMap, parentMap, error: 'Draft tree contains dangling edges' };
    }
    if (parentMap.has(edge.target)) {
      const label = nodeMap.get(edge.target)?.label ?? edge.target;
      return {
        nodeMap,
        childrenMap,
        parentMap,
        error: `Node '${label}' cannot have multiple parents`,
      };
    }
    parentMap.set(edge.target, edge.source);
    childrenMap.set(edge.source, [...(childrenMap.get(edge.source) ?? []), edge.target]);
  }
  return { nodeMap, childrenMap, parentMap, error: null };
}

/**
 * Validate and order the behavior-tree topology without serializing an old
 * Program DTO. I6 reconnects this tree identity to the qualified artifact.
 */
export function buildAuthoringTree(
  nodes: AuthoringGraphNode[],
  edges: AuthoringGraphEdge[],
  direction: LayoutDirection,
): AuthoringTreeBuildResult {
  if (nodes.length === 0) return { tree: null, error: 'Draft tree is empty' };
  const relations = graphRelations(nodes, edges);
  if (relations.error) return { tree: null, error: relations.error };
  const { nodeMap, childrenMap, parentMap } = relations;
  const roots = nodes.filter((node) => !parentMap.has(node.id));
  if (roots.length !== 1) {
    return {
      tree: null,
      error: `Draft tree must contain exactly one root node; found ${roots.length}`,
    };
  }

  const visited = new Set<string>();
  const visiting = new Set<string>();
  const build = (nodeId: string): AuthoringTreeNode => {
    const node = nodeMap.get(nodeId);
    if (!node) throw new Error('Draft tree contains dangling edges');
    if (visiting.has(nodeId)) throw new Error(`Cycle detected at node '${node.label}'`);
    if (visited.has(nodeId)) {
      throw new Error(`Node '${node.label}' is referenced more than once`);
    }
    visiting.add(nodeId);
    visited.add(nodeId);
    const children = [...(childrenMap.get(nodeId) ?? [])].sort((left, right) => {
      const leftNode = nodeMap.get(left);
      const rightNode = nodeMap.get(right);
      if (!leftNode || !rightNode) return 0;
      return primaryCoordinate(leftNode, direction) - primaryCoordinate(rightNode, direction);
    });
    const result = { id: node.id, children: children.map(build) };
    visiting.delete(nodeId);
    return result;
  };

  try {
    const tree = build(roots[0].id);
    if (visited.size !== nodes.length) {
      return {
        tree: null,
        error: 'Draft tree must be fully connected; disconnected nodes are present',
      };
    }
    return { tree, error: null };
  } catch (error) {
    return {
      tree: null,
      error: error instanceof Error ? error.message : 'Draft tree is invalid',
    };
  }
}

/** Deterministic subtree-span layout retained independently of React Flow. */
export function layoutAuthoringGraph(
  nodes: AuthoringGraphNode[],
  edges: AuthoringGraphEdge[],
  direction: LayoutDirection,
  orderingDirection: LayoutDirection = direction,
): AuthoringGraphNode[] {
  if (nodes.length === 0) return [];
  const relations = graphRelations(nodes, edges);
  if (relations.error) return nodes;
  const { nodeMap, childrenMap, parentMap } = relations;
  for (const children of childrenMap.values()) {
    children.sort((left, right) => {
      const leftNode = nodeMap.get(left);
      const rightNode = nodeMap.get(right);
      if (!leftNode || !rightNode) return 0;
      return primaryCoordinate(leftNode, orderingDirection)
        - primaryCoordinate(rightNode, orderingDirection);
    });
  }
  const root = nodes.find((node) => !parentMap.has(node.id));
  if (!root) return nodes;

  const spanCache = new Map<string, number>();
  const measureSpan = (nodeId: string): number => {
    const cached = spanCache.get(nodeId);
    if (cached !== undefined) return cached;
    const node = nodeMap.get(nodeId);
    if (!node) return 0;
    const children = childrenMap.get(nodeId) ?? [];
    const childrenSpan = children.reduce(
      (sum, childId, index) =>
        sum + measureSpan(childId) + (index > 0 ? PRIMARY_GAP[direction] : 0),
      0,
    );
    const span = Math.max(primarySize(node, direction), childrenSpan);
    spanCache.set(nodeId, span);
    return span;
  };
  measureSpan(root.id);

  const positions = new Map<string, { x: number; y: number }>();
  const rootCenter = ROOT_LAYOUT_CENTER[direction];
  const place = (nodeId: string, primaryCenter: number, depthCenter: number) => {
    const node = nodeMap.get(nodeId);
    if (!node) return;
    positions.set(
      nodeId,
      positionFromCenters(primaryCenter, depthCenter, direction, node),
    );
    const children = childrenMap.get(nodeId) ?? [];
    const totalSpan = children.reduce(
      (sum, childId, index) =>
        sum + measureSpan(childId) + (index > 0 ? PRIMARY_GAP[direction] : 0),
      0,
    );
    let cursor = primaryCenter - totalSpan / 2;
    for (const childId of children) {
      const child = nodeMap.get(childId);
      if (!child) continue;
      const childSpan = measureSpan(childId);
      const childPrimary = cursor + childSpan / 2;
      const childDepth = depthCenter
        + depthSize(node, direction) / 2
        + DEPTH_GAP[direction]
        + depthSize(child, direction) / 2;
      place(childId, childPrimary, childDepth);
      cursor += childSpan + PRIMARY_GAP[direction];
    }
  };
  place(root.id, rootCenter.primary, rootCenter.depth);

  return nodes.map((node) => ({
    ...node,
    position: positions.get(node.id) ?? node.position,
  }));
}
