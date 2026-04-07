import { create } from 'zustand';
import { api } from '../api/client';
import type { TreeNode, ProgramInfo } from '../types/api';
import type { Node, Edge } from '@xyflow/react';

interface ProgramStore {
  programInfo: ProgramInfo | null;
  nodes: Node[];
  edges: Edge[];
  selectedNodeId: string | null;
  loading: boolean;
  error: string | null;
  isDirty: boolean;
  
  loadProgram: (path: string) => Promise<void>;
  fetchProgramInfo: () => Promise<boolean>;
  fetchTree: () => Promise<boolean>;
  hydrateLoadedProgram: (options?: { retries?: number; retryDelayMs?: number }) => Promise<boolean>;
  setNodes: (nodes: Node[]) => void;
  setEdges: (edges: Edge[]) => void;
  addNode: (catalogId: string, position: { x: number; y: number }) => void;
  removeNode: (nodeId: string) => void;
  selectNode: (id: string | null) => void;
  updateNodeParams: (nodeId: string, params: Record<string, unknown>) => void;
  updateNodeLabel: (nodeId: string, label: string) => void;
  flowToTree: () => TreeNode | null;
  saveProgram: (path: string) => Promise<void>;
}

// Helper: convert backend TreeNode → React Flow nodes + edges
function treeToFlow(
  node: TreeNode,
  parentId: string | null,
  position: { x: number; y: number },
  nodes: Node[],
  edges: Edge[],
  depth: number = 0,
) {
  const flowId = `node-${nodes.length}`;
  
  nodes.push({
    id: flowId,
    type: 'btNode',
    position,
    data: {
      label: node.name,
      catalogId: node.id,
      params: node.params,
    },
  });
  
  if (parentId) {
    edges.push({
      id: `edge-${parentId}-${flowId}`,
      source: parentId,
      target: flowId,
    });
  }
  
  // Layout children vertically, spaced horizontally
  const childSpacing = 200;
  const startX = position.x - ((node.children.length - 1) * childSpacing) / 2;
  
  node.children.forEach((child, i) => {
    treeToFlow(
      child,
      flowId,
      { x: startX + i * childSpacing, y: position.y + 120 },
      nodes,
      edges,
      depth + 1,
    );
  });
}

// Helper: convert React Flow nodes + edges → backend TreeNode
function flowToTree(nodes: Node[], edges: Edge[]): TreeNode | null {
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
  
  // Sort children by x-position (left to right)
  const nodeMap = new Map(nodes.map(n => [n.id, n]));
  
  function buildTree(nodeId: string): TreeNode {
    const node = nodeMap.get(nodeId)!;
    const childIds = childrenMap.get(nodeId) || [];
    
    // Sort children by x-position
    childIds.sort((a, b) => {
      const na = nodeMap.get(a)!;
      const nb = nodeMap.get(b)!;
      return na.position.x - nb.position.x;
    });
    
    return {
      id: (node.data as any).catalogId || node.id,
      name: (node.data as any).label || '',
      params: (node.data as any).params || {},
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
  selectedNodeId: null,
  loading: false,
  error: null,
  isDirty: false,

  loadProgram: async (path: string) => {
    set({ loading: true, error: null });
    try {
      const info = await api.program.load(path);
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
      const data = await api.program.tree();
      const nodes: Node[] = [];
      const edges: Edge[] = [];
      treeToFlow(data.tree, null, { x: 400, y: 50 }, nodes, edges);
      set({ nodes, edges, loading: false, isDirty: false, error: null });
      return true;
    } catch (e) {
      console.error('Failed to fetch program tree', e);
      set({ error: (e as Error).message, loading: false });
      return false;
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

  setNodes: (nodes) => set({ nodes, isDirty: true }),
  setEdges: (edges) => set({ edges, isDirty: true }),

  addNode: (catalogId, position) => {
    const id = `node-${Date.now()}`;
    const newNode: Node = {
      id,
      type: 'btNode',
      position,
      data: {
        label: catalogId,
        catalogId,
        params: {},
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
    return flowToTree(get().nodes, get().edges);
  },

  saveProgram: async (path: string) => {
    const tree = get().flowToTree();
    if (!tree) {
      throw new Error('No tree to save');
    }
    await api.program.updateTree(tree);
    await api.program.save(path);
    set({ isDirty: false });
  },
}));
