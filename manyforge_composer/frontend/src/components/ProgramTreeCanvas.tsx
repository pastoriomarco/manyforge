import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type DragEvent,
  type KeyboardEvent,
} from 'react';
import {
  BaseEdge,
  Background,
  Controls,
  ReactFlow,
  type Connection,
  type Edge,
  type EdgeProps,
  type EdgeTypes,
  type Node,
  type NodeChange,
  type NodeMouseHandler,
  type NodeProps,
  type NodeTypes,
  type ReactFlowInstance,
  type XYPosition,
} from '@xyflow/react';
import '@xyflow/react/dist/style.css';

import {
  useComposerViewState,
  type LayoutDirection,
  type NodePresentationStatus,
} from '../store/composerStore';
import type {
  CatalogEntry,
  JsonValue,
  NodeEnvironmentResponse,
  ProgramTreeNode,
} from '../types/api';
import {
  authoringMoveIntent,
  authoringReorderedIds,
  authoringSiblingInsertionIndex,
  type AuthoringGraphEdge,
  type AuthoringGraphNode,
  layoutAuthoringGraph,
  validateAuthoringConnection,
  validateAuthoringDeletion,
} from '../utils/authoringGraph';
import {
  flattenProgramForest,
} from '../utils/programAuthoring';
import {
  isProgramCanvasFormTarget,
  programCanvasKeyIntent,
  programChangeKindMutation,
  programDeleteMutation,
  programDetachedInsertionMutation,
  programInsertionMutation,
  programMoveMutation,
  programWrapMutation,
  readProgramPaletteDrag,
} from '../utils/programCanvasInteraction';
import { BtNode, type BtNodePresentation } from './BtNode';
import { CanvasViewControls } from './CanvasViewControls';
import {
  ProgramNoticeRail,
  type ProgramNotice,
  type ProgramNoticeInput,
} from './ProgramNoticeRail';
import { statusEdgePath } from './StatusEdge';
import { CANVAS_COLORS, STATUS_COLORS } from '../styles/tokens';

// UI6-I-VC D3: every node has the same fixed size (--mf-node-width/height).
const NODE_WIDTH = 200;
const NODE_BASE_HEIGHT = 70;
const STAGE_PADDING = 48;

function acceptsChildCount(entry: CatalogEntry | undefined, count: number): boolean {
  return entry !== undefined
    && count >= entry.childrenMin
    && (entry.childrenMax < 0 || count <= entry.childrenMax);
}

const NODE_SIZE = { width: NODE_WIDTH, height: NODE_BASE_HEIGHT } as const;

interface ProgramFlowNodeData extends Record<string, unknown> {
  presentation: BtNodePresentation;
  onSelect: (instanceId: string) => void;
  onFocus: (instanceId: string) => void;
  onDelete: (event: KeyboardEvent<HTMLDivElement>, instanceId: string) => void;
  onNavigate: (instanceId: string, key: string) => void;
  registerElement: (instanceId: string, element: HTMLDivElement | null) => void;
}

interface ProgramFlowEdgeData extends Record<string, unknown> {
  edgeStyle: ProgramEdgeStyle;
  layoutDirection: 'vertical' | 'horizontal';
  targetStatus: BtNodePresentation['status'];
}

type ProgramFlowNode = Node<ProgramFlowNodeData, 'programNode'>;
type ProgramFlowEdge = Edge<ProgramFlowEdgeData, 'statusEdge'>;
type ProgramEdgeStyle = 'smoothstep' | 'simplebezier' | 'default' | 'straight' | 'step';

interface ProgramTreeCanvasProps {
  roots: ProgramTreeNode[];
  retainedPositions: Record<string, { x: number; y: number }>;
  catalogEntries: CatalogEntry[];
  environment: NodeEnvironmentResponse | null;
  selectedId: string | null;
  invalidNodeIds: ReadonlySet<string>;
  nodeStatuses?: Record<string, NodePresentationStatus>;
  busy: boolean;
  onSelect: (instanceId: string | null) => void;
  onMutate: (
    mutation: string,
    args: Record<string, JsonValue>,
  ) => Promise<import('../types/api').DraftMutationResponse | null>;
  notices?: ProgramNotice[];
  onNotice?: (notice: ProgramNoticeInput) => void;
  onDismissNotice?: (id: string) => void;
  onDismissAllNotices?: () => void;
}

function ProgramFlowNode({ data, selected }: NodeProps<ProgramFlowNode>) {
  const presentationId = data.presentation.id;
  const registerElement = data.registerElement;
  const elementRef = useCallback((element: HTMLDivElement | null) => {
    registerElement(presentationId, element);
  }, [presentationId, registerElement]);
  return (
    <BtNode
      node={{ ...data.presentation, selected: selected || data.presentation.selected }}
      onSelect={data.onSelect}
      onFocus={data.onFocus}
      onDelete={data.onDelete}
      onNavigate={data.onNavigate}
      elementRef={elementRef}
    />
  );
}

function ProgramStatusEdge({
  sourceX,
  sourceY,
  targetX,
  targetY,
  sourcePosition,
  targetPosition,
  markerEnd,
  data,
}: EdgeProps<ProgramFlowEdge>) {
  const edgeStyle = data?.edgeStyle ?? 'default';
  const stroke = STATUS_COLORS[data?.targetStatus ?? 'idle'];
  return (
    <BaseEdge
      path={statusEdgePath(
        { sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition },
        edgeStyle,
        data?.layoutDirection ?? 'vertical',
      )}
      markerEnd={markerEnd}
      style={{
        stroke,
        strokeWidth: 2,
      }}
    />
  );
}

const NODE_TYPES: NodeTypes = { programNode: ProgramFlowNode };
const EDGE_TYPES: EdgeTypes = { statusEdge: ProgramStatusEdge };
export function ProgramTreeCanvas({
  roots,
  retainedPositions,
  catalogEntries,
  environment,
  selectedId,
  invalidNodeIds,
  nodeStatuses = {},
  busy,
  onSelect,
  onMutate,
  notices = [],
  onNotice = () => undefined,
  onDismissNotice = () => undefined,
  onDismissAllNotices = () => undefined,
}: ProgramTreeCanvasProps) {
  const [view, dispatchView] = useComposerViewState();
  const [presentationPositions, setPresentationPositions] = useState<
    Record<string, XYPosition>
  >({});
  const [nodeMeasurements, setNodeMeasurements] = useState<
    Record<string, { width: number; height: number }>
  >({});
  const [focusedNodeId, setFocusedNodeId] = useState<string | null>(null);
  const flowInstance = useRef<ReactFlowInstance<ProgramFlowNode, ProgramFlowEdge> | null>(null);
  const nodeElements = useRef<Record<string, HTMLDivElement | null>>({});
  const pendingPositionChanges = useRef<Record<string, XYPosition>>({});
  const positionFrame = useRef<number | null>(null);
  const showCanvasMessage = useCallback((message: string, kind: 'info' | 'error') => {
    onNotice({
      key: `canvas:${kind}:${message}`,
      message,
      kind,
    });
  }, [onNotice]);
  useEffect(() => () => {
    if (
      positionFrame.current !== null
      && typeof globalThis.cancelAnimationFrame === 'function'
    ) {
      globalThis.cancelAnimationFrame(positionFrame.current);
    }
  }, []);
  const flat = useMemo(() => flattenProgramForest(roots), [roots]);
  const catalog = useMemo(
    () => new Map(catalogEntries.map((entry) => [entry.id, entry])),
    [catalogEntries],
  );
  const availability = useMemo(() => {
    const result = new Map<string, {
      authoring: boolean;
      execution: boolean;
      reasons: string[];
    }>();
    for (const row of environment?.implementations ?? []) {
      const previous = result.get(row.nodeId) ?? {
        authoring: false,
        execution: false,
        reasons: [],
      };
      result.set(row.nodeId, {
        authoring: previous.authoring || row.authoringAvailable,
        execution: previous.execution || row.executionAvailable,
        reasons: [...new Set([...previous.reasons, ...row.reasonCodes])].sort(),
      });
    }
    return result;
  }, [environment]);
  const flatById = useMemo(
    () => new Map(flat.map((row) => [row.node.instance_id, row])),
    [flat],
  );
  const edges = useMemo<AuthoringGraphEdge[]>(
    () => flat
      .filter((row) => row.parentId !== null)
      .map((row) => ({ source: row.parentId as string, target: row.node.instance_id })),
    [flat],
  );
  const layoutNodes = useCallback((
    layoutDirection: LayoutDirection,
    orderedIds: string[] = flat.map((row) => row.node.instance_id),
  ) => {
    // The view names the direction in which siblings are read. The layout
    // utility names the perpendicular graph-growth axis.
    const graphDirection = layoutDirection === 'vertical'
      ? 'horizontal'
      : 'vertical';
    const orderById = new Map(orderedIds.map((id, order) => [id, order]));
    const seeds: AuthoringGraphNode[] = flat.map((row, fallbackOrder) => {
      const id = row.node.instance_id;
      const dimensions = nodeMeasurements[id] ?? NODE_SIZE;
      const order = orderById.get(id) ?? fallbackOrder;
      return {
        id,
        label: id,
        width: dimensions.width,
        height: dimensions.height,
        childrenMin: catalog.get(row.node.node_id)?.childrenMin,
        childrenMax: catalog.get(row.node.node_id)?.childrenMax,
        position: graphDirection === 'vertical'
          ? { x: order * (NODE_WIDTH + 24), y: row.depth * 150 }
          : { x: row.depth * 340, y: order * (NODE_BASE_HEIGHT + 40) },
      };
    });
    const positioned = layoutAuthoringGraph(
      seeds,
      edges,
      graphDirection,
      graphDirection,
    );
    const minX = Math.min(...positioned.map((node) => node.position.x), 0);
    const minY = Math.min(...positioned.map((node) => node.position.y), 0);
    return positioned.map((node) => ({
      ...node,
      position: {
        x: node.position.x - minX + STAGE_PADDING,
        y: node.position.y - minY + STAGE_PADDING,
      },
    }));
  }, [catalog, edges, flat, nodeMeasurements]);
  const graphDirection = view.layoutDirection === 'vertical'
    ? 'horizontal'
    : 'vertical';
  const laidOut = useMemo(
    () => layoutNodes(view.layoutDirection),
    [layoutNodes, view.layoutDirection],
  );
  const topologyKey = useMemo(
    () => flat.map((row) => [
      row.node.instance_id,
      row.node.node_id,
      row.parentId ?? '',
      row.index,
    ].join(':')).join('|'),
    [flat],
  );
  const selectedRow = flat.find((row) => row.node.instance_id === selectedId) ?? null;
  const [topologyCatalogId, setTopologyCatalogId] = useState('');
  const authoringDisabled = busy || environment?.portableAuthoringAvailable !== true;

  useEffect(() => {
    setPresentationPositions(retainedPositions);
    setNodeMeasurements((current) => {
      const retainedIds = new Set(flat.map((row) => row.node.instance_id));
      const retained = Object.fromEntries(
        Object.entries(current).filter(([id]) => retainedIds.has(id)),
      );
      return Object.keys(retained).length === Object.keys(current).length
        ? current
        : retained;
    });
  }, [flat, retainedPositions, topologyKey]);

  const registerElement = useCallback((id: string, element: HTMLDivElement | null) => {
    nodeElements.current[id] = element;
  }, []);

  const deleteSelectedNode = useCallback((
    event?: KeyboardEvent<HTMLDivElement>,
  ) => {
    if (
      (event !== undefined && isProgramCanvasFormTarget(event.target))
      ||
      authoringDisabled
      || selectedRow === null
    ) return;
    event?.preventDefault();
    event?.stopPropagation();
    const validationError = validateAuthoringDeletion(
      laidOut,
      selectedRow.node.instance_id,
    );
    if (validationError) {
      showCanvasMessage(validationError, 'error');
      return;
    }
    const mutation = programDeleteMutation(selectedRow.node.instance_id);
    onMutate(mutation.mutation, mutation.args);
  }, [authoringDisabled, laidOut, onMutate, selectedRow, showCanvasMessage]);

  const onNodeDelete = useCallback((
    event: KeyboardEvent<HTMLDivElement>,
    instanceId: string,
  ) => {
    if (instanceId !== selectedId || focusedNodeId !== instanceId) return;
    deleteSelectedNode(event);
  }, [deleteSelectedNode, focusedNodeId, selectedId]);

  const onNodeNavigate = useCallback((instanceId: string, key: string) => {
    const index = flat.findIndex((row) => row.node.instance_id === instanceId);
    if (index < 0) return;
    const nextIndex = key === 'Home'
      ? 0
      : key === 'End'
        ? flat.length - 1
        : key === 'ArrowUp' || key === 'ArrowLeft'
          ? index - 1
          : index + 1;
    const next = flat[nextIndex];
    if (!next) return;
    onSelect(next.node.instance_id);
    nodeElements.current[next.node.instance_id]?.focus();
  }, [flat, onSelect]);

  const graphNodes = useMemo<ProgramFlowNode[]>(
    () => laidOut.map((node) => {
      const row = flatById.get(node.id);
      const entry = catalog.get(row?.node.node_id ?? '');
      const nodeAvailability = availability.get(row?.node.node_id ?? '');
      const configurationRequired = entry?.parameters.some((parameter) => (
        parameter.required
        && !parameter.hasDefault
        && row !== undefined
        && !Object.prototype.hasOwnProperty.call(row.node.parameters, parameter.id)
        && !Object.prototype.hasOwnProperty.call(row.node.value_sources ?? {}, parameter.id)
      )) ?? false;
      const executionDisposition = configurationRequired
        ? 'configuration_required' as const
        : nodeAvailability?.execution
          ? 'executable' as const
          : 'unavailable' as const;
      return {
        id: node.id,
        type: 'programNode',
        position: presentationPositions[node.id] ?? retainedPositions[node.id] ?? node.position,
        measured: nodeMeasurements[node.id],
        draggable: true,
        selectable: false,
        focusable: false,
        selected: selectedId === node.id,
        data: {
          presentation: {
            id: node.id,
            label: node.id,
            catalogId: row?.node.node_id ?? '',
            category: entry?.category ?? 'unknown',
            kind: entry?.kind ?? 'leaf',
            childrenMax: entry?.childrenMax ?? row?.node.children.length ?? 0,
            childCount: row?.node.children.length ?? 0,
            hasParent: row?.parentId !== null,
            isRoot: row?.parentId === null,
            status: nodeStatuses[node.id] ?? 'idle',
            selected: selectedId === node.id,
            layoutDirection: view.layoutDirection,
            executionAvailable: nodeAvailability?.execution ?? false,
            executionDisposition,
            executionReason: nodeAvailability?.reasons[0]?.replaceAll('_', ' '),
            invalid: invalidNodeIds.has(node.id),
          },
          onSelect,
          onFocus: setFocusedNodeId,
          onDelete: onNodeDelete,
          onNavigate: onNodeNavigate,
          registerElement,
        },
      };
    }),
    [
      availability,
      catalog,
      flatById,
      laidOut,
      invalidNodeIds,
      nodeMeasurements,
      onNodeNavigate,
      onNodeDelete,
      onSelect,
      presentationPositions,
      retainedPositions,
      registerElement,
      selectedId,
      view.layoutDirection,
      nodeStatuses,
    ],
  );
  const graphEdges = useMemo<ProgramFlowEdge[]>(
    () => edges.map((edge) => ({
      id: `${edge.source}:${edge.target}`,
      source: edge.source,
      target: edge.target,
      type: 'statusEdge',
      data: {
        edgeStyle: view.edgeStyle,
        layoutDirection: view.layoutDirection,
        targetStatus: nodeStatuses[edge.target] ?? 'idle',
      },
    })),
    [edges, nodeStatuses, view.edgeStyle, view.layoutDirection],
  );
  const positions = useMemo(
    () => new Map(graphNodes.map((node) => [node.id, node.position])),
    [graphNodes],
  );
  const graphWidth = Math.max(
    720,
    ...graphNodes.map((node) => node.position.x
      + (nodeMeasurements[node.id]?.width ?? NODE_SIZE.width)
      + STAGE_PADDING),
  );
  const graphHeight = Math.max(
    320,
    ...graphNodes.map((node) => node.position.y
      + (nodeMeasurements[node.id]?.height ?? NODE_SIZE.height)
      + STAGE_PADDING),
  );

  const onNodesChange = useCallback((
    changes: NodeChange<ProgramFlowNode>[],
  ) => {
    const measured: Record<string, { width: number; height: number }> = {};
    for (const change of changes) {
      if (change.type === 'position' && change.position) {
        pendingPositionChanges.current[change.id] = change.position;
      }
      if (change.type === 'dimensions') {
        const dimensions = change.dimensions;
        if (
          dimensions !== undefined
          && dimensions.width > 0
          && dimensions.height > 0
        ) measured[change.id] = dimensions;
      }
      if (change.type === 'select' && change.selected) onSelect(change.id);
    }
    if (Object.keys(measured).length > 0) {
      setNodeMeasurements((current) => {
        let changed = false;
        const next = { ...current };
        for (const [id, dimensions] of Object.entries(measured)) {
          if (
            current[id]?.width === dimensions.width
            && current[id]?.height === dimensions.height
          ) continue;
          next[id] = dimensions;
          changed = true;
        }
        return changed ? next : current;
      });
    }
    if (positionFrame.current !== null) return;
    const flushPositions = () => {
      const pending = pendingPositionChanges.current;
      pendingPositionChanges.current = {};
      positionFrame.current = null;
      setPresentationPositions((current) => ({ ...current, ...pending }));
    };
    if (typeof globalThis.requestAnimationFrame !== 'function') {
      flushPositions();
      return;
    }
    positionFrame.current = globalThis.requestAnimationFrame(flushPositions);
  }, [onSelect]);

  const onNodeDragStop = useCallback<NodeMouseHandler<ProgramFlowNode>>((_, node) => {
    delete pendingPositionChanges.current[node.id];
    setPresentationPositions((current) => ({ ...current, [node.id]: node.position }));
    if (authoringDisabled) return;
    const row = flatById.get(node.id);
    const nextPositions = new Map(positions);
    nextPositions.set(node.id, node.position);
    if (row?.parentId !== null && row?.parentId !== undefined) {
      const displayedNodes = graphNodes.map((candidate) => ({
        ...candidate,
        position: nextPositions.get(candidate.id) ?? candidate.position,
      }));
      const index = authoringSiblingInsertionIndex(
        displayedNodes,
        edges,
        row.parentId,
        node.id,
        graphDirection,
      );
      if (index !== null && index !== row.index) {
        const mutation = programMoveMutation(
          node.id,
          row.parentId,
          index,
          Object.fromEntries(nextPositions),
        );
        onMutate(mutation.mutation, mutation.args);
        return;
      }
    }
    onMutate('tree_draft_update_positions', {
      positions: { [node.id]: { x: node.position.x, y: node.position.y } },
    });
  }, [authoringDisabled, edges, flatById, graphDirection, graphNodes, onMutate, positions]);

  const onConnect = useCallback((connection: Connection) => {
    if (authoringDisabled) {
      showCanvasMessage('Draft editing is unavailable while the owner is busy or portable authoring is gated.', 'error');
      return;
    }
    const result = authoringMoveIntent(
      laidOut,
      edges,
      connection.source,
      connection.target,
    );
    if (result.error || !result.intent) {
      showCanvasMessage(result.error ?? 'That connection is not valid.', 'error');
      return;
    }
    const parentId = result.intent.new_parent_instance_id;
    const index = parentId === null
      ? result.intent.index
      : authoringSiblingInsertionIndex(
        graphNodes,
        edges,
        parentId,
        result.intent.instance_id,
        graphDirection,
      ) ?? result.intent.index;
    const mutation = programMoveMutation(
      result.intent.instance_id,
      parentId,
      index,
      Object.fromEntries(positions),
    );
    onMutate(mutation.mutation, mutation.args);
  }, [
    authoringDisabled,
    edges,
    graphDirection,
    graphNodes,
    laidOut,
    onMutate,
    positions,
    showCanvasMessage,
  ]);

  const detachEdge = useCallback((edge: ProgramFlowEdge) => {
    if (authoringDisabled) return;
    const mutation = programMoveMutation(
      edge.target,
      null,
      roots.length,
      Object.fromEntries(positions),
    );
    onMutate(mutation.mutation, mutation.args);
  }, [authoringDisabled, onMutate, positions, roots.length]);

  const onDragOver = useCallback((event: DragEvent) => {
    if (authoringDisabled) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
  }, [authoringDisabled]);

  const onDrop = useCallback((event: DragEvent) => {
    event.preventDefault();
    if (authoringDisabled) return;
    const catalogId = readProgramPaletteDrag((format) => event.dataTransfer.getData(format));
    const flow = flowInstance.current;
    if (!catalogId || !flow) return;
    const point = flow.screenToFlowPosition({ x: event.clientX, y: event.clientY });
    const entry = catalogEntries.find((candidate) => candidate.id === catalogId);
    if (!entry) {
      showCanvasMessage(`Unknown catalog entry '${catalogId}'.`, 'error');
      return;
    }
    const mutation = programDetachedInsertionMutation(
      entry,
      flat.map((row) => row.node.instance_id),
      roots.length,
      point,
    );
    onMutate(mutation.mutation, mutation.args);
    const instanceId = String(
      (mutation.args.node as Record<string, JsonValue>).instance_id,
    );
    onSelect(instanceId);
  }, [
    authoringDisabled,
    catalogEntries,
    flat,
    onMutate,
    onSelect,
    roots.length,
    showCanvasMessage,
  ]);

  const fitView = useCallback(() => {
    flowInstance.current?.fitView({ padding: 0.2, duration: 180 });
  }, []);
  const applyLayout = useCallback((layout: AuthoringGraphNode[], message: string) => {
    const nextPositions = Object.fromEntries(
      layout.map((node) => [node.id, node.position]),
    );
    setPresentationPositions(nextPositions);
    if (!authoringDisabled) {
      void onMutate('tree_draft_update_positions', { positions: nextPositions });
    }
    showCanvasMessage(message, 'info');
    requestAnimationFrame(fitView);
  }, [authoringDisabled, fitView, onMutate, showCanvasMessage]);
  const autoLayout = useCallback(() => {
    applyLayout(laidOut, 'Layout refreshed.');
  }, [applyLayout, laidOut]);
  const changeLayoutDirection = useCallback((layoutDirection: LayoutDirection) => {
    if (layoutDirection === view.layoutDirection) return;
    dispatchView({ type: 'set_layout_direction', layoutDirection });
    applyLayout(
      layoutNodes(layoutDirection),
      `${layoutDirection === 'vertical' ? 'Vertical' : 'Horizontal'} layout applied.`,
    );
  }, [applyLayout, dispatchView, layoutNodes, view.layoutDirection]);

  const parentOptions = selectedRow === null
    ? []
    : flat.filter((candidate) =>
      candidate.node.instance_id !== selectedRow.node.instance_id
      && validateAuthoringConnection(
        laidOut,
        edges,
        candidate.node.instance_id,
        selectedRow.node.instance_id,
      ) === null);

  const moveSelected = async (newParentId: string, index: number) => {
    if (!selectedRow || authoringDisabled) return;
    const orderedIds = authoringReorderedIds(
      flat.map((row) => ({ id: row.node.instance_id, parentId: row.parentId })),
      newParentId,
      selectedRow.node.instance_id,
      index,
    );
    if (orderedIds === null) return;
    const aligned = layoutNodes(view.layoutDirection, orderedIds);
    const nextPositions = Object.fromEntries(
      aligned.map((node) => [node.id, node.position]),
    );
    const mutation = programMoveMutation(
      selectedRow.node.instance_id,
      newParentId,
      index,
      nextPositions,
    );
    const accepted = await onMutate(mutation.mutation, mutation.args);
    if (accepted) {
      setPresentationPositions(nextPositions);
      showCanvasMessage('Order updated and aligned.', 'info');
    }
  };

  const topologyEntries = catalogEntries.filter(
    (entry) => availability.get(entry.id)?.authoring === true,
  );
  const topologyEntry = catalog.get(topologyCatalogId);
  const selectedNodeEntry = selectedRow === null
    ? undefined
    : catalog.get(selectedRow.node.node_id);
  const selectedParentRow = selectedRow?.parentId === null
    || selectedRow?.parentId === undefined
    ? null
    : flatById.get(selectedRow.parentId) ?? null;
  const selectedParentEntry = selectedParentRow === null
    ? undefined
    : catalog.get(selectedParentRow.node.node_id);
  const childInsertionAvailable = selectedRow !== null
    && acceptsChildCount(selectedNodeEntry, selectedRow.node.children.length + 1);
  const siblingInsertionAvailable = selectedRow !== null
    && (selectedParentRow === null || acceptsChildCount(
      selectedParentEntry,
      selectedParentRow.node.children.length + 1,
    ));
  const wrapperAvailable = acceptsChildCount(topologyEntry, 1);
  const retainedParameterNames = selectedRow === null
    ? []
    : [
        ...Object.keys(selectedRow.node.parameters),
        ...Object.keys(selectedRow.node.value_sources ?? {}),
      ];
  const replacementParameterNames = new Set(
    topologyEntry?.parameters.map((parameter) => parameter.id) ?? [],
  );
  const kindReplacementAvailable = selectedRow !== null
    && topologyEntry !== undefined
    && topologyEntry.id !== selectedRow.node.node_id
    && acceptsChildCount(topologyEntry, selectedRow.node.children.length)
    && retainedParameterNames.every((name) => replacementParameterNames.has(name));

  const insertTopologyNode = async (kind: 'child' | 'after') => {
    if (selectedRow === null || topologyEntry === undefined || authoringDisabled) return;
    const mutation = programInsertionMutation(
      topologyEntry,
      flat.map((row) => row.node.instance_id),
      kind === 'child'
        ? {
            kind: 'child',
            parentInstanceId: selectedRow.node.instance_id,
            index: selectedRow.node.children.length,
          }
        : { kind: 'after', siblingInstanceId: selectedRow.node.instance_id },
    );
    const accepted = await onMutate(mutation.mutation, mutation.args);
    if (accepted === null) return;
    const node = mutation.args.node as Record<string, JsonValue>;
    onSelect(String(node.instance_id));
    showCanvasMessage(
      `${topologyEntry.name} inserted ${kind === 'child' ? 'as a child' : 'after the selected node'}. Complete any required inputs in Properties.`,
      'info',
    );
  };

  const wrapSelected = async () => {
    if (selectedRow === null || topologyEntry === undefined || authoringDisabled) return;
    const mutation = programWrapMutation(
      topologyEntry,
      flat.map((row) => row.node.instance_id),
      selectedRow.node.instance_id,
    );
    const accepted = await onMutate(mutation.mutation, mutation.args);
    if (accepted === null) return;
    const wrapper = mutation.args.wrapper as Record<string, JsonValue>;
    onSelect(String(wrapper.instance_id));
    showCanvasMessage(`${topologyEntry.name} now wraps the selected subtree.`, 'info');
  };

  const changeSelectedKind = async () => {
    if (selectedRow === null || topologyEntry === undefined || authoringDisabled) return;
    const mutation = programChangeKindMutation(
      topologyEntry,
      selectedRow.node.instance_id,
      retainedParameterNames,
    );
    const accepted = await onMutate(mutation.mutation, mutation.args);
    if (accepted === null) return;
    showCanvasMessage(`Node type changed to ${topologyEntry.name} without dropping parameters.`, 'info');
  };

  return (
    <section className="program-tree-surface" aria-label="Program tree canvas">
      <h3 className="sr-only">Program tree canvas</h3>

      <div
        className="program-tree-react-flow"
        aria-label="Interactive Program tree canvas"
        onKeyDown={(event) => {
          const intent = programCanvasKeyIntent(
            event.key,
            isProgramCanvasFormTarget(event.target) ? 'form' : 'node',
          );
          // Node-local handlers own Delete/Backspace. This guard prevents a
          // future canvas shortcut from escaping into surrounding workbench
          // controls or form fields.
          if (intent?.type === 'delete') event.stopPropagation();
        }}
      >
        <div className="program-canvas-topbar">
          <button
            type="button"
            className="icon-button program-canvas-help"
            aria-label="Program canvas keyboard help"
            title="Focus a node, use arrow keys to navigate, Enter or Space to select, and Delete or Backspace to remove it. Double-click a link, or select it and press Delete, to detach it."
          >
            ?
          </button>
          <ProgramNoticeRail
            notices={notices}
            onDismiss={onDismissNotice}
            onDismissAll={onDismissAllNotices}
          />
          <div className="program-canvas-toolbar">
            <CanvasViewControls
              edgeStyle={view.edgeStyle}
              layoutDirection={view.layoutDirection}
              onEdgeStyleChange={(edgeStyle) => dispatchView({
                type: 'set_edge_style',
                edgeStyle,
              })}
              onLayoutDirectionChange={changeLayoutDirection}
              onAutoLayout={autoLayout}
              onFitView={fitView}
            />
          </div>
        </div>
        <ReactFlow
          nodes={graphNodes}
          edges={graphEdges}
          nodeTypes={NODE_TYPES}
          edgeTypes={EDGE_TYPES}
          onNodesChange={onNodesChange}
          onNodeDragStop={onNodeDragStop}
          onConnect={onConnect}
          onEdgesDelete={(deletedEdges) => {
            const edge = deletedEdges[0];
            if (edge) detachEdge(edge);
          }}
          onEdgeDoubleClick={(event, edge) => {
            event.preventDefault();
            event.stopPropagation();
            detachEdge(edge);
          }}
          onPaneClick={() => {
            setFocusedNodeId(null);
            onSelect(null);
          }}
          onDragOver={onDragOver}
          onDrop={onDrop}
          onInit={(instance) => {
            flowInstance.current = instance;
            instance.fitView({ padding: 0.2 });
          }}
          nodesDraggable
          nodesConnectable={!authoringDisabled}
          elementsSelectable
          nodesFocusable={false}
          deleteKeyCode={['Backspace', 'Delete']}
          multiSelectionKeyCode={null}
          minZoom={0.1}
          maxZoom={2}
          panOnDrag
          zoomOnScroll
          zoomOnPinch
          zoomOnDoubleClick={false}
          fitView
          proOptions={{ hideAttribution: true }}
        >
          <Background color={CANVAS_COLORS.grid} gap={24} />
          <Controls showInteractive />
        </ReactFlow>
        {selectedRow && (
          <div
            className="program-tree-actions"
            aria-label={`Topology actions for ${selectedRow.node.instance_id}`}
          >
            {topologyEntry !== undefined && (
              <div className="program-tree-action-row program-tree-action-row-type-commands">
                <button
                  type="button"
                  disabled={authoringDisabled || !childInsertionAvailable}
                  title={childInsertionAvailable
                    ? 'Insert the chosen type as the final child and open its Properties.'
                    : 'The selected node cannot accept another child.'}
                  onClick={() => { void insertTopologyNode('child'); }}
                >
                  Add child
                </button>
                <button
                  type="button"
                  disabled={authoringDisabled || !siblingInsertionAvailable}
                  title={siblingInsertionAvailable
                    ? 'Insert the chosen type immediately after the selected node.'
                    : 'The selected node parent cannot accept another child.'}
                  onClick={() => { void insertTopologyNode('after'); }}
                >
                  Add after
                </button>
                <button
                  type="button"
                  disabled={authoringDisabled || !wrapperAvailable}
                  title={wrapperAvailable
                    ? 'Wrap the selected subtree atomically with the chosen type.'
                    : 'The chosen node type cannot wrap one child.'}
                  onClick={() => { void wrapSelected(); }}
                >
                  Wrap
                </button>
                <button
                  type="button"
                  disabled={authoringDisabled || !kindReplacementAvailable}
                  title={kindReplacementAvailable
                    ? 'Change type while retaining every compatible authored parameter and all children.'
                    : 'The chosen type is not compatible with the current children and authored parameters.'}
                  onClick={() => { void changeSelectedKind(); }}
                >
                  Change type
                </button>
              </div>
            )}
            <div className="program-tree-action-row program-tree-action-row-placement">
              <label className="inline-select-label program-topology-node-type">
                Node type
                <select
                  value={topologyCatalogId}
                  disabled={authoringDisabled || topologyEntries.length === 0}
                  onChange={(event) => setTopologyCatalogId(event.target.value)}
                >
                  <option value="">Choose a node type</option>
                  {topologyEntries.map((entry) => (
                    <option key={entry.id} value={entry.id}>{entry.name}</option>
                  ))}
                </select>
              </label>
              <button
                type="button"
                disabled={
                  authoringDisabled
                  || selectedRow.parentId === null
                  || selectedRow.index === 0
                }
                onClick={() => {
                  if (selectedRow.parentId !== null) {
                    void moveSelected(selectedRow.parentId, selectedRow.index - 1);
                  }
                }}
              >
                Move earlier
              </button>
              <button
                type="button"
                disabled={
                  authoringDisabled
                  || selectedRow.parentId === null
                  || selectedRow.index >= (
                    flat.find((row) => row.node.instance_id === selectedRow.parentId)
                      ?.node.children.length ?? 1
                  ) - 1
                }
                onClick={() => {
                  if (selectedRow.parentId !== null) {
                    void moveSelected(selectedRow.parentId, selectedRow.index + 1);
                  }
                }}
              >
                Move later
              </button>
              <label className="inline-select-label">
                {selectedRow.parentId === null ? 'Parent' : 'Reparent'}
                <select
                  value=""
                  disabled={authoringDisabled || parentOptions.length === 0}
                  onChange={(event) => {
                    const parent = parentOptions.find(
                      (row) => row.node.instance_id === event.target.value,
                    );
                    if (!parent) return;
                    const result = authoringMoveIntent(
                      laidOut,
                      edges,
                      parent.node.instance_id,
                      selectedRow.node.instance_id,
                    );
                    if (result.intent) {
                      const mutation = programMoveMutation(
                        result.intent.instance_id,
                        result.intent.new_parent_instance_id,
                        result.intent.index,
                      );
                      onMutate(mutation.mutation, mutation.args);
                    }
                  }}
                >
                  <option value="" disabled>Choose parent</option>
                  {parentOptions.map((row) => (
                    <option key={row.node.instance_id} value={row.node.instance_id}>
                      {row.node.instance_id}
                    </option>
                  ))}
                </select>
              </label>
              <button
                type="button"
                className="danger-button"
                disabled={authoringDisabled}
                onClick={() => deleteSelectedNode()}
              >
                Delete
              </button>
            </div>
          </div>
        )}
      </div>
      <span className="sr-only" aria-live="polite">
        Canvas bounds {positions.size} nodes, {graphWidth} by {graphHeight} pixels.
      </span>
    </section>
  );
}
