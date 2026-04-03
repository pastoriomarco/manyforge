import { create } from 'zustand';
import { api } from '../api/client';
import { StateWebSocket } from '../api/websocket';
import type { CycleStatus } from '../types/api';

interface CycleStore {
  state: CycleStatus['state'];
  velocityScale: number;
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
      set({ state: update.state, velocityScale: update.velocityScale });
    });
    set({ _ws: ws });
  },

  disconnectWs: () => {
    get()._ws?.disconnect();
    set({ _ws: null });
  },
}));
