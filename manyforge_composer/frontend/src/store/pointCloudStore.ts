import { useEffect } from 'react';
import { create } from 'zustand';

export interface PointCloudFrame {
  points: number[][];
  colors?: number[][];
  timestamp_ns: number;
}

interface PointCloudStore {
  frame: PointCloudFrame | null;
}

/**
 * Latest point-cloud / octomap frame from the backend
 * `/ws/scene/pointcloud` channel. The server pushes either
 * `pointcloud_frame` (with points + optional colors) or
 * `pointcloud_clear`; the latter sets `frame` to null.
 *
 * Frames are *replacements*, not deltas, in this first pass. A
 * future Day 7+ refinement may introduce a diff protocol once a
 * real perception source registers and bandwidth is measurable.
 */
export const usePointCloudStore = create<PointCloudStore>(() => ({
  frame: null,
}));

let ws: WebSocket | null = null;
let active = false;
let reconnectTimer: number | null = null;
let reconnectAttempts = 0;
let subscriberCount = 0;

function wsUrl(): string {
  if (typeof window === 'undefined') return '';
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const apiBase = import.meta.env.VITE_API_URL as string | undefined;
  if (apiBase) {
    return apiBase.replace(/^https?:/, proto) + '/ws/scene/pointcloud';
  }
  return `${proto}//${window.location.host}/ws/scene/pointcloud`;
}

function connect(): void {
  if (!active || typeof window === 'undefined') return;
  ws = new WebSocket(wsUrl());

  ws.onopen = () => {
    reconnectAttempts = 0;
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg && msg.type === 'pointcloud_frame' && Array.isArray(msg.points)) {
        usePointCloudStore.setState({
          frame: {
            points: msg.points,
            colors: Array.isArray(msg.colors) ? msg.colors : undefined,
            timestamp_ns: Number(msg.timestamp_ns) || 0,
          },
        });
      } else if (msg && msg.type === 'pointcloud_clear') {
        usePointCloudStore.setState({ frame: null });
      }
    } catch {
      /* malformed; ignore */
    }
  };

  ws.onclose = () => {
    ws = null;
    if (!active) return;
    // Exponential back-off (1.5 s → 30 s). Point-cloud streaming is
    // best-effort; a sustained outage shouldn't spam the console.
    reconnectAttempts += 1;
    const delay = Math.min(30000, 1500 * 2 ** (reconnectAttempts - 1));
    reconnectTimer = window.setTimeout(connect, delay);
  };

  ws.onerror = () => {
    try {
      ws?.close();
    } catch {
      /* already closed */
    }
  };
}

/**
 * Register the calling component as a point-cloud subscriber. First
 * subscriber opens the WS; last subscriber closes it. Components
 * read frames via `usePointCloudStore((s) => s.frame)`.
 */
export function usePointCloudSubscription(): void {
  useEffect(() => {
    subscriberCount += 1;
    if (subscriberCount === 1) {
      active = true;
      connect();
    }
    return () => {
      subscriberCount -= 1;
      if (subscriberCount === 0) {
        active = false;
        if (reconnectTimer !== null) {
          window.clearTimeout(reconnectTimer);
          reconnectTimer = null;
        }
        try {
          ws?.close();
        } catch {
          /* already closed */
        }
        ws = null;
      }
    };
  }, []);
}
