import { create } from 'zustand';
import { useCycleStore } from './cycleStore';
import { useProgramStore } from './programStore';

type NodeStatus = 'idle' | 'running' | 'success' | 'failure';

interface ComposerStore {
  mode: 'compose' | 'run';
  nodeStatuses: Record<string, NodeStatus>;
  
  // Actions
  setNodeStatuses: (statuses: Record<string, string>) => void;
  setNodeStatus: (nodeId: string, status: NodeStatus) => void;
  clearNodeStatuses: () => void;
}

export const useComposerStore = create<ComposerStore>((set) => ({
  mode: 'compose',
  nodeStatuses: {},
  
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
  
  clearNodeStatuses: () => {
    set({ nodeStatuses: {} });
  },
}));

// Subscribe to cycleStore changes to auto-update mode and nodeStatuses
export function initComposerStore() {
  const unsubscribe = useCycleStore.subscribe((state) => {
    const cycleState = state.state;
    const newMode: 'compose' | 'run' = cycleState === 'idle' ? 'compose' : 'run';
    
    // Update mode
    if (useComposerStore.getState().mode !== newMode) {
      useComposerStore.setState({ mode: newMode });
    }
    
    // Update node statuses based on cycle state
    if (cycleState === 'running') {
      // When running, set all nodes to 'running'
      const { nodes } = useProgramStore.getState();
      const statuses: Record<string, string> = {};
      for (const node of nodes) {
        statuses[node.id] = 'running';
      }
      useComposerStore.getState().setNodeStatuses(statuses);
    } else if (cycleState === 'idle') {
      // When idle, clear all statuses
      useComposerStore.getState().clearNodeStatuses();
    }
    // When stopped, keep current statuses (paused state)
  });
  
  return unsubscribe;
}
