import { create } from 'zustand';
import { useCycleStore } from './cycleStore';
import { useProgramStore } from './programStore';
import { useViewerSelectionStore } from './viewerSelectionStore';

type NodeStatus = 'idle' | 'running' | 'success' | 'failure';
export type EdgeStyle = 'smoothstep' | 'default' | 'simplebezier' | 'straight' | 'step';

interface ComposerStore {
  mode: 'compose' | 'run';
  nodeStatuses: Record<string, NodeStatus>;
  edgeStyle: EdgeStyle;
  
  // Actions
  setNodeStatuses: (statuses: Record<string, string>) => void;
  setNodeStatus: (nodeId: string, status: NodeStatus) => void;
  setEdgeStyle: (style: EdgeStyle) => void;
  clearNodeStatuses: () => void;
}

export const useComposerStore = create<ComposerStore>((set) => ({
  mode: 'compose',
  nodeStatuses: {},
  edgeStyle: 'default',
  
  setNodeStatuses: (statuses) => {
    const normalized: Record<string, NodeStatus> = {};
    for (const [nodeId, status] of Object.entries(statuses)) {
      normalized[nodeId] = status as NodeStatus;
    }
    set({ nodeStatuses: normalized });
  },
  
  setNodeStatus: (nodeId, status) => {
    set((state) => ({
      nodeStatuses: { ...state.nodeStatuses, [nodeId]: status },
    }));
  },

  setEdgeStyle: (edgeStyle) => {
    set({ edgeStyle });
  },
  
  clearNodeStatuses: () => {
    set({ nodeStatuses: {} });
  },
}));

// Subscribe to cycleStore changes to auto-update mode and nodeStatuses.
// We also treat an active TCP-jog gizmo drag as "run" so the UI doesn't
// flicker back to compose between successive jog commands while the
// operator is still dragging (the backend ping-pongs idle↔direct_control
// in the gaps between motion segments).
function computeMode(): 'compose' | 'run' {
  const cycleState = useCycleStore.getState().state;
  const dragging = useViewerSelectionStore.getState().isDraggingGizmo;
  if (dragging) return 'run';
  return cycleState === 'idle' ? 'compose' : 'run';
}

function syncMode(): void {
  const newMode = computeMode();
  if (useComposerStore.getState().mode !== newMode) {
    useComposerStore.setState({ mode: newMode });
  }
}

export function initComposerStore() {
  const unsubscribeDrag = useViewerSelectionStore.subscribe(syncMode);
  const unsubscribe = useCycleStore.subscribe((state) => {
    const cycleState = state.state;
    syncMode();

    const wsStatuses = state.nodeStatuses || {};
    if (Object.keys(wsStatuses).length > 0) {
      // Build label→flowId lookup from current program nodes
      const { nodes } = useProgramStore.getState();
      const labelToId = new Map<string, string>();
      for (const node of nodes) {
        const label = (node.data as { label?: string }).label;
        if (label) labelToId.set(label, node.id);
      }

      // Map WS nodeStatuses (keyed by instance name) to flow node IDs
      const mapped: Record<string, string> = {};
      for (const [instanceName, status] of Object.entries(wsStatuses)) {
        const flowId = labelToId.get(instanceName);
        if (flowId) {
          mapped[flowId] = status.toLowerCase();
        }
      }
      useComposerStore.getState().setNodeStatuses(mapped);
    } else if (cycleState === 'idle') {
      useComposerStore.getState().clearNodeStatuses();
    }
  });

  return () => {
    unsubscribe();
    unsubscribeDrag();
  };
}
