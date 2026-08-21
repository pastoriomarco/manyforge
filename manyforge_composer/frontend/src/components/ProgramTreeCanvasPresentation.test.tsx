import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type {
  CatalogEntry,
  NodeEnvironmentResponse,
  ProgramTreeNode,
} from '../types/api';

const captured = vi.hoisted(() => ({
  flow: null as Record<string, unknown> | null,
  controls: null as Record<string, unknown> | null,
}));

vi.mock('@xyflow/react', () => ({
  Background: () => null,
  BaseEdge: () => null,
  Controls: () => null,
  Handle: () => null,
  Position: { Top: 'top', Bottom: 'bottom', Left: 'left', Right: 'right' },
  ReactFlow: (props: Record<string, unknown>) => {
    captured.flow = props;
    return null;
  },
}));

vi.mock('./CanvasViewControls', () => ({
  CanvasViewControls: (props: Record<string, unknown>) => {
    captured.controls = props;
    return null;
  },
}));

import { ProgramTreeCanvas } from './ProgramTreeCanvas';

const sequence: CatalogEntry = {
  id: 'sequence',
  name: 'Sequence',
  category: 'control_flow',
  description: '',
  stability: 'stable',
  kind: 'composite',
  childrenMin: 1,
  childrenMax: -1,
  parameters: [],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'program' },
  runners: { py_trees: 'sequence_v1' },
  tags: [],
  minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const timer: CatalogEntry = {
  ...sequence,
  id: 'timer',
  name: 'Timer',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
};

const tree: ProgramTreeNode = {
  node_id: 'sequence',
  instance_id: 'root',
  parameters: {},
  children: [{
    node_id: 'timer',
    instance_id: 'settle',
    parameters: {},
    children: [],
  }],
};

const environment: NodeEnvironmentResponse = {
  environmentDigest: 'sha256:environment',
  implementations: [sequence, timer].map((entry) => ({
    nodeId: entry.id,
    engineId: 'py_trees',
    implementationId: `${entry.id}_v1`,
    executablePackageId: 'manyforge_behavior',
    authoringAvailable: true,
    executionAvailable: false,
    reasonCodes: ['native_program_product_unavailable'],
    restorationGates: ['N7'],
  })),
  portableAuthoringAvailable: true,
  executionAvailable: false,
  reasonCodes: ['native_program_product_unavailable'],
  restorationGates: ['N7'],
  active: null,
};

describe('Program tree canvas presentation actions', () => {
  it('keeps pan, zoom, fit, layout, edge style, and selection off the Draft mutation path', () => {
    const onMutate = vi.fn();
    const onSelect = vi.fn();
    renderToStaticMarkup(
      <ProgramTreeCanvas
        root={tree}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="settle"
        busy={false}
        onSelect={onSelect}
        onMutate={onMutate}
      />,
    );

    const flow = captured.flow as {
      nodes: Array<{
        id: string;
        data: { onSelect: (instanceId: string) => void };
      }>;
      panOnDrag: boolean;
      zoomOnScroll: boolean;
      zoomOnPinch: boolean;
      onInit: (instance: { fitView: (options?: unknown) => void }) => void;
      onNodesChange: (changes: unknown[]) => void;
    };
    const controls = captured.controls as {
      onAutoLayout: () => void;
      onFitView: () => void;
      onEdgeStyleChange: (value: 'straight') => void;
      onLayoutDirectionChange: (value: 'horizontal') => void;
    };
    const fitView = vi.fn();
    const animationFrame = vi.fn((callback: () => void) => {
      callback();
      return 1;
    });
    vi.stubGlobal('requestAnimationFrame', animationFrame);
    try {
      flow.onInit({ fitView });
      flow.onNodesChange([{
        type: 'position',
        id: 'settle',
        position: { x: 20, y: 30 },
      }]);
      flow.onNodesChange([{ type: 'select', id: 'settle', selected: true }]);
      flow.nodes[1]?.data.onSelect('settle');
      controls.onFitView();
      controls.onAutoLayout();
      controls.onEdgeStyleChange('straight');
      controls.onLayoutDirectionChange('horizontal');
    } finally {
      vi.unstubAllGlobals();
    }

    expect(flow.nodes.map((node) => node.id)).toEqual(['root', 'settle']);
    expect(flow.panOnDrag).toBe(true);
    expect(flow.zoomOnScroll).toBe(true);
    expect(flow.zoomOnPinch).toBe(true);
    expect(onSelect).toHaveBeenCalledWith('settle');
    expect(fitView).toHaveBeenCalled();
    expect(animationFrame).toHaveBeenCalled();
    expect(onMutate).not.toHaveBeenCalled();
  });
});
