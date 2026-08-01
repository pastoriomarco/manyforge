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
  requiredFrame?: { streamEpoch: bigint; sequence: bigint };
}

export function validateSnapshotRequest(request: SnapshotRequestView): string | null {
  if (!request.requestId.trim()) return 'Snapshot request ID is required';
  if (
    !Number.isInteger(request.width)
    || !Number.isInteger(request.height)
    || request.width < 1
    || request.height < 1
  ) {
    return 'Snapshot dimensions must be positive integers';
  }
  return null;
}

interface SnapshotResponderProps {
  subscribe: (handler: (request: SnapshotRequestView) => void) => () => void;
  upload: (requestId: string, result: Blob | null) => Promise<void>;
  ensureFrame?: (frame: NonNullable<SnapshotRequestView['requiredFrame']>) => Promise<boolean>;
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
  const targetRef = useRef<WebGLRenderTarget | null>(null);
  const pixelsRef = useRef<{
    bytes: Uint8Array<ArrayBuffer>;
    clamped: Uint8ClampedArray<ArrayBuffer>;
  } | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  useEffect(() => {
    let active = true;

    const capture = async (request: SnapshotRequestView): Promise<Blob> => {
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
      return canvasToBlobWithTimeout(canvas, request.mime, 0.9, 1_000);
    };

    const handle = async (request: SnapshotRequestView) => {
      try {
        if (request.requiredFrame) {
          if (!ensureFrame || !await ensureFrame(request.requiredFrame)) {
            throw new Error('Required viewer frame is unavailable');
          }
          applyFrame?.();
        }
        const blob = await capture(request);
        if (active) await upload(request.requestId, blob);
      } catch (error) {
        console.warn('snapshot capture failed', error);
        if (active) {
          try {
            await upload(request.requestId, null);
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
  }, [applyFrame, ensureFrame, store, subscribe, upload]);

  return null;
}
