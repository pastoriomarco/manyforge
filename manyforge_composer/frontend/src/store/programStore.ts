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
  
  loadProgram: (path: string) => Promise<void>;
  fetchTree: () => Promise<void>;
  setNodes: (nodes: Node[]) => void;
  setEdges: (edges: Edge[]) => void;
  addNode: (catalogId: string, position: { x: number; y: number }) => void;
  removeNode: (nodeId: string) => void;
  selectNode: (id: string | null) => void;
  updateNodeParams: (nodeId: string, params: Record<string, unknown>) => void;
  updateNodeLabel: (nodeId: string, label: string) => void;
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

export const useProgramStore = create<ProgramStore>((set, get) => ({
  programInfo: null,
  nodes: [],
  edges: [],
  selectedNodeId: null,
  loading: false,
  error: null,

  loadProgram: async (path: string) => {
    set({ loading: true, error: null });
    try {
      const info = await api.program.load(path);
      set({ programInfo: info });
      // Fetch and render tree
      await get().fetchTree();
    } catch (e) {
      set({ error: (e as Error).message, loading: false });
    }
  },

  fetchTree: async () => {
    try {
      const data = await api.program.tree();
      const nodes: Node[] = [];
      const edges: Edge[] = [];
      treeToFlow(data.tree, null, { x: 400, y: 50 }, nodes, edges);
      set({ nodes, edges, loading: false });
    } catch (e) {
      set({ error: (e as Error).message, loading: false });
    }
  },

  setNodes: (nodes) => set({ nodes }),
  setEdges: (edges) => set({ edges }),

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
    set({ nodes: [...get().nodes, newNode] });
  },

  removeNode: (nodeId) => {
    set({
      nodes: get().nodes.filter(n => n.id !== nodeId),
      edges: get().edges.filter(e => e.source !== nodeId && e.target !== nodeId),
      selectedNodeId: get().selectedNodeId === nodeId ? null : get().selectedNodeId,
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
    });
  },

  updateNodeLabel: (nodeId, label) => {
    set({
      nodes: get().nodes.map(node =>
        node.id === nodeId
          ? { ...node, data: { ...node.data, label } }
          : node
      ),
    });
  },
}));
