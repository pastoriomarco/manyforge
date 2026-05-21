import { useEffect } from 'react';
import { create } from 'zustand';

import { api } from '../api/client';
import { useCycleStore } from './cycleStore';
import type { SceneFramesResponse, SceneStateResponse } from '../types/api';

interface SceneStateStore {
  scene: SceneStateResponse | null;
  frames: SceneFramesResponse | null;
  error: string | null;
  lastUpdatedAtMs: number | null;
  refreshInFlight: boolean;
  refresh: () => Promise<void>;
}

/**
 * Shared scene-state store backed by a single poll loop. Mirrors the
 * subscriber-counting pattern in `statusStore` so the first component
 * that subscribes via `useScenePolling()` starts the loop and the last
 * subscriber stops it.
 *
 * Cadence is 500 ms while the document is visible, 5 s while hidden via
 * the Page Visibility API — faster than the status poll (2 s) because
 * the 3D viewer wants smoother updates during execution, and slower
 * than a per-tick stream because we don't want a 60 Hz REST pump.
 * Day-4 work can replace this with a WS push if needed.
 */
export const useSceneStateStore = create<SceneStateStore>((set) => ({
  scene: null,
  frames: null,
  error: null,
  lastUpdatedAtMs: null,
  refreshInFlight: false,
  refresh: async () => {
    if (useSceneStateStore.getState().refreshInFlight) {
      return;
    }
    set({ refreshInFlight: true });
    // Fetch /api/scene/state + /api/scene/frames in parallel. Both
    // are cheap reads from in-memory state and the viewer needs them
    // at the same cadence (frame axes + visibility checklists track
    // the live kernel poses). If one fails we keep the previous
    // value and surface the most recent error.
    const sceneP = api.scene.state();
    const framesP = api.scene.frames();
    let scene: SceneStateResponse | null = null;
    let frames: SceneFramesResponse | null = null;
    let error: string | null = null;
    try {
      try {
        scene = await sceneP;
      } catch (err: unknown) {
        error = err instanceof Error ? err.message : 'Scene state unavailable';
      }
      try {
        frames = await framesP;
      } catch (err: unknown) {
        if (error === null) {
          error = err instanceof Error ? err.message : 'Frames inventory unavailable';
        }
      }
      const refreshed = scene !== null || frames !== null;
      set((state) => ({
        scene: scene ?? state.scene,
        frames: frames ?? state.frames,
        error,
        lastUpdatedAtMs: refreshed ? Date.now() : state.lastUpdatedAtMs,
      }));
    } finally {
      set({ refreshInFlight: false });
    }
  },
}));

// Adaptive poll cadence (operator feedback: robot motion looked
// ~5 fps during a `cycle/run` execution because the previous
// 500 ms interval = 2 Hz only sampled the kernel mid-trajectory a
// handful of times). When the cycle is in any active state we
// poll at 10 Hz; when idle we stay at the 500 ms editor-friendly
// rate; when the document is hidden we drop to 5 s to save battery.
// 10 Hz is the empirical knee — high enough to read as smooth
// motion on Thor, low enough that the 36-callback / 60 fps Viser
// failure mode the migration was triggered to fix doesn't return.
const POLL_INTERVAL_MS_ACTIVE = 100;
const POLL_INTERVAL_MS_IDLE = 250;
const POLL_INTERVAL_MS_HIDDEN = 5000;

let pollTimer: number | null = null;
let subscriberCount = 0;
let documentVisible: boolean =
  typeof document === 'undefined' ? true : !document.hidden;

function nextInterval(): number {
  if (!documentVisible) return POLL_INTERVAL_MS_HIDDEN;
  const cycleState = useCycleStore.getState().state;
  if (
    cycleState === 'running'
    || cycleState === 'direct_control'
    || cycleState === 'interrupt_handler'
  ) {
    return POLL_INTERVAL_MS_ACTIVE;
  }
  return POLL_INTERVAL_MS_IDLE;
}

function scheduleNextTick(): void {
  if (subscriberCount === 0) {
    pollTimer = null;
    return;
  }
  pollTimer = window.setTimeout(async () => {
    await useSceneStateStore.getState().refresh();
    scheduleNextTick();
  }, nextInterval());
}

function startPolling(): void {
  if (pollTimer !== null) {
    return;
  }
  void useSceneStateStore.getState().refresh();
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
    stopPolling();
    if (documentVisible) {
      void useSceneStateStore.getState().refresh();
    }
    scheduleNextTick();
  });
}

/**
 * Register the calling component as a subscriber to the shared scene
 * poll. Components still read state via individual selectors
 * (`useSceneStateStore((s) => s.scene)`) for granular re-renders.
 */
export function useScenePolling(): void {
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
