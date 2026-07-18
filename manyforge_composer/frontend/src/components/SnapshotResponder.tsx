import { useEffect, useRef } from 'react';
import { useStore } from '@react-three/fiber';
import {
  PerspectiveCamera,
  RGBAFormat,
  SRGBColorSpace,
  UnsignedByteType,
  WebGLRenderTarget,
} from 'three';

import {
  applyLatestViewerFrame,
  waitForViewerFrameGeneration,
} from '../store/viewerFrameStore';
import { canvasToBlobWithTimeout, flipRgbaRowsInPlace } from '../utils/snapshotCapture';

/**
 * Native R3F visualizer Day 3: round-trips scene-snapshot requests
 * from the backend snapshot broker. Mounted inside the operator's Canvas so
 * the offscreen render uses the same scene and camera state without resizing
 * or preserving the visible framebuffer.
 *
 * Wire:
 *   backend SnapshotBroker -> WS /ws/scene/snapshot -> this component
 *   captureFrame(width, height, mime) -> POST /api/scene/snapshot/upload/{id}
 *
 * Capture uses a reusable offscreen render target. The visible framebuffer is
 * never preserved or resized, so snapshot support has no permanent render cost
 * and cannot perturb the operator camera/canvas.
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
  const targetRef = useRef<WebGLRenderTarget | null>(null);
  const pixelsRef = useRef<{
    bytes: Uint8Array<ArrayBuffer>;
    clamped: Uint8ClampedArray<ArrayBuffer>;
  } | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

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
      if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1) {
        throw new Error('Snapshot dimensions must be positive integers');
      }
      const { gl, scene, camera } = store.getState();
      let target = targetRef.current;
      if (target === null) {
        target = new WebGLRenderTarget(width, height, {
          format: RGBAFormat,
          type: UnsignedByteType,
          depthBuffer: true,
          stencilBuffer: false,
        });
        target.texture.colorSpace = SRGBColorSpace;
        targetRef.current = target;
      } else if (target.width !== width || target.height !== height) {
        target.setSize(width, height);
      }
      const requiredBytes = width * height * 4;
      let pixelViews = pixelsRef.current;
      if (pixelViews === null || pixelViews.bytes.byteLength !== requiredBytes) {
        const buffer = new ArrayBuffer(requiredBytes);
        pixelViews = {
          bytes: new Uint8Array(buffer),
          clamped: new Uint8ClampedArray(buffer),
        };
        pixelsRef.current = pixelViews;
      }
      let outputCanvas = canvasRef.current;
      if (outputCanvas === null) {
        outputCanvas = document.createElement('canvas');
        canvasRef.current = outputCanvas;
      }
      outputCanvas.width = width;
      outputCanvas.height = height;
      const context = outputCanvas.getContext('2d');
      if (context === null) throw new Error('2D snapshot encoder is unavailable');

      const captureCamera = camera.clone();
      if (captureCamera instanceof PerspectiveCamera) {
        captureCamera.aspect = width / height;
        captureCamera.updateProjectionMatrix();
      }
      const previousTarget = gl.getRenderTarget();

      try {
        gl.setRenderTarget(target);
        gl.clear(true, true, true);
        gl.render(scene, captureCamera);
        gl.readRenderTargetPixels(target, 0, 0, width, height, pixelViews.bytes);
      } finally {
        gl.setRenderTarget(previousTarget);
      }
      flipRgbaRowsInPlace(pixelViews.bytes, width, height);
      context.putImageData(new ImageData(pixelViews.clamped, width, height), 0, 0);
      return canvasToBlobWithTimeout(outputCanvas, mime, 0.9, 1_000);
    }

    async function handleRequest(msg: {
      requestId: string;
      width: number;
      height: number;
      format: string;
      requiredProducerEpoch?: string;
      requiredFrameSequence?: number;
      timeoutMs?: number;
    }): Promise<void> {
      const mime = msg.format === 'png' ? 'image/png' : 'image/jpeg';
      try {
        if (
          typeof msg.requiredProducerEpoch === 'string'
          && Number.isInteger(msg.requiredFrameSequence)
        ) {
          const available = await waitForViewerFrameGeneration(
            msg.requiredProducerEpoch,
            msg.requiredFrameSequence as number,
            Number.isFinite(msg.timeoutMs) ? Number(msg.timeoutMs) : 1_000,
          );
          if (!available) {
            throw new Error('Required viewer frame did not arrive before capture');
          }
          // Apply refs synchronously so hidden-tab animation throttling cannot
          // make an offscreen snapshot lag the generation the backend prepared.
          applyLatestViewerFrame(Date.now(), 0);
        }
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
      targetRef.current?.dispose();
      targetRef.current = null;
      pixelsRef.current = null;
      canvasRef.current = null;
    };
  }, [store]);

  return null;
}
