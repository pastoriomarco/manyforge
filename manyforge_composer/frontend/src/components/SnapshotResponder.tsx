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
  canvasToBlobWithTimeout,
  flipRgbaRowsInPlace,
} from '../utils/snapshotCapture';

export interface SnapshotRequestView {
  requestId: string;
  width: number;
  height: number;
  mime: 'image/png' | 'image/jpeg';
  requiredFrame: { ownerEpoch: string; generation: number };
  timeoutMs: number;
}

export function validateSnapshotRequest(request: SnapshotRequestView): string | null {
  if (!request.requestId.trim()) return 'Snapshot request ID is required';
  if (
    !Number.isInteger(request.width)
    || !Number.isInteger(request.height)
    || request.width < 1
    || request.height < 1
    || request.width > 1920
    || request.height > 1080
  ) {
    return 'Snapshot dimensions are outside the qualified bounds';
  }
  if (!['image/png', 'image/jpeg'].includes(request.mime)) {
    return 'Snapshot media type is unsupported';
  }
  if (
    !Number.isInteger(request.timeoutMs)
    || request.timeoutMs < 1
    || request.timeoutMs > 2_000
  ) return 'Snapshot timeout is outside the qualified bounds';
  if (
    request.requiredFrame === undefined
    || (
      !request.requiredFrame.ownerEpoch.trim()
      || !Number.isSafeInteger(request.requiredFrame.generation)
      || request.requiredFrame.generation < 0
    )
  ) return 'Snapshot required frame identity is invalid';
  return null;
}

interface SnapshotResponderProps {
  subscribe: (handler: (request: SnapshotRequestView) => void) => () => void;
  upload: (
    requestId: string,
    result: Blob | null,
    observedFrame: SnapshotRequestView['requiredFrame'] | null,
  ) => Promise<void>;
  ensureFrame: (
    frame: NonNullable<SnapshotRequestView['requiredFrame']>,
    timeoutMs: number,
  ) => Promise<SnapshotRequestView['requiredFrame'] | null>;
  applyFrame?: () => void;
}

/**
 * Reusable offscreen R3F capture client. I5/I9 own the exact snapshot request,
 * freshness, and upload adapters; no deleted WebSocket or route is embedded.
 */
export function SnapshotResponder({
  subscribe,
  upload,
  ensureFrame,
  applyFrame,
}: SnapshotResponderProps) {
  const store = useStore();
  const uploadRef = useRef(upload);
  const ensureFrameRef = useRef(ensureFrame);
  const applyFrameRef = useRef(applyFrame);
  const targetRef = useRef<WebGLRenderTarget | null>(null);
  const pixelsRef = useRef<{
    bytes: Uint8Array<ArrayBuffer>;
    clamped: Uint8ClampedArray<ArrayBuffer>;
  } | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  // Snapshot capture callbacks follow the newest presented owner frame, but
  // changing that frame must not tear down the one observer transport. Keep
  // the subscription lifecycle tied only to its actual transport function.
  uploadRef.current = upload;
  ensureFrameRef.current = ensureFrame;
  applyFrameRef.current = applyFrame;

  useEffect(() => {
    let active = true;

    const capture = async (
      request: SnapshotRequestView,
      deadlineMs: number,
    ): Promise<Blob> => {
      const validation = validateSnapshotRequest(request);
      if (validation) throw new Error(validation);
      const { gl, scene, camera } = store.getState();
      let target = targetRef.current;
      if (target === null) {
        target = new WebGLRenderTarget(request.width, request.height, {
          format: RGBAFormat,
          type: UnsignedByteType,
          depthBuffer: true,
          stencilBuffer: false,
        });
        target.texture.colorSpace = SRGBColorSpace;
        targetRef.current = target;
      } else if (target.width !== request.width || target.height !== request.height) {
        target.setSize(request.width, request.height);
      }

      const requiredBytes = request.width * request.height * 4;
      let pixelViews = pixelsRef.current;
      if (pixelViews === null || pixelViews.bytes.byteLength !== requiredBytes) {
        const buffer = new ArrayBuffer(requiredBytes);
        pixelViews = {
          bytes: new Uint8Array(buffer),
          clamped: new Uint8ClampedArray(buffer),
        };
        pixelsRef.current = pixelViews;
      }
      let canvas = canvasRef.current;
      if (canvas === null) {
        canvas = document.createElement('canvas');
        canvasRef.current = canvas;
      }
      canvas.width = request.width;
      canvas.height = request.height;
      const context = canvas.getContext('2d');
      if (context === null) throw new Error('2D snapshot encoder is unavailable');

      const captureCamera = camera.clone();
      if (captureCamera instanceof PerspectiveCamera) {
        captureCamera.aspect = request.width / request.height;
        captureCamera.updateProjectionMatrix();
      }
      const previousTarget = gl.getRenderTarget();
      try {
        gl.setRenderTarget(target);
        gl.clear(true, true, true);
        gl.render(scene, captureCamera);
        gl.readRenderTargetPixels(
          target,
          0,
          0,
          request.width,
          request.height,
          pixelViews.bytes,
        );
      } finally {
        gl.setRenderTarget(previousTarget);
      }
      flipRgbaRowsInPlace(pixelViews.bytes, request.width, request.height);
      context.putImageData(
        new ImageData(pixelViews.clamped, request.width, request.height),
        0,
        0,
      );
      const remainingMs = Math.max(1, Math.floor(deadlineMs - performance.now()));
      return canvasToBlobWithTimeout(
        canvas,
        request.mime,
        0.9,
        Math.min(1_000, remainingMs),
      );
    };

    const handle = async (request: SnapshotRequestView) => {
      try {
        const deadlineMs = performance.now() + request.timeoutMs;
        const observedFrame = await ensureFrameRef.current(
          request.requiredFrame,
          request.timeoutMs,
        );
        if (observedFrame === null) {
          throw new Error('Required viewer frame is unavailable');
        }
        applyFrameRef.current?.();
        const blob = await capture(request, deadlineMs);
        if (active) {
          await uploadRef.current(request.requestId, blob, observedFrame);
        }
      } catch (error) {
        console.warn('snapshot capture failed', error);
        if (active) {
          try {
            await uploadRef.current(request.requestId, null, null);
          } catch (uploadError) {
            console.warn('snapshot failure report could not be uploaded', uploadError);
          }
        }
      }
    };

    const unsubscribe = subscribe((request) => {
      void handle(request);
    });
    return () => {
      active = false;
      unsubscribe();
      targetRef.current?.dispose();
      targetRef.current = null;
      pixelsRef.current = null;
      canvasRef.current = null;
    };
  }, [store, subscribe]);

  return null;
}
