import { useEffect, useRef } from 'react';
import { useStore } from '@react-three/fiber';
import { Vector2 } from 'three';

/**
 * Native R3F visualizer Day 3: round-trips scene-snapshot requests
 * from the backend snapshot broker. Mounted inside the same Canvas
 * the operator sees so we capture the live framebuffer rather than
 * a hidden offscreen renderer (which keeps the bytes consistent with
 * what the operator is looking at).
 *
 * Wire:
 *   backend SnapshotBroker -> WS /ws/scene/snapshot -> this component
 *   captureFrame(width, height, mime) -> POST /api/scene/snapshot/upload/{id}
 *
 * The Canvas's `gl` config must include `preserveDrawingBuffer: true`
 * — without it `toDataURL` may return a transparent frame depending
 * on the browser's compositor.
 */
export function SnapshotResponder() {
  // R3F's `useThree(selector)` returns the selected value, but every
  // change to the underlying R3F store fires the selector — when those
  // selected values end up in a `useEffect` dep list the effect
  // re-mounts on every frame and the WebSocket thrashes, which in
  // turn pumps the React reconciler into update-depth-exceeded (the
  // minified React error #185 the operator saw). Subscribe to the
  // store itself (stable reference) and pull live values via
  // `.getState()` inside the effect.
  const store = useStore();
  const wsRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    let active = true;
    let reconnectTimer: number | null = null;
    let reconnectAttempts = 0;

    const wsUrl = (() => {
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const apiBase = import.meta.env.VITE_API_URL as string | undefined;
      if (apiBase) {
        return apiBase.replace(/^https?:/, proto) + '/ws/scene/snapshot';
      }
      return `${proto}//${window.location.host}/ws/scene/snapshot`;
    })();

    async function captureFrame(
      width: number,
      height: number,
      mime: string,
    ): Promise<Blob> {
      const { gl, scene, camera, invalidate } = store.getState();
      const savedSize = new Vector2();
      gl.getSize(savedSize);
      const savedPixelRatio = gl.getPixelRatio();
      const persp = camera as unknown as {
        isPerspectiveCamera?: boolean;
        aspect?: number;
        updateProjectionMatrix?: () => void;
      };
      const savedAspect =
        persp.isPerspectiveCamera && typeof persp.aspect === 'number' ? persp.aspect : null;

      try {
        gl.setPixelRatio(1);
        gl.setSize(width, height, false);
        if (savedAspect !== null && persp.updateProjectionMatrix) {
          persp.aspect = width / height;
          persp.updateProjectionMatrix();
        }
        gl.render(scene, camera);
        const dataUrl = gl.domElement.toDataURL(mime, 0.9);
        const response = await fetch(dataUrl);
        return await response.blob();
      } finally {
        gl.setPixelRatio(savedPixelRatio);
        gl.setSize(savedSize.x, savedSize.y, false);
        if (savedAspect !== null && persp.updateProjectionMatrix) {
          persp.aspect = savedAspect;
          persp.updateProjectionMatrix();
        }
        // Force a repaint at the restored size so the visible canvas
        // doesn't get stuck showing the snapshot resolution.
        invalidate();
      }
    }

    async function handleRequest(msg: {
      requestId: string;
      width: number;
      height: number;
      format: string;
    }): Promise<void> {
      const mime = msg.format === 'png' ? 'image/png' : 'image/jpeg';
      try {
        const blob = await captureFrame(msg.width, msg.height, mime);
        await fetch(
          `/api/scene/snapshot/upload/${encodeURIComponent(msg.requestId)}`,
          {
            method: 'POST',
            body: blob,
            headers: { 'Content-Type': mime },
          },
        );
      } catch (err) {
        console.warn('snapshot capture/upload failed', err);
        // Send an empty body so the broker resolves to None promptly
        // instead of timing out the full 2 s window.
        try {
          await fetch(
            `/api/scene/snapshot/upload/${encodeURIComponent(msg.requestId)}`,
            {
              method: 'POST',
              body: new Blob([], { type: 'application/octet-stream' }),
            },
          );
        } catch {
          /* the broker times out on its own */
        }
      }
    }

    function connect(): void {
      if (!active) return;
      const ws = new WebSocket(wsUrl);
      wsRef.current = ws;

      ws.onopen = () => {
        // Successful connection resets the back-off ladder.
        reconnectAttempts = 0;
      };

      ws.onmessage = (event) => {
        try {
          const msg = JSON.parse(event.data);
          if (msg && msg.type === 'snapshot_request' && typeof msg.requestId === 'string') {
            void handleRequest(msg);
          }
        } catch {
          /* ignore malformed messages */
        }
      };

      ws.onclose = () => {
        wsRef.current = null;
        if (!active) return;
        // Exponential back-off (1.5 s, 3 s, 6 s, ... capped at 30 s)
        // avoids spamming the console with reconnect attempts when
        // the snapshot route is unavailable for an extended period.
        // The snapshot service is best-effort — if it's down the
        // viewer continues to work, only VLM nodes degrade.
        reconnectAttempts += 1;
        const delay = Math.min(30000, 1500 * 2 ** (reconnectAttempts - 1));
        reconnectTimer = window.setTimeout(connect, delay);
      };

      ws.onerror = () => {
        try {
          ws.close();
        } catch {
          /* already closed */
        }
      };
    }

    connect();

    return () => {
      active = false;
      if (reconnectTimer !== null) {
        window.clearTimeout(reconnectTimer);
      }
      try {
        wsRef.current?.close();
      } catch {
        /* already closed */
      }
    };
  }, [store]);

  return null;
}
