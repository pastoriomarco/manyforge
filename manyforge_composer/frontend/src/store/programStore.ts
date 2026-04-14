import { create } from 'zustand';
import { api } from '../api/client';
import type { TreeNode, ProgramInfo, RuntimeOverrideStatus } from '../types/api';
import { Position } from '@xyflow/react';
import type { Node, Edge } from '@xyflow/react';

export type LayoutDirection = 'vertical' | 'horizontal';

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
const ROOT_LAYOUT_CENTER: Record<LayoutDirection, { primary: number; depth: number }> = {
  vertical: { primary: 420, depth: 72 },
  horizontal: { primary: 320, depth: 124 },
};

interface ProgramStore {
  programInfo: ProgramInfo | null;
  nodes: Node[];
  edges: Edge[];
  appliedTree: TreeNode | null;
  savedTree: TreeNode | null;
  layoutDirection: LayoutDirection;
  layoutRevision: number;
  selectedNodeId: string | null;
  runtimeOverrideStatus: RuntimeOverrideStatus | null;
  loading: boolean;
  error: string | null;
  isDirty: boolean;
  
  loadProgram: (path: string, deploymentPath?: string) => Promise<void>;
  fetchProgramInfo: () => Promise<boolean>;
  fetchTree: () => Promise<boolean>;
  fetchRuntimeOverrideStatus: () => Promise<RuntimeOverrideStatus | null>;
  hydrateLoadedProgram: (options?: { retries?: number; retryDelayMs?: number }) => Promise<boolean>;
  setNodes: (
    nodes: Node[] | ((current: Node[]) => Node[]),
    options?: { dirty?: boolean },
  ) => void;
  setEdges: (
    edges: Edge[] | ((current: Edge[]) => Edge[]),
    options?: { dirty?: boolean },
  ) => void;
  addNode: (catalogId: string, position: { x: number; y: number }) => void;
  removeNode: (nodeId: string) => void;
  removeEdge: (edgeId: string) => void;
  disconnectNode: (nodeId: string) => void;
  selectNode: (id: string | null) => void;
  updateNodeParams: (nodeId: string, params: Record<string, unknown>) => void;
  updateNodeLabel: (nodeId: string, label: string) => void;
  flowToTree: () => TreeNode | null;
  setLayoutDirection: (direction: LayoutDirection) => void;
  autoLayout: () => void;
  discardToApplied: () => void;
  setAppliedToDraft: () => void;
  setSavedToDraft: () => void;
  activateRuntimeOverrides: () => Promise<void>;
  clearRuntimeOverrides: () => Promise<void>;
  saveProgram: (path: string) => Promise<void>;
}

function getNodeWidth(node: Node): number {
  return typeof node.width === 'number' && Number.isFinite(node.width)
    ? node.width
    : DEFAULT_NODE_WIDTH;
}

function getNodeHeight(node: Node): number {
  return typeof node.height === 'number' && Number.isFinite(node.height)
    ? node.height
    : DEFAULT_NODE_HEIGHT;
}

function getPrimarySize(node: Node, direction: LayoutDirection): number {
  return direction === 'vertical' ? getNodeWidth(node) : getNodeHeight(node);
}

function getDepthSize(node: Node, direction: LayoutDirection): number {
  return direction === 'vertical' ? getNodeHeight(node) : getNodeWidth(node);
}

function getPrimaryCoordinate(node: Node, direction: LayoutDirection): number {
  return direction === 'vertical'
    ? node.position.x + getNodeWidth(node) / 2
    : node.position.y + getNodeHeight(node) / 2;
}

function getNodeHandlePositions(direction: LayoutDirection): Pick<Node, 'sourcePosition' | 'targetPosition'> {
  return direction === 'vertical'
    ? {
        sourcePosition: Position.Bottom,
        targetPosition: Position.Top,
      }
    : {
        sourcePosition: Position.Right,
        targetPosition: Position.Left,
      };
}

function makePosition(
  primaryCenter: number,
  depthCenter: number,
  direction: LayoutDirection,
  node: Node,
): { x: number; y: number } {
  const width = getNodeWidth(node);
  const height = getNodeHeight(node);
  return direction === 'vertical'
    ? { x: primaryCenter - width / 2, y: depthCenter - height / 2 }
    : { x: depthCenter - width / 2, y: primaryCenter - height / 2 };
}

function collectTreeToFlow(
  node: TreeNode,
  parentId: string | null,
  nodes: Node[],
  edges: Edge[],
) {
  const flowId = `node-${nodes.length}`;
  
  nodes.push({
    id: flowId,
    type: 'btNode',
    position: { x: 0, y: 0 },
    ...getNodeHandlePositions('vertical'),
    data: {
      label: node.name,
      catalogId: node.id,
      params: node.params,
      modificationPolicy: node.modificationPolicy ?? {},
      recoveryPolicy: node.recoveryPolicy ?? {},
    },
  });
  
  if (parentId) {
    edges.push({
      id: `edge-${parentId}-${flowId}`,
      source: parentId,
      target: flowId,
      type: 'statusEdge',
    });
  }

  node.children.forEach((child) => {
    collectTreeToFlow(child, flowId, nodes, edges);
  });
}

function treeToLayoutedFlow(node: TreeNode, direction: LayoutDirection): { nodes: Node[]; edges: Edge[] } {
  const nodes: Node[] = [];
  const edges: Edge[] = [];
  collectTreeToFlow(node, null, nodes, edges);
  return {
    nodes: layoutFlow(nodes, edges, direction),
    edges,
  };
}

function layoutFlow(
  nodes: Node[],
  edges: Edge[],
  direction: LayoutDirection,
  orderingDirection: LayoutDirection = direction,
): Node[] {
  if (nodes.length === 0) {
    return nodes;
  }

  const nodeMap = new Map(nodes.map((node) => [node.id, node]));
  const childrenMap = new Map<string, string[]>();
  const hasParent = new Set<string>();

  for (const edge of edges) {
    const children = childrenMap.get(edge.source) || [];
    children.push(edge.target);
    childrenMap.set(edge.source, children);
    hasParent.add(edge.target);
  }

  for (const [parentId, childIds] of childrenMap.entries()) {
    childIds.sort((a, b) => {
      const left = nodeMap.get(a);
      const right = nodeMap.get(b);
      if (!left || !right) {
        return 0;
      }
      return (
        getPrimaryCoordinate(left, orderingDirection) -
        getPrimaryCoordinate(right, orderingDirection)
      );
    });
    childrenMap.set(parentId, childIds);
  }

  const rootNode = nodes.find((node) => !hasParent.has(node.id));
  if (!rootNode) {
    return nodes;
  }

  const spanCache = new Map<string, number>();
  const measureSpan = (nodeId: string): number => {
    const cached = spanCache.get(nodeId);
    if (cached !== undefined) {
      return cached;
    }
    const currentNode = nodeMap.get(nodeId);
    if (!currentNode) {
      return 0;
    }
    const childIds = childrenMap.get(nodeId) || [];
    const ownSpan = getPrimarySize(currentNode, direction);
    let span = ownSpan;
    if (childIds.length > 0) {
      const childSpan = childIds.reduce((sum, childId, index) => {
        return sum + measureSpan(childId) + (index > 0 ? PRIMARY_GAP[direction] : 0);
      }, 0);
      span = Math.max(ownSpan, childSpan);
    }
    spanCache.set(nodeId, span);
    return span;
  };

  measureSpan(rootNode.id);

  const positions = new Map<string, { x: number; y: number }>();
  const { primary: rootPrimary, depth: rootDepth } = ROOT_LAYOUT_CENTER[direction];

  const placeNode = (nodeId: string, primaryCenter: number, depthCenter: number) => {
    const currentNode = nodeMap.get(nodeId);
    if (!currentNode) {
      return;
    }
    positions.set(nodeId, makePosition(primaryCenter, depthCenter, direction, currentNode));
    const childIds = childrenMap.get(nodeId) || [];
    if (childIds.length === 0) {
      return;
    }

    const totalSpan = childIds.reduce((sum, childId, index) => {
      return sum + measureSpan(childId) + (index > 0 ? PRIMARY_GAP[direction] : 0);
    }, 0);
    let cursor = primaryCenter - totalSpan / 2;

    for (const childId of childIds) {
      const childNode = nodeMap.get(childId);
      if (!childNode) {
        continue;
      }
      const childSpan = measureSpan(childId);
      const childPrimary = cursor + childSpan / 2;
      const nextDepthCenter =
        depthCenter
        + getDepthSize(currentNode, direction) / 2
        + DEPTH_GAP[direction]
        + getDepthSize(childNode, direction) / 2;
      placeNode(childId, childPrimary, nextDepthCenter);
      cursor += childSpan + PRIMARY_GAP[direction];
    }
  };

  placeNode(rootNode.id, rootPrimary, rootDepth);

  return nodes.map((node) => {
    const position = positions.get(node.id);
    return {
      ...node,
      ...getNodeHandlePositions(direction),
      ...(position ? { position } : {}),
    };
  });
}

// Helper: convert React Flow nodes + edges → backend TreeNode
function flowToTree(nodes: Node[], edges: Edge[], direction: LayoutDirection): TreeNode | null {
  if (nodes.length === 0) return null;
  
  // Build children map from edges: parentId → childIds[]
  const childrenMap = new Map<string, string[]>();
  const hasParent = new Set<string>();
  
  for (const edge of edges) {
    const children = childrenMap.get(edge.source) || [];
    children.push(edge.target);
    childrenMap.set(edge.source, children);
    hasParent.add(edge.target);
  }
  
  // Find root: node with no incoming edge
  const rootNode = nodes.find(n => !hasParent.has(n.id));
  if (!rootNode) return null;
  
  const nodeMap = new Map(nodes.map(n => [n.id, n]));
  
  function buildTree(nodeId: string): TreeNode {
    const node = nodeMap.get(nodeId)!;
    const childIds = childrenMap.get(nodeId) || [];
    
    // Preserve semantic sibling order from the active layout axis.
    childIds.sort((a, b) => {
      const na = nodeMap.get(a)!;
      const nb = nodeMap.get(b)!;
      return getPrimaryCoordinate(na, direction) - getPrimaryCoordinate(nb, direction);
    });
    
    return {
      id: (node.data as any).catalogId || node.id,
      name: (node.data as any).label || '',
      params: (node.data as any).params || {},
      modificationPolicy: (node.data as any).modificationPolicy || {},
      recoveryPolicy: (node.data as any).recoveryPolicy || {},
      children: childIds.map(buildTree),
    };
  }
  
  return buildTree(rootNode.id);
}

function isProgramNotLoadedError(error: Error): boolean {
  return error.message.includes('No program loaded');
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => {
    window.setTimeout(resolve, ms);
  });
}

let hydratePromise: Promise<boolean> | null = null;

export const useProgramStore = create<ProgramStore>((set, get) => ({
  programInfo: null,
  nodes: [],
  edges: [],
  appliedTree: null,
  savedTree: null,
  layoutDirection: 'horizontal',
  layoutRevision: 0,
  selectedNodeId: null,
  runtimeOverrideStatus: null,
  loading: false,
  error: null,
  isDirty: false,

  loadProgram: async (path: string, deploymentPath?: string) => {
    set({ loading: true, error: null });
    try {
      const info = await api.program.load(path, deploymentPath);
      set({ programInfo: info, isDirty: false });
      // Fetch and render tree
      await get().fetchTree();
    } catch (e) {
      console.error('Failed to load program', e);
      set({ error: (e as Error).message, loading: false });
    }
  },

  fetchProgramInfo: async () => {
    try {
      const info = await api.program.get();
      set({ programInfo: info, error: null });
      return true;
    } catch (e) {
      const error = e as Error;
      if (isProgramNotLoadedError(error)) {
        set({ runtimeOverrideStatus: null });
        return false;
      }
      console.error('Failed to fetch program info', e);
      set({ error: error.message, loading: false });
      return false;
    }
  },

  fetchTree: async () => {
    set({ loading: true });
    try {
      const [data, savedData, runtimeOverrideStatus] = await Promise.all([
        api.program.tree(),
        api.program.savedTree().catch(() => null),
        api.program.runtimeOverrides().catch(() => null),
      ]);
      const nodes: Node[] = [];
      const edges: Edge[] = [];
      collectTreeToFlow(data.tree, null, nodes, edges);
      const layoutDirection = get().layoutDirection;
      set({
        nodes: layoutFlow(nodes, edges, layoutDirection),
        edges,
        appliedTree: data.tree,
        savedTree: savedData?.tree ?? data.tree,
        runtimeOverrideStatus,
        layoutRevision: get().layoutRevision + 1,
        loading: false,
        isDirty: false,
        error: null,
      });
      return true;
    } catch (e) {
      console.error('Failed to fetch program tree', e);
      set({ error: (e as Error).message, loading: false });
      return false;
    }
  },

  fetchRuntimeOverrideStatus: async () => {
    try {
      const status = await api.program.runtimeOverrides();
      set({ runtimeOverrideStatus: status, error: null });
      return status;
    } catch (e) {
      const error = e as Error;
      if (isProgramNotLoadedError(error)) {
        set({ runtimeOverrideStatus: null });
        return null;
      }
      console.error('Failed to fetch runtime override status', e);
      set({ error: error.message });
      return null;
    }
  },

  hydrateLoadedProgram: async ({ retries = 4, retryDelayMs = 500 } = {}) => {
    if (hydratePromise) {
      return hydratePromise;
    }

    hydratePromise = (async () => {
      for (let attempt = 0; attempt <= retries; attempt += 1) {
        const hasProgram = await get().fetchProgramInfo();
        if (hasProgram) {
          return get().fetchTree();
        }

        if (attempt < retries) {
          await sleep(retryDelayMs);
        }
      }

      return false;
    })();

    try {
      return await hydratePromise;
    } finally {
      hydratePromise = null;
    }
  },

  setNodes: (nodesOrUpdater, options) =>
    set((state) => {
      const nodes =
        typeof nodesOrUpdater === 'function'
          ? nodesOrUpdater(state.nodes)
          : nodesOrUpdater;
      return {
        nodes,
        isDirty: options?.dirty === false ? state.isDirty : true,
      };
    }),
  setEdges: (edgesOrUpdater, options) =>
    set((state) => {
      const edges =
        typeof edgesOrUpdater === 'function'
          ? edgesOrUpdater(state.edges)
          : edgesOrUpdater;
      return {
        edges,
        isDirty: options?.dirty === false ? state.isDirty : true,
      };
    }),

  addNode: (catalogId, position) => {
    const id = `node-${Date.now()}`;
    const newNode: Node = {
      id,
      type: 'btNode',
      position,
      ...getNodeHandlePositions(get().layoutDirection),
      data: {
        label: catalogId,
        catalogId,
        params: {},
        modificationPolicy: {},
      },
    };
    set({ nodes: [...get().nodes, newNode], isDirty: true });
  },

  removeNode: (nodeId) => {
    set({
      nodes: get().nodes.filter(n => n.id !== nodeId),
      edges: get().edges.filter(e => e.source !== nodeId && e.target !== nodeId),
      selectedNodeId: get().selectedNodeId === nodeId ? null : get().selectedNodeId,
      isDirty: true,
    });
  },

  removeEdge: (edgeId) => {
    set({
      edges: get().edges.filter(e => e.id !== edgeId),
      isDirty: true,
    });
  },

  disconnectNode: (nodeId) => {
    set({
      edges: get().edges.filter(e => e.source !== nodeId && e.target !== nodeId),
      isDirty: true,
    });
  },

  selectNode: (id) => set({ selectedNodeId: id }),

  updateNodeParams: (nodeId, params) => {
    set({
      nodes: get().nodes.map(node =>
        node.id === nodeId
          ? { ...node, data: { ...node.data, params } }
          : node
      ),
      isDirty: true,
    });
  },

  updateNodeLabel: (nodeId, label) => {
    set({
      nodes: get().nodes.map(node =>
        node.id === nodeId
          ? { ...node, data: { ...node.data, label } }
          : node
      ),
      isDirty: true,
    });
  },

  flowToTree: () => {
    return flowToTree(get().nodes, get().edges, get().layoutDirection);
  },

  setLayoutDirection: (direction) => {
    set((state) => {
      if (state.layoutDirection === direction) {
        return state;
      }
      return {
        layoutDirection: direction,
        nodes: layoutFlow(state.nodes, state.edges, direction, state.layoutDirection),
        layoutRevision: state.layoutRevision + 1,
      };
    });
  },

  autoLayout: () => {
    set((state) => ({
      nodes: layoutFlow(state.nodes, state.edges, state.layoutDirection),
      layoutRevision: state.layoutRevision + 1,
    }));
  },

  discardToApplied: () => {
    const appliedTree = get().appliedTree;
    if (!appliedTree) {
      return;
    }
    const { nodes, edges } = treeToLayoutedFlow(appliedTree, get().layoutDirection);
    set({
      nodes,
      edges,
      selectedNodeId: null,
      isDirty: false,
      error: null,
      layoutRevision: get().layoutRevision + 1,
    });
  },

  setAppliedToDraft: () => {
    const tree = get().flowToTree();
    set({ appliedTree: tree });
  },

  setSavedToDraft: () => {
    const tree = get().flowToTree();
    set({ savedTree: tree, isDirty: false });
  },

  activateRuntimeOverrides: async () => {
    await api.program.activateRuntimeOverrides();
    await get().fetchTree();
  },

  clearRuntimeOverrides: async () => {
    await api.program.clearRuntimeOverrides();
    await get().fetchTree();
  },

  saveProgram: async (path: string) => {
    const tree = get().flowToTree();
    if (!tree) {
      throw new Error('No tree to save');
    }
    await api.program.saveTree(path, tree);
    set((state) => ({
      savedTree: tree,
      isDirty: false,
      programInfo: state.programInfo
        ? { ...state.programInfo, sourcePath: path }
        : state.programInfo,
    }));
  },
}));
