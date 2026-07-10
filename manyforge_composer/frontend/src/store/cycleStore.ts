import { create } from 'zustand';
import { api, ApiError } from '../api/client';
import { StateWebSocket } from '../api/websocket';
import { useProgramStore } from './programStore';
import { useViewerSelectionStore } from './viewerSelectionStore';
import { coerceCycleStateFailClosed } from './cycleState';
import type { CycleCommand, CycleStatus } from '../types/api';

/**
 * Extract a structured error tuple from a thrown value. `ApiError` carries
 * the kernel ErrorDTO fields (errorCode + errorDetails) parsed from a 409
 * `detail.{message, errorCode, errorDetails}` response. Other Error types
 * carry only a message; the structured fields fall back to null. This is
 * needed because the cycle store's run/stop/reset/setVelocityScale catch
 * blocks would otherwise discard the structured cause and force the viewer
 * KernelErrorPanel to render its generic-banner fallback.
 */
function structuredErrorFromException(e: unknown): {
  message: string;
  errorCode: string | null;
  errorDetails: Record<string, unknown> | null;
} {
  if (e instanceof ApiError) {
    return {
      message: e.message,
      errorCode: e.errorCode,
      errorDetails: e.errorDetails,
    };
  }
  if (e instanceof Error) {
    return { message: e.message, errorCode: null, errorDetails: null };
  }
  return { message: String(e), errorCode: null, errorDetails: null };
}

type CycleStatePatch = Pick<
  CycleStore,
  'state' | 'motionStopState' | 'velocityScale' | 'error' | 'errorCode' | 'errorDetails'
>;

function cycleStatePatchFromCommand(
  result: CycleCommand,
): Pick<CycleStore, 'state' | 'motionStopState' | 'error' | 'errorCode' | 'errorDetails'> {
  const parsed = coerceCycleStateFailClosed(result.state);
  return {
    state: parsed.state,
    motionStopState: result.motionStopState ?? 'idle',
    error: parsed.error ?? result.error ?? null,
    errorCode: result.errorCode ?? null,
    errorDetails: result.errorDetails ?? null,
  };
}

function cycleStatePatchFromStatus(status: CycleStatus): CycleStatePatch {
  const parsed = coerceCycleStateFailClosed(status.state);
  return {
    state: parsed.state,
    motionStopState: status.motionStopState ?? 'idle',
    velocityScale: status.velocityScale,
    error: parsed.error ?? status.error ?? null,
    errorCode: status.errorCode ?? null,
    errorDetails: status.errorDetails ?? null,
  };
}

let statusFallbackTimer: number | null = null;

function stopStatusFallbackPoll(): void {
  if (statusFallbackTimer !== null) {
    window.clearTimeout(statusFallbackTimer);
    statusFallbackTimer = null;
  }
}

function startStatusFallbackPoll(
  set: (patch: Partial<CycleStore>) => void,
  get: () => CycleStore,
): void {
  if (statusFallbackTimer !== null) {
    return;
  }

  const tick = async () => {
    statusFallbackTimer = null;
    const current = get();
    const shouldPoll =
      current.state === 'running' ||
      current.state === 'stopping' ||
      current.state === 'direct_control' ||
      current._ws?.stale === true;

    if (shouldPoll) {
      try {
        const status = await api.cycle.status();
        set(cycleStatePatchFromStatus(status));
      } catch {
        // The WebSocket remains the primary state channel. A fallback poll
        // failure should not replace the last known operator-visible state.
      }
    }

    statusFallbackTimer = window.setTimeout(tick, shouldPoll ? 500 : 2000);
  };

  statusFallbackTimer = window.setTimeout(tick, 500);
}

interface CycleStore {
  state: CycleStatus['state'];
  motionStopState: NonNullable<CycleStatus['motionStopState']>;
  velocityScale: number;
  nodeStatuses: Record<string, string>;
  programLoaded: boolean;
  programSessionGeneration: number;
  error: string | null;
  // Structured cause from the cycle manager / kernel ErrorDTO so the viewer
  // can render the KernelErrorPanel without a separate /api/cycle/status
  // poll. Updated from the same WebSocket push as `error`.
  errorCode: string | null;
  errorDetails: Record<string, unknown> | null;

  // Actions
  run: () => Promise<void>;
  stop: () => Promise<void>;
  fullStop: () => Promise<void>;
  reset: () => Promise<void>;
  acknowledgeInterventionForRetry: () => Promise<void>;
  acknowledgeFailedValidationForRetry: () => Promise<void>;
  resetAndRun: () => Promise<void>;
  setVelocityScale: (value: number) => Promise<void>;
  refreshStatus: () => Promise<void>;

  // WebSocket
  _ws: StateWebSocket | null;
  connectWs: () => void;
  disconnectWs: () => void;
}

export const useCycleStore = create<CycleStore>((set, get) => ({
  state: 'idle',
  motionStopState: 'idle',
  velocityScale: 1.0,
  nodeStatuses: {},
  programLoaded: false,
  programSessionGeneration: 0,
  error: null,
  errorCode: null,
  errorDetails: null,
  _ws: null,

  run: async () => {
    try {
      const result = await api.cycle.run();
      set(cycleStatePatchFromCommand(result));
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
    }
  },

  stop: async () => {
    try {
      const result = await api.cycle.stop();
      set(cycleStatePatchFromCommand(result));
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
    }
  },

  fullStop: async () => {
    try {
      const result = await api.cycle.fullStop();
      set(cycleStatePatchFromCommand(result));
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
    }
  },

  reset: async () => {
    try {
      const result = await api.cycle.reset();
      set(cycleStatePatchFromCommand(result));
      const programStore = useProgramStore.getState();
      if (get().programLoaded || programStore.programInfo !== null) {
        await Promise.all([
          programStore.fetchProgramInfo(),
          programStore.fetchTree(),
        ]);
      }
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
    }
  },

  acknowledgeInterventionForRetry: async () => {
    try {
      const result = await api.cycle.acknowledgeInterventionForRetry();
      set(cycleStatePatchFromCommand(result));
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
      throw e;
    }
  },

  acknowledgeFailedValidationForRetry: async () => {
    try {
      const result = await api.cycle.acknowledgeFailedValidationForRetry();
      set(cycleStatePatchFromCommand(result));
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
      throw e;
    }
  },

  resetAndRun: async () => {
    try {
      const resetResult = await api.cycle.reset();
      set(cycleStatePatchFromCommand(resetResult));
      const programStore = useProgramStore.getState();
      if (get().programLoaded || programStore.programInfo !== null) {
        await Promise.all([
          programStore.fetchProgramInfo(),
          programStore.fetchTree(),
        ]);
      }
      const runResult = await api.cycle.run();
      set(cycleStatePatchFromCommand(runResult));
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
      throw e;
    }
  },

  setVelocityScale: async (value: number) => {
    try {
      const result = await api.cycle.setVelocityScale(value);
      set({
        velocityScale: result.velocityScale,
        error: null,
        errorCode: null,
        errorDetails: null,
      });
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
    }
  },

  refreshStatus: async () => {
    try {
      const result = await api.cycle.status();
      set(cycleStatePatchFromStatus(result));
    } catch (e) {
      const s = structuredErrorFromException(e);
      set({ error: s.message, errorCode: s.errorCode, errorDetails: s.errorDetails });
    }
  },

  connectWs: () => {
    const ws = new StateWebSocket();
    ws.connect();
    ws.subscribe((update) => {
      const prev = get();
      const nextProgramSessionGeneration = update.programSessionGeneration ?? 0;
      const parsed = coerceCycleStateFailClosed(update.state);
      set({
        state: parsed.state,
        motionStopState: update.motionStopState ?? 'idle',
        velocityScale: update.velocityScale,
        nodeStatuses: update.nodeStatuses || {},
        programLoaded: update.programLoaded ?? false,
        programSessionGeneration: nextProgramSessionGeneration,
        error: parsed.error ?? update.error ?? null,
        errorCode: update.errorCode ?? null,
        errorDetails: update.errorDetails ?? null,
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
    void get().refreshStatus();
    startStatusFallbackPoll(set, get);
  },

  disconnectWs: () => {
    get()._ws?.disconnect();
    set({ _ws: null });
    stopStatusFallbackPoll();
  },
}));

/**
 * Whether interactive editing of the program/scene should be locked.
 * True when the cycle is in any active backend state
 * (running / stopping / direct_control) OR when the user
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
    cycleState === 'stopping' ||
    cycleState === 'direct_control' ||
    dragging
  );
}
