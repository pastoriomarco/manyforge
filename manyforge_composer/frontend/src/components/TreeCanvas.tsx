import { useCallback, useRef, useMemo, useEffect } from 'react';
import {
  ReactFlow,
  Background,
  Controls,
  addEdge,
} from '@xyflow/react';
import type { Connection, NodeTypes, Node } from '@xyflow/react';
import '@xyflow/react/dist/style.css';

import { BtNode } from './BtNode';
import { useProgramStore } from '../store/programStore';
import { useComposerStore } from '../store/composerStore';

export function TreeCanvas() {
  const { nodes: storeNodes, edges: storeEdges, addNode, setNodes, setEdges, selectNode, selectedNodeId, removeNode } = useProgramStore();
  const { mode } = useComposerStore();
  const reactFlowWrapper = useRef<HTMLDivElement>(null);

  const nodeTypes: NodeTypes = useMemo(() => ({ btNode: BtNode }), []);

  const onConnect = useCallback(
    (connection: Connection) => {
      setEdges(addEdge(connection, storeEdges));
    },
    [storeEdges, setEdges],
  );

  // Handle node click for selection
  const onNodeClick = useCallback(
    (_: React.MouseEvent, node: Node) => {
      selectNode(node.id);
    },
    [selectNode],
  );

  // Handle pane click to deselect
  const onPaneClick = useCallback(() => {
    selectNode(null);
  }, [selectNode]);

  // Handle drop from palette
  const onDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  }, []);

  const onDrop = useCallback(
    (e: React.DragEvent) => {
      e.preventDefault();
      // Disable drop in run mode
      if (mode === 'run') return;
      
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
    [addNode, mode],
  );

  // Handle Delete key to remove selected node
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      // Disable delete in run mode
      if (mode === 'run') return;
      
      if (e.key === 'Delete' && selectedNodeId) {
        removeNode(selectedNodeId);
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [selectedNodeId, removeNode, mode]);

  // Determine if editing is enabled based on mode
  const isComposeMode = mode === 'compose';

  return (
    <div ref={reactFlowWrapper} className="flex-1">
      <ReactFlow
        nodes={storeNodes}
        edges={storeEdges}
        onNodesChange={(changes) => {
          // Apply position changes
          const updated = [...storeNodes];
          for (const change of changes) {
            if (change.type === 'position' && change.position) {
              const node = updated.find(n => n.id === change.id);
              if (node) node.position = change.position;
            }
          }
          setNodes(updated);
        }}
        onEdgesChange={() => {}}
        onConnect={isComposeMode ? onConnect : undefined}
        onNodeClick={onNodeClick}
        onPaneClick={onPaneClick}
        onDragOver={onDragOver}
        onDrop={onDrop}
        nodeTypes={nodeTypes}
        fitView
        nodesDraggable={isComposeMode}
        nodesConnectable={isComposeMode}
        proOptions={{ hideAttribution: true }}
        className="bg-gray-900"
      >
        <Background color="#374151" gap={20} />
        <Controls />
      </ReactFlow>
    </div>
  );
}
