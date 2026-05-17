import { create } from 'zustand';
import { api } from '../api/client';
import { StateWebSocket } from '../api/websocket';
import { useProgramStore } from './programStore';
import { useViewerSelectionStore } from './viewerSelectionStore';
import type { CycleStatus } from '../types/api';

interface CycleStore {
  state: CycleStatus['state'];
  velocityScale: number;
  nodeStatuses: Record<string, string>;
  programLoaded: boolean;
  programSessionGeneration: number;
  error: string | null;

  // Actions
  run: () => Promise<void>;
  stop: () => Promise<void>;
  reset: () => Promise<void>;
  setVelocityScale: (value: number) => Promise<void>;

  // WebSocket
  _ws: StateWebSocket | null;
  connectWs: () => void;
  disconnectWs: () => void;
}

export const useCycleStore = create<CycleStore>((set, get) => ({
  state: 'idle',
  velocityScale: 1.0,
  nodeStatuses: {},
  programLoaded: false,
  programSessionGeneration: 0,
  error: null,
  _ws: null,

  run: async () => {
    try {
      const result = await api.cycle.run();
      set({ state: result.state as CycleStatus['state'], error: null });
    } catch (e) {
      set({ error: (e as Error).message });
    }
  },

  stop: async () => {
    try {
      const result = await api.cycle.stop();
      set({ state: result.state as CycleStatus['state'], error: null });
    } catch (e) {
      set({ error: (e as Error).message });
    }
  },

  reset: async () => {
    try {
      const result = await api.cycle.reset();
      set({ state: result.state as CycleStatus['state'], error: null });
      const programStore = useProgramStore.getState();
      if (get().programLoaded || programStore.programInfo !== null) {
        await Promise.all([
          programStore.fetchProgramInfo(),
          programStore.fetchTree(),
        ]);
      }
    } catch (e) {
      set({ error: (e as Error).message });
    }
  },

  setVelocityScale: async (value: number) => {
    try {
      const result = await api.cycle.setVelocityScale(value);
      set({ velocityScale: result.velocityScale, error: null });
    } catch (e) {
      set({ error: (e as Error).message });
    }
  },

  connectWs: () => {
    const ws = new StateWebSocket();
    ws.connect();
    ws.subscribe((update) => {
      const prev = get();
      const nextProgramSessionGeneration = update.programSessionGeneration ?? 0;
      set({
        state: update.state,
        velocityScale: update.velocityScale,
        nodeStatuses: update.nodeStatuses || {},
        programLoaded: update.programLoaded ?? false,
        programSessionGeneration: nextProgramSessionGeneration,
        error: update.error ?? null,
      });
      // Keep the canvas hydrated even if startup ordering or reconnect timing
      // causes the initial load edge to be missed.
      const programStore = useProgramStore.getState();
      useProgramStore.setState({
        runtimeOverrideStatus: update.runtimeOverrideStatus ?? null,
      });
      const needsHydration =
        programStore.programInfo === null ||
        programStore.nodes.length === 0 ||
        nextProgramSessionGeneration !== prev.programSessionGeneration;
      if (update.programLoaded && (!prev.programLoaded || needsHydration)) {
        void programStore.hydrateLoadedProgram({
          retries: prev.programLoaded ? 1 : 4,
          retryDelayMs: 500,
        });
      }
    });
    set({ _ws: ws });
  },

  disconnectWs: () => {
    get()._ws?.disconnect();
    set({ _ws: null });
  },
}));

/**
 * Whether interactive editing of the program/scene should be locked.
 * True when the cycle is in any active backend state
 * (running / interrupt_handler / direct_control) OR when the user
 * is actively dragging the TCP jog gizmo — even if the backend
 * hasn't yet transitioned to direct_control. Keeps the UI feel
 * consistent: any interaction that actively commands the robot
 * gates the rest of the surface the same way.
 */
export function useEditLocked(): boolean {
  const cycleState = useCycleStore((s) => s.state);
  const dragging = useViewerSelectionStore((s) => s.isDraggingGizmo);
  return (
    cycleState === 'running' ||
    cycleState === 'interrupt_handler' ||
    cycleState === 'direct_control' ||
    dragging
  );
}
