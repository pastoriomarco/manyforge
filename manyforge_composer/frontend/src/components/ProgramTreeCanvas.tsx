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
  type NodeProps,
  type NodeTypes,
  type ReactFlowInstance,
  type XYPosition,
} from '@xyflow/react';
import '@xyflow/react/dist/style.css';

import { useComposerViewState } from '../store/composerStore';
import type {
  CatalogEntry,
  JsonValue,
  NodeEnvironmentResponse,
  ProgramTreeNode,
} from '../types/api';
import {
  authoringMoveIntent,
  type AuthoringGraphEdge,
  type AuthoringGraphNode,
  layoutAuthoringGraph,
  validateAuthoringConnection,
  validateAuthoringDeletion,
} from '../utils/authoringGraph';
import {
  flattenProgramTree,
} from '../utils/programAuthoring';
import {
  isProgramCanvasFormTarget,
  programCanvasKeyIntent,
  programDeleteMutation,
  programMoveMutation,
  readProgramPaletteDrag,
  stageProgramCanvasDrop,
  type ProgramInsertionRequest,
} from '../utils/programCanvasInteraction';
import { BtNode, type BtNodePresentation } from './BtNode';
import { CanvasViewControls } from './CanvasViewControls';
import { statusEdgePath } from './StatusEdge';

const NODE_WIDTH = 180;
const NODE_HEIGHT = 70;
const STAGE_PADDING = 48;

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
  targetStatus: BtNodePresentation['status'];
}

type ProgramFlowNode = Node<ProgramFlowNodeData, 'programNode'>;
type ProgramFlowEdge = Edge<ProgramFlowEdgeData, 'statusEdge'>;
type ProgramEdgeStyle = 'smoothstep' | 'simplebezier' | 'default' | 'straight' | 'step';

interface ProgramTreeCanvasProps {
  root: ProgramTreeNode;
  catalogEntries: CatalogEntry[];
  environment: NodeEnvironmentResponse | null;
  selectedId: string | null;
  busy: boolean;
  onSelect: (instanceId: string) => void;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
  onStageInsert?: (request: ProgramInsertionRequest) => void;
}

function ProgramFlowNode({ data, selected }: NodeProps<ProgramFlowNode>) {
  return (
    <BtNode
      node={{ ...data.presentation, selected: selected || data.presentation.selected }}
      onSelect={data.onSelect}
      onFocus={data.onFocus}
      onDelete={data.onDelete}
      onNavigate={data.onNavigate}
      elementRef={(element) => data.registerElement(data.presentation.id, element)}
    />
  );
}

function ProgramStatusEdge({
  sourceX,
  sourceY,
  targetX,
  targetY,
  markerEnd,
  data,
}: EdgeProps<ProgramFlowEdge>) {
  const edgeStyle = data?.edgeStyle ?? 'default';
  const stroke = data?.targetStatus === 'running'
    ? '#fbbf24'
    : data?.targetStatus === 'success'
      ? '#4ade80'
      : data?.targetStatus === 'failure'
        ? '#f87171'
        : '#6b7280';
  return (
    <BaseEdge
      path={statusEdgePath(
        { sourceX, sourceY, targetX, targetY },
        edgeStyle,
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
  root,
  catalogEntries,
  environment,
  selectedId,
  busy,
  onSelect,
  onMutate,
  onStageInsert,
}: ProgramTreeCanvasProps) {
  const [view, dispatchView] = useComposerViewState();
  const [presentationPositions, setPresentationPositions] = useState<
    Record<string, XYPosition>
  >({});
  const [focusedNodeId, setFocusedNodeId] = useState<string | null>(null);
  const [canvasMessage, setCanvasMessage] = useState<string | null>(null);
  const flowInstance = useRef<ReactFlowInstance<ProgramFlowNode, ProgramFlowEdge> | null>(null);
  const nodeElements = useRef<Record<string, HTMLDivElement | null>>({});
  const dropRequestId = useRef(0);
  const flat = useMemo(() => flattenProgramTree(root), [root]);
  const catalog = useMemo(
    () => new Map(catalogEntries.map((entry) => [entry.id, entry])),
    [catalogEntries],
  );
  const availability = useMemo(() => {
    const result = new Map<string, { authoring: boolean; execution: boolean }>();
    for (const row of environment?.implementations ?? []) {
      const previous = result.get(row.nodeId) ?? { authoring: false, execution: false };
      result.set(row.nodeId, {
        authoring: previous.authoring || row.authoringAvailable,
        execution: previous.execution || row.executionAvailable,
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
  const laidOut = useMemo(() => {
    const seeds: AuthoringGraphNode[] = flat.map((row, order) => ({
      id: row.node.instance_id,
      label: row.node.instance_id,
      width: NODE_WIDTH,
      height: NODE_HEIGHT,
      childrenMin: catalog.get(row.node.node_id)?.childrenMin,
      childrenMax: catalog.get(row.node.node_id)?.childrenMax,
      position: view.layoutDirection === 'vertical'
        ? { x: order * (NODE_WIDTH + 24), y: row.depth * 150 }
        : { x: row.depth * 250, y: order * (NODE_HEIGHT + 24) },
    }));
    const positioned = layoutAuthoringGraph(
      seeds,
      edges,
      view.layoutDirection,
      view.layoutDirection,
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
  }, [catalog, edges, flat, view.layoutDirection]);
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
  const authoringDisabled = busy || environment?.portableAuthoringAvailable !== true;

  useEffect(() => {
    // Positions are disposable presentation state. A changed owner topology
    // always starts from the same deterministic projection.
    setPresentationPositions({});
  }, [topologyKey, view.layoutDirection]);

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
      || selectedRow.parentId === null
    ) return;
    event?.preventDefault();
    event?.stopPropagation();
    const validationError = validateAuthoringDeletion(
      laidOut,
      edges,
      selectedRow.node.instance_id,
    );
    if (validationError) {
      setCanvasMessage(validationError);
      return;
    }
    const mutation = programDeleteMutation(
      selectedRow.node.instance_id,
      selectedRow.parentId === null,
    );
    if (mutation) onMutate(mutation.mutation, mutation.args);
  }, [authoringDisabled, edges, laidOut, onMutate, selectedRow]);

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
      return {
        id: node.id,
        type: 'programNode',
        position: presentationPositions[node.id] ?? node.position,
        draggable: true,
        selectable: false,
        width: NODE_WIDTH,
        height: NODE_HEIGHT,
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
            status: view.nodeStatuses[node.id] ?? 'idle',
            selected: selectedId === node.id,
            layoutDirection: view.layoutDirection,
            executionAvailable: nodeAvailability?.execution ?? false,
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
      onNodeNavigate,
      onNodeDelete,
      onSelect,
      presentationPositions,
      registerElement,
      selectedId,
      view.layoutDirection,
      view.nodeStatuses,
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
        targetStatus: view.nodeStatuses[edge.target] ?? 'idle',
      },
    })),
    [edges, view.edgeStyle, view.nodeStatuses],
  );
  const positions = useMemo(
    () => new Map(graphNodes.map((node) => [node.id, node.position])),
    [graphNodes],
  );
  const graphWidth = Math.max(
    720,
    ...graphNodes.map((node) => node.position.x + NODE_WIDTH + STAGE_PADDING),
  );
  const graphHeight = Math.max(
    320,
    ...graphNodes.map((node) => node.position.y + NODE_HEIGHT + STAGE_PADDING),
  );

  const onNodesChange = useCallback((
    changes: NodeChange<ProgramFlowNode>[],
  ) => {
    setPresentationPositions((current) => {
      const next = { ...current };
      for (const change of changes) {
        if (change.type === 'position' && change.position) {
          next[change.id] = change.position;
        }
        if (change.type === 'select' && change.selected) {
          onSelect(change.id);
        }
      }
      return next;
    });
  }, [onSelect]);

  const onConnect = useCallback((connection: Connection) => {
    if (authoringDisabled) {
      setCanvasMessage('Draft editing is unavailable while the owner is busy or portable authoring is gated.');
      return;
    }
    const result = authoringMoveIntent(
      laidOut,
      edges,
      connection.source,
      connection.target,
    );
    if (result.error || !result.intent) {
      setCanvasMessage(result.error ?? 'That connection is not valid.');
      return;
    }
    setCanvasMessage(null);
    const mutation = programMoveMutation(
      result.intent.instance_id,
      result.intent.new_parent_instance_id,
      result.intent.index,
    );
    onMutate(mutation.mutation, mutation.args);
  }, [authoringDisabled, edges, laidOut, onMutate]);

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
    const target = graphNodes.find((node) => (
      point.x >= node.position.x
      && point.x <= node.position.x + NODE_WIDTH
      && point.y >= node.position.y
      && point.y <= node.position.y + NODE_HEIGHT
    ));
    if (!target) {
      setCanvasMessage('Drop a palette node on a concrete tree node to choose its insertion target.');
      return;
    }
    const staged = stageProgramCanvasDrop(
      root,
      catalogEntries,
      catalogId,
      target.id,
      dropRequestId.current + 1,
    );
    if (!staged.request) {
      setCanvasMessage(staged.error ?? 'That drop cannot be staged.');
      return;
    }
    onSelect(target.id);
    dropRequestId.current = staged.request.requestId;
    onStageInsert?.(staged.request);
    setCanvasMessage('Insertion staged. Review the instance ID and parameters, then choose Add to Draft.');
  }, [
    authoringDisabled,
    catalogEntries,
    graphNodes,
    onSelect,
    onStageInsert,
    root,
  ]);

  const fitView = useCallback(() => {
    flowInstance.current?.fitView({ padding: 0.2, duration: 180 });
  }, []);
  const autoLayout = useCallback(() => {
    setPresentationPositions({});
    setCanvasMessage('Deterministic layout refreshed. Dragged positions are presentation-only.');
    requestAnimationFrame(fitView);
  }, [fitView]);

  useEffect(() => {
    if (!flowInstance.current) return;
    requestAnimationFrame(fitView);
  }, [fitView, topologyKey, view.layoutDirection]);

  const parentOptions = selectedRow === null || selectedRow.parentId === null
    ? []
    : flat.filter((candidate) =>
      candidate.node.instance_id !== selectedRow.node.instance_id
      && validateAuthoringConnection(
        laidOut,
        edges,
        candidate.node.instance_id,
        selectedRow.node.instance_id,
      ) === null);

  const moveSelected = (newParentId: string, index: number) => {
    if (!selectedRow || authoringDisabled) return;
    const mutation = programMoveMutation(
      selectedRow.node.instance_id,
      newParentId,
      index,
    );
    onMutate(mutation.mutation, mutation.args);
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
        <div className="program-canvas-toolbar">
          <CanvasViewControls
            edgeStyle={view.edgeStyle}
            layoutDirection={view.layoutDirection}
            onEdgeStyleChange={(edgeStyle) => dispatchView({
              type: 'set_edge_style',
              edgeStyle,
            })}
            onLayoutDirectionChange={(layoutDirection) => dispatchView({
              type: 'set_layout_direction',
              layoutDirection,
            })}
            onAutoLayout={autoLayout}
            onFitView={fitView}
          />
          <button
            type="button"
            className="icon-button program-canvas-help"
            aria-label="Program canvas keyboard help"
            title="Focus a node, use arrow keys to navigate, Enter or Space to select, and Delete or Backspace to remove a selected non-root node."
          >
            ?
          </button>
        </div>
        <ReactFlow
          nodes={graphNodes}
          edges={graphEdges}
          nodeTypes={NODE_TYPES}
          edgeTypes={EDGE_TYPES}
          onNodesChange={onNodesChange}
          onConnect={onConnect}
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
          deleteKeyCode={null}
          minZoom={0.1}
          maxZoom={2}
          panOnDrag
          zoomOnScroll
          zoomOnPinch
          fitView
          proOptions={{ hideAttribution: true }}
        >
          <Background color="#29404d" gap={24} />
          <Controls showInteractive />
        </ReactFlow>
        {selectedRow && selectedRow.parentId !== null && (
          <div
            className="program-tree-actions"
            aria-label={`Topology actions for ${selectedRow.node.instance_id}`}
          >
            <button
              type="button"
              disabled={authoringDisabled || selectedRow.index === 0}
              onClick={() => moveSelected(selectedRow.parentId as string, selectedRow.index - 1)}
            >
              Move earlier
            </button>
            <button
              type="button"
              disabled={authoringDisabled || selectedRow.index >= (
                flat.find((row) => row.node.instance_id === selectedRow.parentId)
                  ?.node.children.length ?? 1
              ) - 1}
              onClick={() => moveSelected(selectedRow.parentId as string, selectedRow.index + 1)}
            >
              Move later
            </button>
            <label className="inline-select-label">
              Reparent
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
        )}
        {canvasMessage && (
          <p className="program-inline-message program-canvas-message">{canvasMessage}</p>
        )}
      </div>
      <span className="sr-only" aria-live="polite">
        Canvas bounds {positions.size} nodes, {graphWidth} by {graphHeight} pixels.
      </span>
    </section>
  );
}
