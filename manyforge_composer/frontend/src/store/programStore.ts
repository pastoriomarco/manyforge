import { create } from 'zustand';
import { api } from '../api/client';
import { useCatalogStore } from './catalogStore';
import type {
  CatalogParameter,
  TreeNode,
  ProgramInfo,
  ProgramParameter,
  ProgramSceneResource,
  ProgramBlackboard,
  BlackboardNamespace,
  BlackboardKey,
  RuntimeOverrideStatus,
} from '../types/api';
import { Position } from '@xyflow/react';
import type { Node, Edge } from '@xyflow/react';
import { profileSatisfies } from '../utils/profiles';

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
  programParameters: ProgramParameter[];
  appliedProgramParameters: ProgramParameter[];
  savedProgramParameters: ProgramParameter[];
  sceneResources: ProgramSceneResource[];
  appliedSceneResources: ProgramSceneResource[];
  savedSceneResources: ProgramSceneResource[];
  blackboardNamespaces: BlackboardNamespace[];
  appliedBlackboardNamespaces: BlackboardNamespace[];
  savedBlackboardNamespaces: BlackboardNamespace[];
  blackboardKeys: BlackboardKey[];
  appliedBlackboardKeys: BlackboardKey[];
  savedBlackboardKeys: BlackboardKey[];
  layoutDirection: LayoutDirection;
  layoutRevision: number;
  selectedNodeId: string | null;
  runtimeOverrideStatus: RuntimeOverrideStatus | null;
  loading: boolean;
  error: string | null;
  setError: (error: string | null) => void;
  isDirty: boolean;
  
  loadProgram: (path: string, deploymentPath?: string, forceDiscardOverrides?: boolean) => Promise<void>;
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
  updateProgramParameter: (name: string, patch: Partial<ProgramParameter>) => void;
  addProgramParameter: (parameter: ProgramParameter) => void;
  deleteProgramParameter: (name: string) => void;
  setSceneResources: (sceneResources: ProgramSceneResource[]) => void;
  updateBlackboardNamespace: (id: string, patch: Partial<BlackboardNamespace>) => void;
  addBlackboardNamespace: (namespace: BlackboardNamespace) => void;
  deleteBlackboardNamespace: (id: string) => void;
  updateBlackboardKey: (id: string, patch: Partial<BlackboardKey>) => void;
  addBlackboardKey: (key: BlackboardKey) => void;
  deleteBlackboardKey: (id: string) => void;
  flowToTree: () => TreeNode | null;
  setLayoutDirection: (direction: LayoutDirection) => void;
  autoLayout: () => void;
  discardToApplied: () => void;
  setAppliedToDraft: () => void;
  setSavedToDraft: () => void;
  activateRuntimeOverrides: () => Promise<void>;
  clearRuntimeOverrides: () => Promise<void>;
  saveProgram: (path: string) => Promise<void>;
  validateDraftGraph: () => string | null;
}

interface FlowTreeBuildResult {
  tree: TreeNode | null;
  error: string | null;
}

function cloneBlackboard(blackboard?: ProgramBlackboard | null): ProgramBlackboard {
  return {
    namespaces: structuredClone(blackboard?.namespaces ?? []),
    keys: structuredClone(blackboard?.keys ?? []),
  };
}

function cloneSceneResources(sceneResources?: ProgramSceneResource[] | null): ProgramSceneResource[] {
  return structuredClone(sceneResources ?? []);
}

function defaultEnumValue(
  param: CatalogParameter,
  programProfile: string | null | undefined,
): string | null {
  const options = param.enumOptions.length > 0
    ? param.enumOptions
    : param.enumValues.map((value) => ({
        value,
        label: value,
        description: '',
        profilesMin: 'v0.0',
        requiredCapabilities: [],
      }));
  return options.find((option) => profileSatisfies(option.profilesMin, programProfile))?.value ?? null;
}

function buildDefaultNodeParams(
  catalogId: string,
  programProfile: string | null | undefined,
): Record<string, unknown> {
  const entry = useCatalogStore.getState().entries.find((candidate) => candidate.id === catalogId);
  const defaults: Record<string, unknown> = {};

  for (const param of entry?.parameters ?? []) {
    if (!profileSatisfies(param.profilesMin, programProfile)) {
      continue;
    }
    if (param.default !== undefined && param.default !== null) {
      defaults[param.name] = structuredClone(param.default);
      continue;
    }
    if (param.required && param.type === 'enum') {
      const value = defaultEnumValue(param, programProfile);
      if (value != null) {
        defaults[param.name] = value;
      }
    }
  }

  if (catalogId === 'add_collision_object') {
    defaults.shape_type = typeof defaults.shape_type === 'string' ? defaults.shape_type : 'box';
    if (!Array.isArray(defaults.box_dimensions_m) || defaults.box_dimensions_m.length !== 3) {
      defaults.box_dimensions_m = [0.1, 0.1, 0.1];
    }
    if (
      defaults.pose === undefined
      || defaults.pose === null
      || typeof defaults.pose !== 'object'
      || Array.isArray(defaults.pose)
    ) {
      defaults.pose = {
        position_m: [0, 0, 0],
        orientation_rpy_deg: [0, 0, 0],
      };
    }
    defaults.frame_id = typeof defaults.frame_id === 'string' && defaults.frame_id.trim()
      ? defaults.frame_id
      : 'universe';
  }

  if (catalogId === 'update_collision_object_pose') {
    if (
      defaults.pose === undefined
      || defaults.pose === null
      || typeof defaults.pose !== 'object'
      || Array.isArray(defaults.pose)
    ) {
      defaults.pose = {
        position_m: [0, 0, 0],
        orientation_rpy_deg: [0, 0, 0],
      };
    }
    defaults.frame_id = typeof defaults.frame_id === 'string' && defaults.frame_id.trim()
      ? defaults.frame_id
      : 'universe';
  }

  return defaults;
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

function getNodeLabel(node: Node): string {
  const data = node.data as { label?: string; catalogId?: string };
  return String(data.label || data.catalogId || node.id);
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

function buildFlowTree(
  nodes: Node[],
  edges: Edge[],
  direction: LayoutDirection,
): FlowTreeBuildResult {
  if (nodes.length === 0) {
    return { tree: null, error: null };
  }

  const nodeMap = new Map(nodes.map((node) => [node.id, node]));
  const childrenMap = new Map<string, string[]>();
  const parentMap = new Map<string, string>();

  for (const edge of edges) {
    if (!nodeMap.has(edge.source) || !nodeMap.has(edge.target)) {
      return { tree: null, error: "Draft tree contains dangling edges" };
    }
    if (parentMap.has(edge.target)) {
      return {
        tree: null,
        error: `Node '${getNodeLabel(nodeMap.get(edge.target)!)}' cannot have multiple parents`,
      };
    }
    parentMap.set(edge.target, edge.source);
    const children = childrenMap.get(edge.source) || [];
    children.push(edge.target);
    childrenMap.set(edge.source, children);
  }

  const roots = nodes.filter((node) => !parentMap.has(node.id));
  if (roots.length === 0) {
    return { tree: null, error: "Draft tree must contain exactly one root node" };
  }
  if (roots.length > 1) {
    return {
      tree: null,
      error: `Draft tree must contain exactly one root node; found ${roots.length}`,
    };
  }

  const visited = new Set<string>();
  const visiting = new Set<string>();

  function buildTree(nodeId: string): TreeNode {
    const node = nodeMap.get(nodeId)!;
    if (visiting.has(nodeId)) {
      throw new Error(`Cycle detected at node '${getNodeLabel(node)}'`);
    }
    if (visited.has(nodeId)) {
      throw new Error(`Node '${getNodeLabel(node)}' is referenced more than once`);
    }
    visiting.add(nodeId);
    visited.add(nodeId);

    const childIds = [...(childrenMap.get(nodeId) || [])];
    childIds.sort((a, b) => {
      const left = nodeMap.get(a)!;
      const right = nodeMap.get(b)!;
      return getPrimaryCoordinate(left, direction) - getPrimaryCoordinate(right, direction);
    });

    const data = node.data as {
      catalogId?: string;
      label?: string;
      params?: Record<string, unknown>;
      modificationPolicy?: Record<string, string>;
      recoveryPolicy?: Record<string, unknown>;
    };
    const built: TreeNode = {
      id: data.catalogId || node.id,
      name: data.label || "",
      params: data.params || {},
      modificationPolicy: data.modificationPolicy || {},
      recoveryPolicy: data.recoveryPolicy || {},
      children: childIds.map(buildTree),
    };
    visiting.delete(nodeId);
    return built;
  }

  try {
    const tree = buildTree(roots[0].id);
    if (visited.size !== nodes.length) {
      return {
        tree: null,
        error: "Draft tree must be fully connected; disconnected nodes are present",
      };
    }
    return { tree, error: null };
  } catch (error) {
    return {
      tree: null,
      error: error instanceof Error ? error.message : "Draft tree is invalid",
    };
  }
}

// Helper: convert React Flow nodes + edges → backend TreeNode
function flowToTree(nodes: Node[], edges: Edge[], direction: LayoutDirection): TreeNode | null {
  return buildFlowTree(nodes, edges, direction).tree;
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
  programParameters: [],
  appliedProgramParameters: [],
  savedProgramParameters: [],
  sceneResources: [],
  appliedSceneResources: [],
  savedSceneResources: [],
  blackboardNamespaces: [],
  appliedBlackboardNamespaces: [],
  savedBlackboardNamespaces: [],
  blackboardKeys: [],
  appliedBlackboardKeys: [],
  savedBlackboardKeys: [],
  layoutDirection: 'horizontal',
  layoutRevision: 0,
  selectedNodeId: null,
  runtimeOverrideStatus: null,
  loading: false,
  error: null,
  isDirty: false,
  setError: (error) => set({ error }),

  loadProgram: async (path: string, deploymentPath?: string, forceDiscardOverrides = false) => {
    set({ loading: true, error: null });
    try {
      const info = await api.program.load(path, deploymentPath, forceDiscardOverrides);
      set({
        programInfo: info,
        sceneResources: cloneSceneResources(info.sceneResources),
        appliedSceneResources: cloneSceneResources(info.sceneResources),
        savedSceneResources: cloneSceneResources(info.sceneResources),
        programParameters: structuredClone(info.parameters ?? []),
        appliedProgramParameters: structuredClone(info.parameters ?? []),
        savedProgramParameters: structuredClone(info.parameters ?? []),
        blackboardNamespaces: cloneBlackboard(info.blackboard).namespaces,
        appliedBlackboardNamespaces: cloneBlackboard(info.blackboard).namespaces,
        savedBlackboardNamespaces: cloneBlackboard(info.blackboard).namespaces,
        blackboardKeys: cloneBlackboard(info.blackboard).keys,
        appliedBlackboardKeys: cloneBlackboard(info.blackboard).keys,
        savedBlackboardKeys: cloneBlackboard(info.blackboard).keys,
        isDirty: false,
      });
      // Fetch and render tree
      await get().fetchTree();
    } catch (e) {
      console.error('Failed to load program', e);
      set({ error: (e as Error).message, loading: false });
    }
  },

  fetchProgramInfo: async () => {
    try {
      const [info, savedInfo] = await Promise.all([
        api.program.get(),
        api.program.saved().catch(() => null),
      ]);
      set({
        programInfo: info,
        sceneResources: cloneSceneResources(info.sceneResources),
        appliedSceneResources: cloneSceneResources(info.sceneResources),
        savedSceneResources: cloneSceneResources(savedInfo?.sceneResources ?? info.sceneResources),
        programParameters: structuredClone(info.parameters ?? []),
        appliedProgramParameters: structuredClone(info.parameters ?? []),
        savedProgramParameters: structuredClone(savedInfo?.parameters ?? info.parameters ?? []),
        blackboardNamespaces: cloneBlackboard(info.blackboard).namespaces,
        appliedBlackboardNamespaces: cloneBlackboard(info.blackboard).namespaces,
        savedBlackboardNamespaces: cloneBlackboard(savedInfo?.blackboard ?? info.blackboard).namespaces,
        blackboardKeys: cloneBlackboard(info.blackboard).keys,
        appliedBlackboardKeys: cloneBlackboard(info.blackboard).keys,
        savedBlackboardKeys: cloneBlackboard(savedInfo?.blackboard ?? info.blackboard).keys,
        error: null,
      });
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
      const [data, savedData, programInfo, savedProgramInfo, runtimeOverrideStatus] = await Promise.all([
        api.program.tree(),
        api.program.savedTree().catch(() => null),
        api.program.get(),
        api.program.saved().catch(() => null),
        api.program.runtimeOverrides().catch(() => null),
      ]);
      const nodes: Node[] = [];
      const edges: Edge[] = [];
      collectTreeToFlow(data.tree, null, nodes, edges);
      const layoutDirection = get().layoutDirection;
      set({
        nodes: layoutFlow(nodes, edges, layoutDirection),
        edges,
        programInfo,
        appliedTree: data.tree,
        savedTree: savedData?.tree ?? data.tree,
        sceneResources: cloneSceneResources(programInfo.sceneResources),
        appliedSceneResources: cloneSceneResources(programInfo.sceneResources),
        savedSceneResources: cloneSceneResources(savedProgramInfo?.sceneResources ?? programInfo.sceneResources),
        programParameters: structuredClone(programInfo.parameters ?? []),
        appliedProgramParameters: structuredClone(programInfo.parameters ?? []),
        savedProgramParameters: structuredClone(savedProgramInfo?.parameters ?? programInfo.parameters ?? []),
        blackboardNamespaces: cloneBlackboard(programInfo.blackboard).namespaces,
        appliedBlackboardNamespaces: cloneBlackboard(programInfo.blackboard).namespaces,
        savedBlackboardNamespaces: cloneBlackboard(savedProgramInfo?.blackboard ?? programInfo.blackboard).namespaces,
        blackboardKeys: cloneBlackboard(programInfo.blackboard).keys,
        appliedBlackboardKeys: cloneBlackboard(programInfo.blackboard).keys,
        savedBlackboardKeys: cloneBlackboard(savedProgramInfo?.blackboard ?? programInfo.blackboard).keys,
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
        error: options?.dirty === false ? state.error : null,
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
        error: options?.dirty === false ? state.error : null,
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
        params: buildDefaultNodeParams(catalogId, get().programInfo?.programProfile ?? null),
        modificationPolicy: {},
      },
    };
    set({ nodes: [...get().nodes, newNode], isDirty: true, error: null });
  },

  removeNode: (nodeId) => {
    set({
      nodes: get().nodes.filter(n => n.id !== nodeId),
      edges: get().edges.filter(e => e.source !== nodeId && e.target !== nodeId),
      selectedNodeId: get().selectedNodeId === nodeId ? null : get().selectedNodeId,
      error: null,
      isDirty: true,
    });
  },

  removeEdge: (edgeId) => {
    set({
      edges: get().edges.filter(e => e.id !== edgeId),
      error: null,
      isDirty: true,
    });
  },

  disconnectNode: (nodeId) => {
    set({
      edges: get().edges.filter(e => e.source !== nodeId && e.target !== nodeId),
      error: null,
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
      error: null,
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
      error: null,
      isDirty: true,
    });
  },

  updateProgramParameter: (name, patch) => {
    set((state) => ({
      programParameters: state.programParameters.map((parameter) =>
        parameter.name === name
          ? { ...parameter, ...patch }
          : parameter
      ),
      error: null,
      isDirty: true,
    }));
  },

  addProgramParameter: (parameter) => {
    set((state) => {
      if (state.programParameters.some((existing) => existing.name === parameter.name)) {
        return state;
      }
      const programParameters = [...state.programParameters, parameter];
      return {
        programParameters,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              parameters: structuredClone(programParameters),
              parameterCount: programParameters.length,
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  deleteProgramParameter: (name) => {
    set((state) => {
      const programParameters = state.programParameters.filter((parameter) => parameter.name !== name);
      if (programParameters.length === state.programParameters.length) {
        return state;
      }
      return {
        programParameters,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              parameters: structuredClone(programParameters),
              parameterCount: programParameters.length,
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  setSceneResources: (sceneResources) => {
    set((state) => ({
      sceneResources: cloneSceneResources(sceneResources),
      programInfo: state.programInfo
        ? {
            ...state.programInfo,
            sceneResources: cloneSceneResources(sceneResources),
            sceneCount: sceneResources.length,
          }
        : state.programInfo,
      error: null,
      isDirty: true,
    }));
  },

  updateBlackboardNamespace: (id, patch) => {
    set((state) => {
      const blackboardNamespaces = state.blackboardNamespaces.map((namespace) =>
        namespace.id === id ? { ...namespace, ...patch } : namespace,
      );
      return {
        blackboardNamespaces,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              blackboard: {
                namespaces: structuredClone(blackboardNamespaces),
                keys: structuredClone(state.blackboardKeys),
              },
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  addBlackboardNamespace: (namespace) => {
    set((state) => {
      if (state.blackboardNamespaces.some((existing) => existing.id === namespace.id)) {
        return state;
      }
      const blackboardNamespaces = [...state.blackboardNamespaces, namespace];
      return {
        blackboardNamespaces,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              blackboard: {
                namespaces: structuredClone(blackboardNamespaces),
                keys: structuredClone(state.blackboardKeys),
              },
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  deleteBlackboardNamespace: (id) => {
    set((state) => {
      const blackboardNamespaces = state.blackboardNamespaces.filter((namespace) => namespace.id !== id);
      return {
        blackboardNamespaces,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              blackboard: {
                namespaces: structuredClone(blackboardNamespaces),
                keys: structuredClone(state.blackboardKeys),
              },
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  updateBlackboardKey: (id, patch) => {
    set((state) => {
      const blackboardKeys = state.blackboardKeys.map((key) =>
        key.id === id ? { ...key, ...patch } : key,
      );
      return {
        blackboardKeys,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              blackboard: {
                namespaces: structuredClone(state.blackboardNamespaces),
                keys: structuredClone(blackboardKeys),
              },
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  addBlackboardKey: (key) => {
    set((state) => {
      if (state.blackboardKeys.some((existing) => existing.id === key.id)) {
        return state;
      }
      const blackboardKeys = [...state.blackboardKeys, key];
      return {
        blackboardKeys,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              blackboard: {
                namespaces: structuredClone(state.blackboardNamespaces),
                keys: structuredClone(blackboardKeys),
              },
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  deleteBlackboardKey: (id) => {
    set((state) => {
      const blackboardKeys = state.blackboardKeys.filter((key) => key.id !== id);
      return {
        blackboardKeys,
        programInfo: state.programInfo
          ? {
              ...state.programInfo,
              blackboard: {
                namespaces: structuredClone(state.blackboardNamespaces),
                keys: structuredClone(blackboardKeys),
              },
            }
          : state.programInfo,
        error: null,
        isDirty: true,
      };
    });
  },

  flowToTree: () => {
    return flowToTree(get().nodes, get().edges, get().layoutDirection);
  },

  validateDraftGraph: () => {
    return buildFlowTree(get().nodes, get().edges, get().layoutDirection).error;
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
      error: null,
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
      sceneResources: cloneSceneResources(get().appliedSceneResources),
      programParameters: structuredClone(get().appliedProgramParameters),
      blackboardNamespaces: structuredClone(get().appliedBlackboardNamespaces),
      blackboardKeys: structuredClone(get().appliedBlackboardKeys),
      selectedNodeId: null,
      isDirty: false,
      error: null,
      layoutRevision: get().layoutRevision + 1,
    });
  },

  setAppliedToDraft: () => {
    const tree = get().flowToTree();
    set((state) => ({
      appliedTree: tree,
      appliedSceneResources: cloneSceneResources(get().sceneResources),
      appliedProgramParameters: structuredClone(get().programParameters),
      appliedBlackboardNamespaces: structuredClone(get().blackboardNamespaces),
      appliedBlackboardKeys: structuredClone(get().blackboardKeys),
      programInfo: state.programInfo
        ? {
            ...state.programInfo,
            sceneResources: cloneSceneResources(state.sceneResources),
            sceneCount: state.sceneResources.length,
            parameters: structuredClone(state.programParameters),
            parameterCount: state.programParameters.length,
            blackboard: {
              namespaces: structuredClone(state.blackboardNamespaces),
              keys: structuredClone(state.blackboardKeys),
            },
          }
        : state.programInfo,
    }));
  },

  setSavedToDraft: () => {
    const tree = get().flowToTree();
    set({
      savedTree: tree,
      savedSceneResources: cloneSceneResources(get().sceneResources),
      savedProgramParameters: structuredClone(get().programParameters),
      savedBlackboardNamespaces: structuredClone(get().blackboardNamespaces),
      savedBlackboardKeys: structuredClone(get().blackboardKeys),
      isDirty: false,
    });
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
    const validationError = get().validateDraftGraph();
    if (validationError) {
      throw new Error(validationError);
    }
    const tree = get().flowToTree();
    if (!tree) {
      throw new Error('Draft tree is invalid');
    }
    const result = await api.program.saveTree(path, tree, get().sceneResources, get().programParameters, {
      namespaces: get().blackboardNamespaces,
      keys: get().blackboardKeys,
    });
    const savedPath =
      typeof result === 'object' && result !== null && 'path' in result && typeof result.path === 'string'
        ? result.path
        : path;
    set((state) => ({
      savedTree: tree,
      savedSceneResources: cloneSceneResources(state.sceneResources),
      savedProgramParameters: structuredClone(state.programParameters),
      savedBlackboardNamespaces: structuredClone(state.blackboardNamespaces),
      savedBlackboardKeys: structuredClone(state.blackboardKeys),
      isDirty: false,
      programInfo: state.programInfo
        ? {
            ...state.programInfo,
            sourcePath: savedPath,
            sceneResources: cloneSceneResources(state.sceneResources),
            sceneCount: state.sceneResources.length,
            parameters: structuredClone(state.programParameters),
            parameterCount: state.programParameters.length,
            blackboard: {
              namespaces: structuredClone(state.blackboardNamespaces),
              keys: structuredClone(state.blackboardKeys),
            },
          }
        : state.programInfo,
    }));
  },
}));
