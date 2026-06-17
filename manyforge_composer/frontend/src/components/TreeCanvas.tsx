import { useCallback, useEffect, useRef } from 'react';
import {
  ReactFlow,
  Background,
  Controls,
  addEdge,
  applyNodeChanges,
  applyEdgeChanges,
} from '@xyflow/react';
import type { Connection, NodeTypes, EdgeTypes, Node, Edge, ReactFlowInstance } from '@xyflow/react';
import '@xyflow/react/dist/style.css';

import { BtNode } from './BtNode';
import { StatusEdge } from './StatusEdge';
import { useProgramStore } from '../store/programStore';
import { useCycleStore } from '../store/cycleStore';
import { useCatalogStore } from '../store/catalogStore';

const nodeTypes: NodeTypes = { btNode: BtNode };
const edgeTypes: EdgeTypes = { statusEdge: StatusEdge };
const DROP_NODE_WIDTH = 180;
const DROP_NODE_HEIGHT = 64;

export function TreeCanvas() {
  const catalogEntries = useCatalogStore((state) => state.entries);
  const {
    nodes: storeNodes,
    edges: storeEdges,
    addNode,
    setNodes,
    setEdges,
    setError,
    selectNode,
    removeNode,
    removeEdge,
    layoutRevision,
  } = useProgramStore();
  const cycleState = useCycleStore((state) => state.state);
  const reactFlowWrapper = useRef<HTMLDivElement>(null);
  const reactFlowInstance = useRef<ReactFlowInstance<Node, Edge> | null>(null);
  const isRunning = cycleState === 'running' || cycleState === 'stopping';

  const validateConnection = useCallback(
    (connection: Connection | Edge): string | null => {
      if (!connection.source || !connection.target) {
        return 'Connections must include both a parent and a child node';
      }
      if (connection.source === connection.target) {
        return 'A node cannot be its own parent';
      }
      const sourceNode = storeNodes.find((node) => node.id === connection.source);
      const targetNode = storeNodes.find((node) => node.id === connection.target);
      if (!sourceNode || !targetNode) {
        return 'Connection references an unknown node';
      }
      if (storeEdges.some((edge) => edge.source === connection.source && edge.target === connection.target)) {
        return 'That link already exists';
      }
      if (storeEdges.some((edge) => edge.target === connection.target)) {
        const targetLabel = String((targetNode.data as { label?: string }).label || connection.target);
        return `Node '${targetLabel}' already has a parent`;
      }

      const sourceCatalogId = String((sourceNode.data as { catalogId?: string }).catalogId || sourceNode.id);
      const sourceEntry = catalogEntries.find((entry) => entry.id === sourceCatalogId);
      if (!sourceEntry) {
        return `Unknown catalog entry '${sourceCatalogId}'`;
      }
      const currentChildCount = storeEdges.filter((edge) => edge.source === connection.source).length;
      if (sourceEntry.childrenMax === 0) {
        return `Node '${String((sourceNode.data as { label?: string }).label || sourceCatalogId)}' does not allow child nodes`;
      }
      if (sourceEntry.childrenMax >= 0 && currentChildCount >= sourceEntry.childrenMax) {
        return `Node '${String((sourceNode.data as { label?: string }).label || sourceCatalogId)}' accepts at most ${sourceEntry.childrenMax} child${sourceEntry.childrenMax === 1 ? '' : 'ren'}`;
      }

      const adjacency = new Map<string, string[]>();
      for (const edge of storeEdges) {
        const children = adjacency.get(edge.source) || [];
        children.push(edge.target);
        adjacency.set(edge.source, children);
      }
      const stack = [connection.target];
      const seen = new Set<string>();
      while (stack.length > 0) {
        const nodeId = stack.pop()!;
        if (nodeId === connection.source) {
          return 'That connection would create a cycle';
        }
        if (seen.has(nodeId)) {
          continue;
        }
        seen.add(nodeId);
        for (const childId of adjacency.get(nodeId) || []) {
          stack.push(childId);
        }
      }

      return null;
    },
    [catalogEntries, storeEdges, storeNodes],
  );

  const onConnect = useCallback(
    (connection: Connection) => {
      const validationError = validateConnection(connection);
      if (validationError) {
        setError(validationError);
        return;
      }
      setError(null);
      setEdges((edges) => addEdge({ ...connection, type: 'statusEdge' }, edges));
    },
    [setEdges, setError, validateConnection],
  );

  // Handle drop from palette
  const onDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  }, []);

  const onDrop = useCallback(
    (e: React.DragEvent) => {
      e.preventDefault();
      if (isRunning) return;
      
      const catalogId = e.dataTransfer.getData('application/manyforge-node');
      if (!catalogId) return;

      const flow = reactFlowInstance.current;
      if (!flow) return;
      const flowPosition = flow.screenToFlowPosition({
        x: e.clientX,
        y: e.clientY,
      });
      const position = {
        x: flowPosition.x - DROP_NODE_WIDTH / 2,
        y: flowPosition.y - DROP_NODE_HEIGHT / 2,
      };

      setError(null);
      addNode(catalogId, position);
    },
    [addNode, isRunning, setError],
  );

  const handleInit = useCallback((instance: ReactFlowInstance<Node, Edge>) => {
    reactFlowInstance.current = instance;
    instance.fitView({ padding: 0.2 });
  }, []);

  useEffect(() => {
    if (!reactFlowInstance.current) {
      return;
    }
    window.requestAnimationFrame(() => {
      reactFlowInstance.current?.fitView({ duration: 250, padding: 0.2 });
    });
  }, [layoutRevision]);

  const syncSelectedNode = useCallback(
    (nodes: Node[]) => {
      const selected = nodes.find((node) => node.selected);
      selectNode(selected?.id ?? null);
    },
    [selectNode],
  );

  // Determine if editing is enabled based on mode
  return (
    <div ref={reactFlowWrapper} className="h-full min-h-0 w-full flex-1">
      <ReactFlow
        nodes={storeNodes}
        edges={storeEdges}
        edgeTypes={edgeTypes}
        onNodesChange={(changes) => {
          if (isRunning) {
            const safeChanges = changes.filter(c => c.type === 'select');
            if (safeChanges.length > 0) {
              setNodes(
                (nodes) => {
                  const nextNodes = applyNodeChanges(safeChanges, nodes);
                  syncSelectedNode(nextNodes);
                  return nextNodes;
                },
                { dirty: false },
              );
            }
            return;
          }
          const shouldMarkDirty = changes.some(c => c.type !== 'select');
          setNodes(
            (nodes) => {
              const nextNodes = applyNodeChanges(changes, nodes);
              syncSelectedNode(nextNodes);
              return nextNodes;
            },
            { dirty: shouldMarkDirty },
          );
        }}
        onEdgesChange={(changes) => {
          if (isRunning) return;
          const shouldMarkDirty = changes.some(c => c.type !== 'select');
          setEdges(
            (edges) => applyEdgeChanges(changes, edges),
            { dirty: shouldMarkDirty },
          );
        }}
        onNodesDelete={!isRunning ? (deleted) => {
          for (const node of deleted) {
            removeNode(node.id);
          }
        } : undefined}
        onEdgesDelete={!isRunning ? (deleted) => {
          for (const edge of deleted) {
            removeEdge(edge.id);
          }
        } : undefined}
        onEdgeDoubleClick={!isRunning ? (_event, edge) => {
          removeEdge(edge.id);
        } : undefined}
        isValidConnection={!isRunning ? (connection) => validateConnection(connection) === null : undefined}
        onConnect={!isRunning ? onConnect : undefined}
        onDragOver={onDragOver}
        onDrop={onDrop}
        onInit={handleInit}
        nodeTypes={nodeTypes}
        nodesDraggable={!isRunning}
        nodesConnectable={!isRunning}
        elementsSelectable={true}
        edgesFocusable={!isRunning}
        deleteKeyCode="Delete"
        minZoom={0.05}
        maxZoom={2}
        defaultEdgeOptions={{
          type: 'statusEdge',
          animated: false,
        }}
        proOptions={{ hideAttribution: true }}
        className="bg-gray-900"
      >
        <Background color="#374151" gap={20} />
        <Controls />
      </ReactFlow>
    </div>
  );
}
