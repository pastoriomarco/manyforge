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
  childrenMin?: number;
  childrenMax?: number;
}

interface AuthoringGraphPositionNode {
  id: string;
  position: { x: number; y: number };
  width?: number;
  height?: number;
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
  const targetParents = edges.filter((edge) => edge.target === targetId);
  if (targetParents.length > 1) {
    return `Node '${target.label}' already has multiple parents`;
  }
  if (targetParents.length === 1 && targetParents[0]?.source === sourceId) {
    return 'That link already exists';
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

export function validateAuthoringDeletion(
  nodes: AuthoringGraphNode[],
  targetId: string | null | undefined,
): string | null {
  if (!targetId) return 'Deletion must name a tree node';
  const nodeMap = new Map(nodes.map((node) => [node.id, node]));
  const target = nodeMap.get(targetId);
  if (!target) return 'Deletion references an unknown node';
  return null;
}

export interface AuthoringMoveIntent {
  instance_id: string;
  new_parent_instance_id: string | null;
  index: number;
}

export type AuthoringMoveResult =
  | { intent: AuthoringMoveIntent; error: null }
  | { intent: null; error: string };

/**
 * Convert one validated graph connection into the sole owner mutation used
 * for both reparenting and newly connected presentation links. The graph is
 * never changed locally: callers submit this intent and wait for the fenced
 * Draft response.
 */
export function authoringMoveIntent(
  nodes: AuthoringGraphNode[],
  edges: AuthoringGraphEdge[],
  sourceId: string | null | undefined,
  targetId: string | null | undefined,
): AuthoringMoveResult {
  const error = validateAuthoringConnection(nodes, edges, sourceId, targetId);
  if (error || !sourceId || !targetId) {
    return { intent: null, error: error ?? 'Connections must include both a parent and a child node' };
  }
  return {
    intent: {
      instance_id: targetId,
      new_parent_instance_id: sourceId,
      index: edges.filter((edge) => edge.source === sourceId).length,
    },
    error: null,
  };
}

export interface AuthoringGraphEdge {
  source: string;
  target: string;
}

function nodeWidth(node: AuthoringGraphPositionNode): number {
  return typeof node.width === 'number' && Number.isFinite(node.width)
    ? node.width
    : DEFAULT_NODE_WIDTH;
}

function nodeHeight(node: AuthoringGraphPositionNode): number {
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
  node: AuthoringGraphPositionNode,
  direction: LayoutDirection,
): number {
  return direction === 'vertical'
    ? node.position.x + nodeWidth(node) / 2
    : node.position.y + nodeHeight(node) / 2;
}

/**
 * Map one displayed subtree root to its insertion slot among a parent's
 * children. Coordinates remain presentation metadata: the caller uses this
 * result to issue one explicit, revision-fenced owner reorder on gesture end.
 */
export function authoringSiblingInsertionIndex(
  nodes: AuthoringGraphPositionNode[],
  edges: AuthoringGraphEdge[],
  parentId: string,
  targetId: string,
  orderingDirection: LayoutDirection,
): number | null {
  const nodeMap = new Map(nodes.map((node) => [node.id, node]));
  const target = nodeMap.get(targetId);
  if (!nodeMap.has(parentId) || !target) return null;
  const siblings: Array<{ node: AuthoringGraphPositionNode; order: number }> = [];
  for (const [order, edge] of edges.entries()) {
    if (edge.source !== parentId || edge.target === targetId) continue;
    const sibling = nodeMap.get(edge.target);
    if (!sibling) return null;
    siblings.push({ node: sibling, order });
  }
  siblings.sort((left, right) => {
    const difference = primaryCoordinate(
      left.node,
      orderingDirection,
    ) - primaryCoordinate(
      right.node,
      orderingDirection,
    );
    return difference || left.order - right.order;
  });
  const targetCoordinate = primaryCoordinate(target, orderingDirection);
  const index = siblings.findIndex(({ node }) => targetCoordinate <= primaryCoordinate(
    node,
    orderingDirection,
  ));
  return index < 0 ? siblings.length : index;
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
  const roots = nodes.filter((node) => !parentMap.has(node.id));
  if (roots.length === 0) return nodes;

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
  roots.forEach((root) => measureSpan(root.id));

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
  const forestSpan = roots.reduce(
    (sum, root, index) => sum + measureSpan(root.id) + (index > 0 ? PRIMARY_GAP[direction] : 0),
    0,
  );
  let cursor = rootCenter.primary - forestSpan / 2;
  for (const root of roots) {
    const span = measureSpan(root.id);
    place(root.id, cursor + span / 2, rootCenter.depth);
    cursor += span + PRIMARY_GAP[direction];
  }

  return nodes.map((node) => ({
    ...node,
    position: positions.get(node.id) ?? node.position,
  }));
}
