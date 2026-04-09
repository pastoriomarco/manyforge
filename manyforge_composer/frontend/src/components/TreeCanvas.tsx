import { useCallback, useEffect, useRef } from 'react';
import {
  ReactFlow,
  Background,
  Controls,
  addEdge,
  applyNodeChanges,
  applyEdgeChanges,
} from '@xyflow/react';
import type { Connection, NodeTypes, EdgeTypes, Node } from '@xyflow/react';
import '@xyflow/react/dist/style.css';

import { BtNode } from './BtNode';
import { StatusEdge } from './StatusEdge';
import { useProgramStore } from '../store/programStore';
import { useCycleStore } from '../store/cycleStore';

const nodeTypes: NodeTypes = { btNode: BtNode };
const edgeTypes: EdgeTypes = { statusEdge: StatusEdge };

export function TreeCanvas() {
  const {
    nodes: storeNodes,
    edges: storeEdges,
    addNode,
    setNodes,
    setEdges,
    selectNode,
    removeNode,
    layoutRevision,
  } = useProgramStore();
  const cycleState = useCycleStore((state) => state.state);
  const reactFlowWrapper = useRef<HTMLDivElement>(null);
  const reactFlowInstance = useRef<{ fitView: (options?: { duration?: number; padding?: number }) => void } | null>(null);
  const isRunning = cycleState === 'running';

  const onConnect = useCallback(
    (connection: Connection) => {
      setEdges((edges) => addEdge({ ...connection, type: 'statusEdge' }, edges));
    },
    [setEdges],
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

      const bounds = reactFlowWrapper.current?.getBoundingClientRect();
      if (!bounds) return;

      const position = {
        x: e.clientX - bounds.left,
        y: e.clientY - bounds.top,
      };

      addNode(catalogId, position);
    },
    [addNode, isRunning],
  );

  const handleInit = useCallback((instance: { fitView: (options?: { duration?: number; padding?: number }) => void }) => {
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
