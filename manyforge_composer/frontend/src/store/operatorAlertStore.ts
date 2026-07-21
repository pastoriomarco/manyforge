import { useEffect } from 'react';
import { create } from 'zustand';

import { api } from '../api/client';
import type { OperatorAlert, OperatorAlertInbox } from '../types/api';

interface OperatorAlertStore {
  alerts: OperatorAlert[];
  blockingCount: number;
  unacknowledgedAlertIds: string[];
  generation: number;
  timestampNs: number;
  truncated: boolean;
  loaded: boolean;
  busy: boolean;
  error: string | null;
  refresh: () => Promise<void>;
  acknowledge: (alertId: string) => Promise<void>;
  acknowledgeAll: () => Promise<void>;
}

function inboxPatch(inbox: OperatorAlertInbox) {
  return {
    alerts: inbox.alerts,
    blockingCount: inbox.blockingCount,
    unacknowledgedAlertIds: inbox.unacknowledgedAlertIds,
    generation: inbox.generation,
    timestampNs: inbox.timestampNs,
    truncated: inbox.truncated,
    loaded: true,
  };
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : 'Operator alerts unavailable';
}

export function operatorAttentionBlocksMotion(
  startsMotion: boolean,
  loaded: boolean,
  error: string | null,
  blockingCount: number,
): boolean {
  return startsMotion && (!loaded || error !== null || blockingCount > 0);
}

function mergeInbox(
  state: OperatorAlertStore,
  inbox: OperatorAlertInbox,
): Partial<OperatorAlertStore> {
  // Server monotonic time survives a Composer process restart, while the
  // process-lifetime generation intentionally resets. Compare the observation
  // timestamp first so an old in-flight response cannot overwrite newer state
  // and a freshly restarted backend can still replace the previous inbox.
  const stale = inbox.timestampNs < state.timestampNs
    || (
      inbox.timestampNs === state.timestampNs
      && inbox.generation < state.generation
    );
  return stale ? { loaded: true } : inboxPatch(inbox);
}

export const useOperatorAlertStore = create<OperatorAlertStore>((set, get) => ({
  alerts: [],
  blockingCount: 0,
  unacknowledgedAlertIds: [],
  generation: 0,
  timestampNs: 0,
  truncated: false,
  loaded: false,
  busy: false,
  error: null,
  refresh: async () => {
    try {
      const inbox = await api.execution.alerts(3);
      set((state) => ({ ...mergeInbox(state, inbox), error: null }));
    } catch (error) {
      set({ loaded: true, error: errorMessage(error) });
    }
  },
  acknowledge: async (alertId) => {
    set({ busy: true, error: null });
    try {
      const response = await api.execution.acknowledgeAlert(alertId);
      set((state) => ({
        ...mergeInbox(state, response.inbox),
        busy: false,
        error: null,
      }));
    } catch (error) {
      const message = errorMessage(error);
      set({ busy: false, error: message });
      await get().refresh();
      set({ error: message });
      throw error;
    }
  },
  acknowledgeAll: async () => {
    const { unacknowledgedAlertIds, generation } = get();
    set({ busy: true, error: null });
    try {
      const response = await api.execution.acknowledgeAllAlerts(
        unacknowledgedAlertIds,
        generation,
        true,
      );
      set((state) => ({
        ...mergeInbox(state, response.inbox),
        busy: false,
        error: null,
      }));
    } catch (error) {
      const message = errorMessage(error);
      set({ busy: false, error: message });
      await get().refresh();
      set({ error: message });
      throw error;
    }
  },
}));

const VISIBLE_POLL_MS = 1000;
const HIDDEN_POLL_MS = 30000;
let pollTimer: number | null = null;
let subscriberCount = 0;

function scheduleAlertPoll(): void {
  if (subscriberCount === 0) {
    pollTimer = null;
    return;
  }
  const delay = typeof document !== 'undefined' && document.hidden
    ? HIDDEN_POLL_MS
    : VISIBLE_POLL_MS;
  pollTimer = window.setTimeout(() => {
    void useOperatorAlertStore.getState().refresh();
    scheduleAlertPoll();
  }, delay);
}

function stopAlertPoll(): void {
  if (pollTimer !== null) {
    window.clearTimeout(pollTimer);
    pollTimer = null;
  }
}

if (typeof document !== 'undefined') {
  document.addEventListener('visibilitychange', () => {
    if (subscriberCount === 0) return;
    stopAlertPoll();
    if (!document.hidden) void useOperatorAlertStore.getState().refresh();
    scheduleAlertPoll();
  });
}

export function useOperatorAlertPolling(): void {
  useEffect(() => {
    subscriberCount += 1;
    if (subscriberCount === 1) {
      void useOperatorAlertStore.getState().refresh();
      scheduleAlertPoll();
    }
    return () => {
      subscriberCount -= 1;
      if (subscriberCount === 0) stopAlertPoll();
    };
  }, []);
}
