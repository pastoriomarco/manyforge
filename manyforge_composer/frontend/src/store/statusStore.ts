import { useEffect } from 'react';
import { create } from 'zustand';

import { api } from '../api/client';
import type { InfraStatus, RuntimeStatus } from '../types/api';

interface StatusStore {
  runtime: RuntimeStatus | null;
  infra: InfraStatus | null;
  runtimeError: string | null;
  refresh: () => Promise<void>;
}

/**
 * Shared status store backed by a single poll loop. Replaces the
 * per-component setInterval timers that previously ran in
 * the viewer panel and RuntimeStatusOverlay. One subscriber-counted
 * tick fires per 2 seconds while the document is visible, dropping
 * to 30 seconds while hidden via the Page Visibility API.
 *
 * Why visibility-only (not window.focus / blur): when an operator
 * pulls focus to another window but Composer remains in
 * view, we want the status panels to keep updating smoothly. The
 * browser already throttles setInterval to ~1 Hz when the tab is
 * hidden; we go further (30 s) because runtime status changes are
 * not time-critical in that state.
 */
export const useStatusStore = create<StatusStore>((set) => ({
  runtime: null,
  infra: null,
  runtimeError: null,
  refresh: async () => {
    let runtime: RuntimeStatus | null = null;
    let runtimeError: string | null = null;
    let infra: InfraStatus | null = null;
    const runtimePromise = api.runtime
      .status()
      .then((value) => {
        runtime = value;
      })
      .catch((err: unknown) => {
        runtimeError = err instanceof Error ? err.message : 'Runtime status unavailable';
      });
    // infra status is best-effort; preserve the previous payload on failure
    // so transient disconnects don't blink the HMI panel into "unavailable".
    const infraPromise = api.infra
      .status()
      .then((value) => {
        infra = value;
      })
      .catch(() => {
        infra = null;
      });
    await Promise.allSettled([runtimePromise, infraPromise]);
    set((state) => ({
      runtime: runtime ?? state.runtime,
      runtimeError,
      infra: infra ?? state.infra,
    }));
  },
}));

const POLL_INTERVAL_MS_VISIBLE = 2000;
const POLL_INTERVAL_MS_HIDDEN = 30000;

let pollTimer: number | null = null;
let subscriberCount = 0;
let documentVisible: boolean =
  typeof document === 'undefined' ? true : !document.hidden;

function nextInterval(): number {
  return documentVisible ? POLL_INTERVAL_MS_VISIBLE : POLL_INTERVAL_MS_HIDDEN;
}

function scheduleNextTick(): void {
  if (subscriberCount === 0) {
    pollTimer = null;
    return;
  }
  pollTimer = window.setTimeout(() => {
    void useStatusStore.getState().refresh();
    scheduleNextTick();
  }, nextInterval());
}

function startPolling(): void {
  if (pollTimer !== null) {
    return;
  }
  // Fire immediately so the first subscriber doesn't see a blank panel
  // for up to 2 seconds.
  void useStatusStore.getState().refresh();
  scheduleNextTick();
}

function stopPolling(): void {
  if (pollTimer !== null) {
    window.clearTimeout(pollTimer);
    pollTimer = null;
  }
}

if (typeof document !== 'undefined') {
  document.addEventListener('visibilitychange', () => {
    const wasVisible = documentVisible;
    documentVisible = !document.hidden;
    if (documentVisible === wasVisible || subscriberCount === 0) {
      return;
    }
    // Reset the timer so the new cadence takes effect immediately
    // rather than waiting for the previously-scheduled tick to fire.
    stopPolling();
    if (documentVisible) {
      // Refresh on becoming-visible so the operator sees a current
      // snapshot the moment they return.
      void useStatusStore.getState().refresh();
    }
    scheduleNextTick();
  });
}

/**
 * Register the calling component as a subscriber to the shared status
 * poll. The first subscriber starts the poll loop; the last subscriber
 * stops it. Components still read state via individual selectors
 * (`useStatusStore((s) => s.runtime)`) for granular re-renders.
 */
export function useStatusPolling(): void {
  useEffect(() => {
    subscriberCount += 1;
    if (subscriberCount === 1) {
      startPolling();
    }
    return () => {
      subscriberCount -= 1;
      if (subscriberCount === 0) {
        stopPolling();
      }
    };
  }, []);
}
