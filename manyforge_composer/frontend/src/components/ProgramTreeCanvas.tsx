import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type DragEvent,
  type KeyboardEvent,
  type MouseEvent as ReactMouseEvent,
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
  type LayoutDirection,
  type NodePresentationStatus,
} from '../store/composerStore';
import { requestOpenSettings } from '../state/settingsNavigation';
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
  type FlatProgramNode,
} from '../utils/programAuthoring';
import {
  isProgramCanvasFormTarget,
  programCanvasKeyIntent,
  programChangeKindMutation,
  programDeleteMutation,
  programDetachedInsertionMutation,
  programPaletteDropPosition,
  programInsertionMutation,
  programMoveMutation,
  programSubtreeCopy,
  programSubtreeInsertionMutation,
  programUnwrapMutation,
  programWrapMutation,
  readProgramPaletteDrag,
  type ProgramInsertionPlacement,
} from '../utils/programCanvasInteraction';
import {
  collapsedProgramItems,
  collapsibleProgramNodeIds,
  defaultCollapsedProgramNodeIds,
  hiddenProgramNodeIds,
} from '../utils/programCollapse';
import { BtNode, type BtNodePresentation } from './BtNode';
import {
  CanvasContextMenu,
  type CanvasMenuEntry,
  type CanvasMenuItem,
} from './CanvasContextMenu';
import { CanvasViewControls } from './CanvasViewControls';
import { useComposerSettings } from '../hooks/useComposerSettings';
import { ProgramCanvasHelp } from './ProgramCanvasHelp';
import {
  ProgramNoticeRail,
  type ProgramNotice,
  type ProgramNoticeInput,
} from './ProgramNoticeRail';
import { statusEdgePath } from './StatusEdge';
import { projectNodeStatuses, projectEdgeStatuses } from './programGraphStatuses';
import { valuePathParameter } from '../utils/valuePaths';
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

// The copied subtree lives for the browser session, across Program switches;
// every paste inserts a fresh copy with new instance ids.
let programClipboard: ProgramTreeNode | null = null;

interface ProgramCanvasMenu {
  x: number;
  y: number;
  /** Where the pointer was on the canvas, for "Paste here" and "Add node here". */
  flowPosition: XYPosition;
  instanceId: string | null;
  /** Set when the menu was opened on a link. */
  edge?: { source: string; target: string };
}

interface ProgramFlowNodeData extends Record<string, unknown> {
  presentation: BtNodePresentation;
  onSelect: (instanceId: string) => void;
  onFocus: (instanceId: string) => void;
  onDelete: (event: KeyboardEvent<HTMLDivElement>, instanceId: string) => void;
  onNavigate: (instanceId: string, key: string) => void;
  registerElement: (instanceId: string, element: HTMLDivElement | null) => void;
  onToggleCollapse: (instanceId: string) => void;
  onItemContextMenu: (event: ReactMouseEvent, instanceId: string) => void;
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
      onToggleCollapse={data.onToggleCollapse}
      onItemContextMenu={data.onItemContextMenu}
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
  const composerSettings = useComposerSettings();
  const edgeStyle = composerSettings.settings.treeEditor.linkStyle;
  // The start direction is an installation setting. The toolbar toggle
  // changes it for this session only (and re-lays out the tree, which is a
  // Program edit).
  const [sessionLayoutDirection, setSessionLayoutDirection] = useState<LayoutDirection | null>(null);
  const view = {
    layoutDirection: sessionLayoutDirection ?? composerSettings.settings.treeEditor.layoutDirection,
  };
  const [presentationPositions, setPresentationPositions] = useState<
    Record<string, XYPosition>
  >({});
  const [nodeMeasurements, setNodeMeasurements] = useState<
    Record<string, { width: number; height: number }>
  >({});
  const [focusedNodeId, setFocusedNodeId] = useState<string | null>(null);
  const [clipboard, setClipboard] = useState<ProgramTreeNode | null>(programClipboard);
  const [menu, setMenu] = useState<ProgramCanvasMenu | null>(null);
  const [helpOpen, setHelpOpen] = useState(false);
  const helpButton = useRef<HTMLButtonElement>(null);
  const closeMenu = useCallback(() => setMenu(null), []);
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
  // Collapsed subtrees are view state only: they never edit the Program. The
  // canvas opens with the nodes the Program flags "collapsed by default"; toggling
  // that flag collapses or expands the node; other clicks stay session-only.
  const [collapsedIds, setCollapsedIds] = useState<ReadonlySet<string>>(
    () => defaultCollapsedProgramNodeIds(roots),
  );
  const defaultCollapsedKey = useMemo(
    () => [...defaultCollapsedProgramNodeIds(roots)].sort().join('\u0000'),
    [roots],
  );
  const previousDefaultCollapsed = useRef(defaultCollapsedKey);
  useEffect(() => {
    const previous = new Set(previousDefaultCollapsed.current.split('\u0000').filter(Boolean));
    const current = new Set(defaultCollapsedKey.split('\u0000').filter(Boolean));
    previousDefaultCollapsed.current = defaultCollapsedKey;
    const added = [...current].filter((id) => !previous.has(id));
    const removed = [...previous].filter((id) => !current.has(id));
    if (added.length === 0 && removed.length === 0) return;
    setCollapsedIds((ids) => {
      const next = new Set(ids);
      added.forEach((id) => next.add(id));
      removed.forEach((id) => next.delete(id));
      return next;
    });
  }, [defaultCollapsedKey]);
  const collapsible = useMemo(() => collapsibleProgramNodeIds(roots), [roots]);
  const collapsed = useMemo(
    () => new Set([...collapsedIds].filter((id) => collapsible.has(id))),
    [collapsedIds, collapsible],
  );
  const hidden = useMemo(() => hiddenProgramNodeIds(roots, collapsed), [collapsed, roots]);
  const collapseActive = collapsed.size > 0;
  const collapseActiveRef = useRef(collapseActive);
  collapseActiveRef.current = collapseActive;
  const toggleCollapse = useCallback((instanceId: string) => {
    setCollapsedIds((current) => {
      const next = new Set(current);
      if (next.has(instanceId)) next.delete(instanceId);
      else next.add(instanceId);
      return next;
    });
  }, []);
  // Collapse all and Expand all change the whole layout, so they refit the
  // view once the new sizes are measured; a single collapse keeps the view.
  const refitAfterLayout = useCallback(() => {
    globalThis.setTimeout(() => {
      flowInstance.current?.fitView({ padding: 0.2, duration: 180 });
    }, 120);
  }, []);
  const collapseAll = useCallback(() => {
    setCollapsedIds(new Set(collapsible));
    refitAfterLayout();
  }, [collapsible, refitAfterLayout]);
  const expandAll = useCallback(() => {
    setCollapsedIds(new Set());
    refitAfterLayout();
  }, [refitAfterLayout]);
  const layoutNodes = useCallback((
    layoutDirection: LayoutDirection,
    orderedIds: string[] = flat.map((row) => row.node.instance_id),
    rows: FlatProgramNode[] = flat,
    graphEdges: AuthoringGraphEdge[] = edges,
  ) => {
    // The view names the direction in which siblings are read. The layout
    // utility names the perpendicular graph-growth axis.
    const graphDirection = layoutDirection === 'vertical'
      ? 'horizontal'
      : 'vertical';
    const orderById = new Map(orderedIds.map((id, order) => [id, order]));
    const seeds: AuthoringGraphNode[] = rows.map((row, fallbackOrder) => {
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
      graphEdges,
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
  // While anything is collapsed the canvas shows an automatic layout of the
  // visible nodes, sized by their measurements (a collapsed node is taller and
  // may be wider). It is not saved; expanding everything restores the
  // Program's own positions. Drags while collapsed move only this view.
  const collapsedLayout = useMemo(() => {
    if (!collapseActive) return null;
    const visibleRows = flat.filter((row) => !hidden.has(row.node.instance_id));
    return layoutNodes(
      view.layoutDirection,
      undefined,
      visibleRows,
      edges.filter((edge) => !hidden.has(edge.target)),
    );
  }, [collapseActive, edges, flat, hidden, layoutNodes, view.layoutDirection]);
  const [collapsedDrag, setCollapsedDrag] = useState<Record<string, XYPosition>>({});
  // A different collapsed set lays the view out again from scratch.
  const collapsedKey = [...collapsed].sort().join('|');
  useEffect(() => setCollapsedDrag({}), [collapsedKey]);
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
  // The chosen type belongs to the current selection; a new or cleared
  // selection starts without one.
  useEffect(() => setTopologyCatalogId(''), [selectedId]);
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

  const deleteRow = useCallback((row: FlatProgramNode) => {
    if (authoringDisabled) return;
    const validationError = validateAuthoringDeletion(laidOut, row.node.instance_id);
    if (validationError) {
      showCanvasMessage(validationError, 'error');
      return;
    }
    const mutation = programDeleteMutation(row.node.instance_id);
    onMutate(mutation.mutation, mutation.args);
  }, [authoringDisabled, laidOut, onMutate, showCanvasMessage]);

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
    deleteRow(selectedRow);
  }, [authoringDisabled, deleteRow, selectedRow]);

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

  const describeCollapsedItem = useCallback((child: ProgramTreeNode) => ({
    id: child.instance_id,
    label: child.instance_id,
    catalogId: child.node_id,
    category: catalog.get(child.node_id)?.category ?? 'unknown',
    status: 'idle' as const,
    invalid: invalidNodeIds.has(child.instance_id),
    selected: selectedId === child.instance_id,
  }), [catalog, invalidNodeIds, selectedId]);
  const openItemMenu = useCallback((event: ReactMouseEvent, instanceId: string) => {
    openMenuRef.current(event, instanceId);
  }, []);
  const baseGraphNodes = useMemo<ProgramFlowNode[]>(
    () => (collapsedLayout ?? laidOut).filter((node) => !hidden.has(node.id)).map((node) => {
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
        position: collapsedLayout !== null
          ? collapsedDrag[node.id] ?? node.position
          : presentationPositions[node.id] ?? retainedPositions[node.id] ?? node.position,
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
            status: 'idle',
            selected: selectedId === node.id,
            layoutDirection: view.layoutDirection,
            executionAvailable: nodeAvailability?.execution ?? false,
            executionDisposition,
            executionReason: nodeAvailability?.reasons[0]?.replaceAll('_', ' '),
            invalid: invalidNodeIds.has(node.id),
            collapsible: collapsible.has(node.id),
            collapsedItems: collapsed.has(node.id) && row !== undefined
              ? collapsedProgramItems(row.node, describeCollapsedItem)
              : null,
          },
          onSelect,
          onFocus: setFocusedNodeId,
          onDelete: onNodeDelete,
          onNavigate: onNodeNavigate,
          registerElement,
          onToggleCollapse: toggleCollapse,
          onItemContextMenu: openItemMenu,
        },
      };
    }),
    [
      availability,
      catalog,
      collapsed,
      collapsedDrag,
      collapsedLayout,
      collapsible,
      describeCollapsedItem,
      hidden,
      openItemMenu,
      toggleCollapse,
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
    ],
  );
  const nodeStatusProjector = useMemo(() => projectNodeStatuses(baseGraphNodes), [baseGraphNodes]);
  const graphNodes = useMemo(() => nodeStatusProjector(nodeStatuses), [nodeStatusProjector, nodeStatuses]);
  const baseGraphEdges = useMemo<ProgramFlowEdge[]>(
    () => edges.filter((edge) => !hidden.has(edge.target)).map((edge) => ({
      id: `${edge.source}:${edge.target}`,
      source: edge.source,
      target: edge.target,
      type: 'statusEdge',
      data: {
        edgeStyle,
        layoutDirection: view.layoutDirection,
        targetStatus: 'idle',
      },
    })),
    [edgeStyle, edges, hidden, view.layoutDirection],
  );
  const edgeStatusProjector = useMemo(() => projectEdgeStatuses(baseGraphEdges), [baseGraphEdges]);
  const graphEdges = useMemo(() => edgeStatusProjector(nodeStatuses), [edgeStatusProjector, nodeStatuses]);
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
      if (collapseActiveRef.current) setCollapsedDrag((current) => ({ ...current, ...pending }));
      else setPresentationPositions((current) => ({ ...current, ...pending }));
    };
    if (typeof globalThis.requestAnimationFrame !== 'function') {
      flushPositions();
      return;
    }
    positionFrame.current = globalThis.requestAnimationFrame(flushPositions);
  }, [onSelect]);

  const onNodeDragStop = useCallback<NodeMouseHandler<ProgramFlowNode>>((_, node) => {
    delete pendingPositionChanges.current[node.id];
    if (collapseActive) {
      setCollapsedDrag((current) => ({ ...current, [node.id]: node.position }));
    } else {
      setPresentationPositions((current) => ({ ...current, [node.id]: node.position }));
    }
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
        // Positions of a collapsed view are not the Program's layout.
        const mutation = programMoveMutation(
          node.id,
          row.parentId,
          index,
          collapseActive ? undefined : Object.fromEntries(nextPositions),
        );
        onMutate(mutation.mutation, mutation.args);
        return;
      }
    }
    if (collapseActive) return;
    onMutate('tree_draft_update_positions', {
      positions: { [node.id]: { x: node.position.x, y: node.position.y } },
    });
  }, [authoringDisabled, collapseActive, edges, flatById, graphDirection, graphNodes, onMutate, positions]);

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
      collapseActive ? undefined : Object.fromEntries(positions),
    );
    onMutate(mutation.mutation, mutation.args);
  }, [
    authoringDisabled,
    collapseActive,
    edges,
    graphDirection,
    graphNodes,
    laidOut,
    onMutate,
    positions,
    showCanvasMessage,
  ]);

  const detachEdge = useCallback((edge: { target: string }) => {
    if (authoringDisabled) return;
    const mutation = programMoveMutation(
      edge.target,
      null,
      roots.length,
      collapseActive ? undefined : Object.fromEntries(positions),
    );
    onMutate(mutation.mutation, mutation.args);
  }, [authoringDisabled, collapseActive, onMutate, positions, roots.length]);

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
    const point = programPaletteDropPosition(
      flow.screenToFlowPosition({ x: event.clientX, y: event.clientY }),
      NODE_SIZE,
    );
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
    setSessionLayoutDirection(layoutDirection);
    applyLayout(
      layoutNodes(layoutDirection),
      `${layoutDirection === 'vertical' ? 'Vertical' : 'Horizontal'} layout applied.`,
    );
  }, [applyLayout, layoutNodes, view.layoutDirection]);

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

  const moveRow = async (movedRow: FlatProgramNode, newParentId: string, index: number) => {
    if (authoringDisabled) return;
    const orderedIds = authoringReorderedIds(
      flat.map((row) => ({ id: row.node.instance_id, parentId: row.parentId })),
      newParentId,
      movedRow.node.instance_id,
      index,
    );
    if (orderedIds === null) return;
    const aligned = layoutNodes(view.layoutDirection, orderedIds);
    const nextPositions = Object.fromEntries(
      aligned.map((node) => [node.id, node.position]),
    );
    const mutation = programMoveMutation(
      movedRow.node.instance_id,
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
  const moveSelected = async (newParentId: string, index: number) => {
    if (selectedRow !== null) await moveRow(selectedRow, newParentId, index);
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
        ...Object.keys(selectedRow.node.value_sources ?? {}).map(valuePathParameter),
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

  // ---- Right-click menu and copy/paste shortcuts -------------------------

  const instanceIds = flat.map((row) => row.node.instance_id);
  const parentEntryOf = (row: FlatProgramNode) => {
    const parent = row.parentId === null ? null : flatById.get(row.parentId) ?? null;
    return { parent, entry: parent === null ? undefined : catalog.get(parent.node.node_id) };
  };
  const canInsertAfter = (row: FlatProgramNode) => {
    const { parent, entry } = parentEntryOf(row);
    return parent === null || acceptsChildCount(entry, parent.node.children.length + 1);
  };
  const canInsertChild = (row: FlatProgramNode) => acceptsChildCount(
    catalog.get(row.node.node_id),
    row.node.children.length + 1,
  );

  const insertSubtree = async (
    subtree: ProgramTreeNode,
    placement: ProgramInsertionPlacement,
    message: string,
  ) => {
    if (authoringDisabled) return;
    const mutation = programSubtreeInsertionMutation(subtree, placement);
    if (await onMutate(mutation.mutation, mutation.args) === null) return;
    onSelect(subtree.instance_id);
    showCanvasMessage(message, 'info');
  };
  const duplicateRow = (row: FlatProgramNode) => {
    if (!canInsertAfter(row)) {
      showCanvasMessage('The parent cannot take another child, so the node cannot be duplicated here.', 'error');
      return;
    }
    void insertSubtree(
      programSubtreeCopy(row.node, instanceIds),
      { kind: 'after', siblingInstanceId: row.node.instance_id },
      `${row.node.instance_id} duplicated.`,
    );
  };
  const copyRow = (row: FlatProgramNode) => {
    programClipboard = structuredClone(row.node);
    setClipboard(programClipboard);
    showCanvasMessage(`${row.node.instance_id} copied.`, 'info');
  };
  const pasteAt = (placement: ProgramInsertionPlacement) => {
    if (clipboard === null) return;
    void insertSubtree(
      programSubtreeCopy(clipboard, instanceIds),
      placement,
      `${clipboard.instance_id} pasted.`,
    );
  };
  const pasteAfter = (row: FlatProgramNode) => {
    if (!canInsertAfter(row)) {
      showCanvasMessage('The parent cannot take another child here.', 'error');
      return;
    }
    pasteAt({ kind: 'after', siblingInstanceId: row.node.instance_id });
  };
  const pasteAsRoot = (position: XYPosition) => {
    pasteAt({ kind: 'root', index: roots.length, position });
  };
  const wrapRow = async (row: FlatProgramNode, entry: CatalogEntry) => {
    if (authoringDisabled) return;
    const mutation = programWrapMutation(entry, instanceIds, row.node.instance_id);
    if (await onMutate(mutation.mutation, mutation.args) === null) return;
    const wrapper = mutation.args.wrapper as Record<string, JsonValue>;
    onSelect(String(wrapper.instance_id));
    showCanvasMessage(`${entry.name} now wraps ${row.node.instance_id}.`, 'info');
  };
  const unwrapRow = async (row: FlatProgramNode) => {
    const child = row.node.children[0];
    if (authoringDisabled || child === undefined || row.node.children.length !== 1) return;
    const mutation = programUnwrapMutation(row.node);
    if (await onMutate(mutation.mutation, mutation.args) === null) return;
    onSelect(child.instance_id);
    showCanvasMessage(`${row.node.instance_id} removed; its child keeps its place.`, 'info');
  };
  const changeRowKind = async (row: FlatProgramNode, entry: CatalogEntry) => {
    if (authoringDisabled) return;
    const mutation = programChangeKindMutation(entry, row.node.instance_id, [
      ...Object.keys(row.node.parameters),
      ...Object.keys(row.node.value_sources ?? {}).map(valuePathParameter),
    ]);
    if (await onMutate(mutation.mutation, mutation.args) === null) return;
    showCanvasMessage(`Node type changed to ${entry.name} without dropping parameters.`, 'info');
  };
  const detachRow = (row: FlatProgramNode) => {
    if (authoringDisabled || row.parentId === null) return;
    const mutation = programMoveMutation(
      row.node.instance_id,
      null,
      roots.length,
      Object.fromEntries(positions),
    );
    onMutate(mutation.mutation, mutation.args);
  };
  const insertCatalogNodeAt = (entry: CatalogEntry, position: XYPosition) => {
    if (authoringDisabled) return;
    const mutation = programDetachedInsertionMutation(entry, instanceIds, roots.length, position);
    onMutate(mutation.mutation, mutation.args);
    onSelect(String((mutation.args.node as Record<string, JsonValue>).instance_id));
  };

  const byKindThenName = (left: CatalogEntry, right: CatalogEntry) => (
    (left.kind === 'decorator' ? 0 : 1) - (right.kind === 'decorator' ? 0 : 1)
    || left.name.localeCompare(right.name)
  );
  const wrapperEntries = topologyEntries
    .filter((entry) => acceptsChildCount(entry, 1))
    .sort(byKindThenName);

  const nodeMenuEntries = (row: FlatProgramNode): CanvasMenuEntry[] => {
    const retained = [
      ...Object.keys(row.node.parameters),
      ...Object.keys(row.node.value_sources ?? {}).map(valuePathParameter),
    ];
    const replacements = topologyEntries
      .filter((entry) => entry.id !== row.node.node_id
        && acceptsChildCount(entry, row.node.children.length)
        && retained.every((name) => entry.parameters.some((parameter) => parameter.id === name)))
      .sort(byKindThenName);
    const siblings = row.parentId === null
      ? roots.length
      : flatById.get(row.parentId)?.node.children.length ?? 1;
    const item = (label: string, onSelectItem: () => void, disabled = false, extra: Partial<CanvasMenuItem> = {}): CanvasMenuItem => ({
      label,
      onSelect: onSelectItem,
      disabled: authoringDisabled || disabled,
      ...extra,
    });
    return [
      ...collapseMenuEntries(row),
      'separator',
      item('Duplicate', () => duplicateRow(row), !canInsertAfter(row), { shortcut: 'Ctrl+D' }),
      { label: 'Copy', onSelect: () => copyRow(row), shortcut: 'Ctrl+C' },
      item('Paste after', () => pasteAfter(row), clipboard === null || !canInsertAfter(row), { shortcut: 'Ctrl+V' }),
      item('Paste as child', () => pasteAt({
        kind: 'child',
        parentInstanceId: row.node.instance_id,
        index: row.node.children.length,
      }), clipboard === null || !canInsertChild(row)),
      'separator',
      {
        label: 'Wrap in',
        disabled: authoringDisabled,
        items: wrapperEntries.map((entry) => item(entry.name, () => { void wrapRow(row, entry); })),
      },
      item('Unwrap', () => { void unwrapRow(row); }, row.node.children.length !== 1, {
        title: 'Remove this node and put its only child in its place.',
      }),
      {
        label: 'Change type',
        disabled: authoringDisabled,
        items: replacements.map((entry) => item(entry.name, () => { void changeRowKind(row, entry); })),
      },
      'separator',
      item('Move earlier', () => {
        if (row.parentId !== null) void moveRow(row, row.parentId, row.index - 1);
      }, row.parentId === null || row.index === 0),
      item('Move later', () => {
        if (row.parentId !== null) void moveRow(row, row.parentId, row.index + 1);
      }, row.parentId === null || row.index >= siblings - 1),
      item('Detach', () => detachRow(row), row.parentId === null, {
        title: 'Disconnect this subtree from its parent and keep it as a separate root.',
      }),
      'separator',
      item('Delete', () => deleteRow(row), false, { danger: true, shortcut: 'Del' }),
    ];
  };
  /** Collapse entries of a node menu; on a node listed inside a collapsed
   * one, the first entry brings it back onto the canvas instead. */
  const collapseMenuEntries = (row: FlatProgramNode): CanvasMenuEntry[] => {
    const id = row.node.instance_id;
    const ancestors: string[] = [];
    for (let parent = row.parentId; parent !== null; parent = flatById.get(parent)?.parentId ?? null) {
      ancestors.push(parent);
    }
    const own: CanvasMenuItem = hidden.has(id)
      ? {
          label: 'Show on canvas',
          title: 'Expand the collapsed nodes above this one.',
          onSelect: () => setCollapsedIds((current) => new Set(
            [...current].filter((candidate) => !ancestors.includes(candidate)),
          )),
        }
      : !collapsible.has(id)
        ? { label: 'Collapse', disabled: true, title: 'Only nodes with children can collapse.' }
        : collapsed.has(id)
          ? { label: 'Expand', onSelect: () => toggleCollapse(id) }
          : { label: 'Collapse', onSelect: () => toggleCollapse(id) };
    return [
      own,
      { label: 'Collapse all', onSelect: collapseAll, disabled: collapsible.size === 0 },
      { label: 'Expand all', onSelect: expandAll, disabled: !collapseActive },
    ];
  };
  const paneMenuEntries = (position: XYPosition): CanvasMenuEntry[] => [
    {
      label: 'Paste here',
      onSelect: () => pasteAsRoot(position),
      disabled: authoringDisabled || clipboard === null,
      shortcut: 'Ctrl+V',
    },
    {
      label: 'Add node here',
      disabled: authoringDisabled,
      items: [...topologyEntries]
        .sort((left, right) => left.name.localeCompare(right.name))
        .map((entry) => ({
          label: entry.name,
          onSelect: () => insertCatalogNodeAt(entry, position),
        })),
    },
    'separator',
    { label: 'Auto-layout', onSelect: autoLayout },
    { label: 'Fit view', onSelect: fitView },
    'separator',
    { label: 'Collapse all', onSelect: collapseAll, disabled: collapsible.size === 0 },
    { label: 'Expand all', onSelect: expandAll, disabled: !collapseActive },
  ];
  const edgeMenuEntries = (edge: { source: string; target: string }): CanvasMenuEntry[] => [
    {
      label: 'Disconnect',
      disabled: authoringDisabled,
      title: `Detach ${edge.target} and its subtree from ${edge.source}; it stays on the canvas as a separate root.`,
      onSelect: () => detachEdge(edge),
    },
  ];

  const openMenu = (
    event: ReactMouseEvent | globalThis.MouseEvent,
    instanceId: string | null,
  ) => {
    event.preventDefault();
    const flow = flowInstance.current;
    const flowPosition = flow?.screenToFlowPosition({ x: event.clientX, y: event.clientY })
      ?? { x: 0, y: 0 };
    if (instanceId !== null) {
      // Right-click selects the node too, which opens its Properties.
      onSelect(instanceId);
      setFocusedNodeId(instanceId);
    }
    setMenu({ x: event.clientX, y: event.clientY, flowPosition, instanceId });
  };
  const openMenuRef = useRef(openMenu);
  openMenuRef.current = openMenu;
  const openEdgeMenu = (event: ReactMouseEvent, edge: ProgramFlowEdge) => {
    event.preventDefault();
    setMenu({
      x: event.clientX,
      y: event.clientY,
      flowPosition: { x: 0, y: 0 },
      instanceId: null,
      edge: { source: edge.source, target: edge.target },
    });
  };

  const onCanvasShortcut = (event: KeyboardEvent<HTMLDivElement>) => {
    if (!(event.ctrlKey || event.metaKey) || event.altKey || event.shiftKey) return;
    if (isProgramCanvasFormTarget(event.target)) return;
    const key = event.key.toLocaleLowerCase();
    if (key !== 'c' && key !== 'd' && key !== 'v') return;
    if (key === 'v') {
      event.preventDefault();
      if (clipboard === null) return;
      if (selectedRow !== null) {
        pasteAfter(selectedRow);
      } else {
        const bounds = event.currentTarget.getBoundingClientRect();
        pasteAsRoot(flowInstance.current?.screenToFlowPosition({
          x: bounds.left + bounds.width / 2,
          y: bounds.top + bounds.height / 2,
        }) ?? { x: 0, y: 0 });
      }
      return;
    }
    if (selectedRow === null) return;
    event.preventDefault();
    if (key === 'c') copyRow(selectedRow);
    else duplicateRow(selectedRow);
  };
  const menuRow = menu?.instanceId == null ? null : flatById.get(menu.instanceId) ?? null;

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
          onCanvasShortcut(event);
        }}
      >
        <div className="program-canvas-topbar">
          <ProgramNoticeRail
            notices={notices}
            onDismiss={onDismissNotice}
            onDismissAll={onDismissAllNotices}
          />
          <div className="program-canvas-toolbar">
            <CanvasViewControls
              edgeStyle={edgeStyle}
              layoutDirection={view.layoutDirection}
              onEdgeStyleChange={(linkStyle) => composerSettings.update((current) => ({
                ...current,
                treeEditor: { ...current.treeEditor, linkStyle },
              }))}
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
          onNodeContextMenu={(event, node) => openMenu(event, node.id)}
          onPaneContextMenu={(event) => openMenu(event, null)}
          onEdgeContextMenu={openEdgeMenu}
          onMoveStart={closeMenu}
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
        {/* Same pattern as the 3D viewer: information and settings. */}
        <div className="surface-corner-actions program-canvas-corner-actions">
          <button
            ref={helpButton}
            type="button"
            className="icon-button"
            aria-label="Tree editor information"
            aria-haspopup="dialog"
            title="How to use the tree editor, colours and collapsing"
            onClick={() => setHelpOpen(true)}
          >i</button>
          <button
            type="button"
            className="icon-button"
            aria-label="Program settings"
            aria-haspopup="dialog"
            title="Program settings, including how the tree editor draws it"
            onClick={() => requestOpenSettings('program')}
          ><span className="transport-settings-glyph" aria-hidden="true">⚙</span></button>
        </div>
        {menu !== null && menu.edge !== undefined && (
          <CanvasContextMenu
            x={menu.x}
            y={menu.y}
            heading={`${menu.edge.source} → ${menu.edge.target}`}
            label={`Actions for the link from ${menu.edge.source} to ${menu.edge.target}`}
            entries={edgeMenuEntries(menu.edge)}
            onClose={closeMenu}
          />
        )}
        {menu !== null && menu.edge === undefined && (menu.instanceId === null || menuRow !== null) && (
          <CanvasContextMenu
            x={menu.x}
            y={menu.y}
            heading={menuRow?.node.instance_id}
            label={menuRow === null ? 'Program canvas actions' : `Actions for ${menuRow.node.instance_id}`}
            entries={menuRow === null ? paneMenuEntries(menu.flowPosition) : nodeMenuEntries(menuRow)}
            onClose={closeMenu}
          />
        )}
        {selectedRow && (
          <div
            className="program-tree-actions"
            aria-label={`Topology actions for ${selectedRow.node.instance_id}`}
          >
            <div className="program-tree-action-row program-tree-action-row-type">
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
              {topologyEntry !== undefined && (
                <>
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
                </>
              )}
            </div>
            <div className="program-tree-action-row program-tree-action-row-placement">
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
      <ProgramCanvasHelp
        open={helpOpen}
        onClose={() => setHelpOpen(false)}
        returnFocusRef={helpButton}
      />
      <span className="sr-only" aria-live="polite">
        Canvas bounds {positions.size} nodes, {graphWidth} by {graphHeight} pixels.
      </span>
    </section>
  );
}
