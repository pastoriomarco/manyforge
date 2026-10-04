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
  it('keeps view-only gestures local and aligns exactly once when layout direction changes', () => {
    const onMutate = vi.fn();
    const onSelect = vi.fn();
    renderToStaticMarkup(
      <ProgramTreeCanvas
        roots={[tree]}
        retainedPositions={{}}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="settle"
        invalidNodeIds={new Set()}
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
      zoomOnDoubleClick: boolean;
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
    expect(flow.zoomOnDoubleClick).toBe(false);
    expect(onSelect).toHaveBeenCalledWith('settle');
    expect(fitView).toHaveBeenCalled();
    expect(animationFrame).toHaveBeenCalled();
    expect(onMutate).toHaveBeenCalledTimes(2);
    expect(onMutate.mock.calls.map(([mutation]) => mutation)).toEqual([
      'tree_draft_update_positions',
      'tree_draft_update_positions',
    ]);
    expect(onMutate.mock.calls[0]?.[1]).toEqual({ positions: expect.any(Object) });
    expect(onMutate.mock.calls[1]?.[1]).toEqual({ positions: expect.any(Object) });
    expect(onMutate.mock.calls[1]?.[1]).not.toEqual(onMutate.mock.calls[0]?.[1]);
  });

  it('issues one owner mutation for drop, connect, disconnect, and drag-end gestures', () => {
    const detached: ProgramTreeNode = {
      node_id: 'timer',
      instance_id: 'detached',
      parameters: {},
      children: [],
    };
    const onMutate = vi.fn();
    const onSelect = vi.fn();
    renderToStaticMarkup(
      <ProgramTreeCanvas
        roots={[tree, detached]}
        retainedPositions={{}}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId={null}
        invalidNodeIds={new Set(['detached'])}
        busy={false}
        onSelect={onSelect}
        onMutate={onMutate}
      />,
    );
    const flow = captured.flow as {
      nodes: Array<{
        id: string;
        position: { x: number; y: number };
        data: { presentation: { invalid?: boolean } };
      }>;
      onInit: (instance: {
        fitView: (options?: unknown) => void;
        screenToFlowPosition: (point: { x: number; y: number }) => { x: number; y: number };
      }) => void;
      onDrop: (event: Record<string, unknown>) => void;
      onConnect: (connection: { source: string; target: string }) => void;
      onEdgesDelete: (edges: Array<{ source: string; target: string }>) => void;
      onEdgeDoubleClick: (
        event: { preventDefault: () => void; stopPropagation: () => void },
        edge: { source: string; target: string },
      ) => void;
      onNodeDragStop: (event: unknown, node: { id: string; position: { x: number; y: number } }) => void;
    };
    flow.onInit({
      fitView: vi.fn(),
      screenToFlowPosition: (point) => point,
    });
    flow.onDrop({
      preventDefault: vi.fn(),
      clientX: 30,
      clientY: 40,
      dataTransfer: {
        getData: (format: string) => format === 'application/manyforge-node' ? 'timer' : '',
      },
    });
    flow.onConnect({ source: 'root', target: 'detached' });
    flow.onEdgesDelete([{ source: 'root', target: 'settle' }]);
    flow.onNodeDragStop(null, { id: 'settle', position: { x: 50, y: 60 } });

    const settlePosition = flow.nodes.find((node) => node.id === 'settle')?.position;
    const allPositions = Object.fromEntries(
      flow.nodes.map((node) => [node.id, node.position]),
    );
    expect(settlePosition).toBeDefined();
    expect(flow.nodes.find((node) => node.id === 'detached')?.data.presentation.invalid).toBe(true);
    expect(onMutate.mock.calls).toEqual([
      ['tree_draft_insert_node', {
        index: 2,
        node: {
          node_id: 'timer',
          instance_id: 'timer',
          parameters: {},
          children: [],
        },
        // Centred on the pointer (30, 40), like the drag preview.
        position: { x: -70, y: 5 },
      }],
      ['tree_draft_move_node', {
        instance_id: 'detached',
        new_parent_instance_id: 'root',
        index: 1,
        positions: allPositions,
      }],
      ['tree_draft_move_node', {
        instance_id: 'settle',
        new_parent_instance_id: null,
        index: 2,
        positions: allPositions,
      }],
      ['tree_draft_update_positions', {
        positions: { settle: { x: 50, y: 60 } },
      }],
    ]);

    onMutate.mockClear();
    const edgeEvent = { preventDefault: vi.fn(), stopPropagation: vi.fn() };
    flow.onEdgeDoubleClick(edgeEvent, { source: 'root', target: 'settle' });
    expect(edgeEvent.preventDefault).toHaveBeenCalledOnce();
    expect(edgeEvent.stopPropagation).toHaveBeenCalledOnce();
    expect(onMutate).toHaveBeenCalledWith('tree_draft_move_node', {
      instance_id: 'settle',
      new_parent_instance_id: null,
      index: 2,
      positions: allPositions,
    });
  });

  it('turns a sibling drag into one explicit semantic reorder while retaining its layout', () => {
    const orderedTree: ProgramTreeNode = {
      ...tree,
      children: ['first', 'middle', 'last'].map((instanceId) => ({
        node_id: 'timer',
        instance_id: instanceId,
        parameters: {},
        children: [],
      })),
    };
    const retainedPositions = {
      root: { x: 40, y: 120 },
      first: { x: 340, y: 0 },
      middle: { x: 340, y: 120 },
      last: { x: 340, y: 240 },
    };
    const onMutate = vi.fn();
    renderToStaticMarkup(
      <ProgramTreeCanvas
        roots={[orderedTree]}
        retainedPositions={retainedPositions}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId={null}
        invalidNodeIds={new Set()}
        busy={false}
        onSelect={vi.fn()}
        onMutate={onMutate}
      />,
    );
    const flow = captured.flow as {
      onNodeDragStop: (
        event: unknown,
        node: { id: string; position: { x: number; y: number } },
      ) => void;
    };

    flow.onNodeDragStop(null, { id: 'last', position: { x: 340, y: 50 } });

    expect(onMutate).toHaveBeenCalledOnce();
    expect(onMutate).toHaveBeenCalledWith('tree_draft_move_node', {
      instance_id: 'last',
      new_parent_instance_id: 'root',
      index: 1,
      positions: {
        ...retainedPositions,
        last: { x: 340, y: 50 },
      },
    });
  });

  it('keeps topology attachment and deletion available for a detached root', () => {
    const detached: ProgramTreeNode = {
      node_id: 'timer',
      instance_id: 'detached',
      parameters: {},
      children: [],
    };
    const markup = renderToStaticMarkup(
      <ProgramTreeCanvas
        roots={[tree, detached]}
        retainedPositions={{}}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="detached"
        invalidNodeIds={new Set(['detached'])}
        busy={false}
        onSelect={vi.fn()}
        onMutate={vi.fn()}
      />,
    );

    expect(markup).toContain('aria-label="Topology actions for detached"');
    expect(markup).toContain('>Parent<select');
    expect(markup).toContain('<option value="root">root</option>');
    expect(markup).toMatch(/disabled="">Move earlier<\/button>/);
    expect(markup).toMatch(/disabled="">Move later<\/button>/);
    expect(markup).toContain('class="danger-button">Delete</button>');

    const noAlternativeParentMarkup = renderToStaticMarkup(
      <ProgramTreeCanvas
        roots={[tree]}
        retainedPositions={{}}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="settle"
        invalidNodeIds={new Set()}
        busy={false}
        onSelect={vi.fn()}
        onMutate={vi.fn()}
      />,
    );
    expect(noAlternativeParentMarkup).toContain('>Reparent<select disabled=""');
  });
});
