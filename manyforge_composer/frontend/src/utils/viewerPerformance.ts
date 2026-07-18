import type { ViewerClientDiagnosticsControlV1 } from '../types/viewerProtocol.generated';
import type { WebGLRenderer } from 'three';

const MAX_RENDER_INTERVAL_SAMPLES = 512;
const textEncoder = new TextEncoder();
let encodeScratch = new Uint8Array(4096);

interface ViewerMetricWindow {
  startedAtMs: number;
  websocketMessages: number;
  websocketBytes: number;
  manifestMessages: number;
  frameMessages: number;
  droppedFrameGaps: number;
  resyncRequests: number;
  materializations: number;
  materializationTotalMs: number;
  materializationMaxMs: number;
  reactCommits: number;
  reactCommitTotalMs: number;
  reactCommitMaxMs: number;
  renderedFrames: number;
  drawCallsMax: number;
  trianglesMax: number;
  canvasPixelsMax: number;
  renderIntervalsMs: number[];
  lastRenderAtMs: number | null;
  longTasks: number;
  longTaskTotalMs: number;
  longTaskMaxMs: number;
  topologyRebuilds: number;
  transformApplies: number;
}

export interface ViewerRendererProfile {
  vendor: string | null;
  renderer: string | null;
  software: boolean;
  effectiveDpr: number;
  antialias: boolean;
  preserveDrawingBuffer: boolean;
  requestedRateHz: number;
}

export interface ViewerPresentationProfile {
  effectiveDpr: number;
  antialias: boolean;
  preserveDrawingBuffer: boolean;
}

const DEFAULT_REQUESTED_RATE_HZ = 15;
// Auto preserves the historical viewer cadence across renderer types. Operators
// can select a lower local rate when software rendering or host load requires it.
export const VIEWER_RATE_OPTIONS_HZ = [2, 5, 10, 15, 30] as const;
export const VIEWER_MAX_REQUESTED_RATE_HZ = 30;
const VIEWER_RATE_STORAGE_KEY = 'manyforge.viewer.requested-rate-hz.v1';
const SOFTWARE_RENDERER_TOKENS = [
  'llvmpipe',
  'softpipe',
  'swiftshader',
  'software rasterizer',
];

function loadViewerRatePreference(): number | null {
  if (typeof window === 'undefined') return null;
  try {
    const raw = window.localStorage.getItem(VIEWER_RATE_STORAGE_KEY);
    if (raw === null || raw === 'auto') return null;
    const rate = Number(raw);
    return Number.isFinite(rate) && rate > 0 && rate <= VIEWER_MAX_REQUESTED_RATE_HZ
      ? rate
      : null;
  } catch {
    return null;
  }
}

function automaticViewerRateHz(): number {
  return DEFAULT_REQUESTED_RATE_HZ;
}

let requestedRatePreferenceHz = loadViewerRatePreference();

let rendererProfile: ViewerRendererProfile = {
  vendor: null,
  renderer: null,
  software: false,
  effectiveDpr: 1,
  antialias: true,
  preserveDrawingBuffer: false,
  requestedRateHz: requestedRatePreferenceHz ?? DEFAULT_REQUESTED_RATE_HZ,
};
let reportedEffectiveDpr = rendererProfile.effectiveDpr;
const rendererProfileListeners = new Set<() => void>();

function newWindow(nowMs = performance.now()): ViewerMetricWindow {
  return {
    startedAtMs: nowMs,
    websocketMessages: 0,
    websocketBytes: 0,
    manifestMessages: 0,
    frameMessages: 0,
    droppedFrameGaps: 0,
    resyncRequests: 0,
    materializations: 0,
    materializationTotalMs: 0,
    materializationMaxMs: 0,
    reactCommits: 0,
    reactCommitTotalMs: 0,
    reactCommitMaxMs: 0,
    renderedFrames: 0,
    drawCallsMax: 0,
    trianglesMax: 0,
    canvasPixelsMax: 0,
    renderIntervalsMs: [],
    lastRenderAtMs: null,
    longTasks: 0,
    longTaskTotalMs: 0,
    longTaskMaxMs: 0,
    topologyRebuilds: 0,
    transformApplies: 0,
  };
}

export function isSoftwareRenderer(renderer: string | null): boolean {
  const normalized = renderer?.toLowerCase() ?? '';
  return SOFTWARE_RENDERER_TOKENS.some((token) => normalized.includes(token));
}

export function inspectViewerRenderer(gl: WebGLRenderer): {
  vendor: string | null;
  renderer: string | null;
  software: boolean;
} {
  const context = gl.getContext();
  const debugInfo = context.getExtension('WEBGL_debug_renderer_info');
  const vendor = String(context.getParameter(
    debugInfo?.UNMASKED_VENDOR_WEBGL ?? context.VENDOR,
  ) ?? '');
  const renderer = String(context.getParameter(
    debugInfo?.UNMASKED_RENDERER_WEBGL ?? context.RENDERER,
  ) ?? '');
  return {
    vendor: vendor || null,
    renderer: renderer || null,
    software: isSoftwareRenderer(renderer),
  };
}

export function selectViewerPresentationProfile(
  software: boolean,
  devicePixelRatio: number,
): ViewerPresentationProfile {
  const finiteDpr = Number.isFinite(devicePixelRatio) && devicePixelRatio > 0
    ? devicePixelRatio
    : 1;
  if (software) {
    return {
      // Half-resolution software frames made thin geometry unreadable and did
      // not materially bound camera-orbit work. Keep a one-device-pixel floor
      // while the lower data cadence contains continuous observer load.
      effectiveDpr: 1,
      antialias: true,
      // Firefox/llvmpipe can present incomplete default-framebuffer tiles
      // during demand-driven camera motion when the buffer is discarded after
      // compositing. Preservation is a software-only correctness workaround;
      // the hardware path keeps the cheaper transient buffer.
      preserveDrawingBuffer: true,
    };
  }
  return {
    effectiveDpr: Math.min(finiteDpr, 1.5),
    antialias: true,
    preserveDrawingBuffer: false,
  };
}

export function publishViewerRendererProfile(
  profile: Omit<ViewerRendererProfile, 'requestedRateHz'>,
): ViewerRendererProfile {
  rendererProfile = {
    ...profile,
    requestedRateHz:
      requestedRatePreferenceHz ?? automaticViewerRateHz(),
  };
  reportedEffectiveDpr = profile.effectiveDpr;
  for (const listener of rendererProfileListeners) listener();
  return rendererProfile;
}

export function getViewerRendererProfile(): ViewerRendererProfile {
  return rendererProfile;
}

export function getViewerRatePreferenceHz(): number | null {
  return requestedRatePreferenceHz;
}

export function getAutomaticViewerRateHz(): number {
  return automaticViewerRateHz();
}

export function setViewerRatePreferenceHz(rateHz: number | null): void {
  if (
    rateHz !== null
    && (!Number.isFinite(rateHz) || rateHz <= 0 || rateHz > VIEWER_MAX_REQUESTED_RATE_HZ)
  ) {
    throw new RangeError(
      `Viewer update rate must be greater than 0 and at most ${VIEWER_MAX_REQUESTED_RATE_HZ} Hz`,
    );
  }
  if (requestedRatePreferenceHz === rateHz) return;
  requestedRatePreferenceHz = rateHz;
  try {
    if (typeof window !== 'undefined') {
      if (rateHz === null) {
        window.localStorage.removeItem(VIEWER_RATE_STORAGE_KEY);
      } else {
        window.localStorage.setItem(VIEWER_RATE_STORAGE_KEY, String(rateHz));
      }
    }
  } catch {
    // Private browsing or an embedded policy may disable storage. The active
    // client preference remains valid for this page session.
  }
  rendererProfile = {
    ...rendererProfile,
    requestedRateHz: rateHz ?? automaticViewerRateHz(),
  };
  for (const listener of rendererProfileListeners) listener();
}

export function recordViewerEffectiveDpr(effectiveDpr: number): void {
  if (!Number.isFinite(effectiveDpr) || effectiveDpr <= 0) return;
  reportedEffectiveDpr = effectiveDpr;
}

export function subscribeViewerRendererProfile(listener: () => void): () => void {
  rendererProfileListeners.add(listener);
  return () => rendererProfileListeners.delete(listener);
}

let metrics = newWindow();
let observer: PerformanceObserver | null = null;
let monitoring = false;
const longTaskSupported = typeof PerformanceObserver !== 'undefined'
  && Array.isArray(PerformanceObserver.supportedEntryTypes)
  && PerformanceObserver.supportedEntryTypes.includes('longtask');

function encodedByteLength(value: string): number {
  const required = Math.max(1, value.length * 3);
  if (encodeScratch.byteLength < required) {
    let capacity = encodeScratch.byteLength;
    while (capacity < required) capacity *= 2;
    encodeScratch = new Uint8Array(capacity);
  }
  const result = textEncoder.encodeInto(value, encodeScratch);
  return result.written;
}

function percentile95(values: number[]): number | null {
  if (values.length === 0) return null;
  const ordered = [...values].sort((a, b) => a - b);
  return ordered[Math.ceil(ordered.length * 0.95) - 1] ?? null;
}

export function startViewerPerformanceMonitoring(): void {
  if (monitoring) return;
  monitoring = true;
  metrics = newWindow();
  if (!longTaskSupported) return;
  observer = new PerformanceObserver((entries) => {
    for (const entry of entries.getEntries()) {
      metrics.longTasks += 1;
      metrics.longTaskTotalMs += entry.duration;
      metrics.longTaskMaxMs = Math.max(metrics.longTaskMaxMs, entry.duration);
    }
  });
  observer.observe({ type: 'longtask', buffered: false });
}

export function stopViewerPerformanceMonitoring(): void {
  monitoring = false;
  observer?.disconnect();
  observer = null;
}

export function recordViewerWebSocketMessage(value: string, type: unknown): void {
  metrics.websocketMessages += 1;
  metrics.websocketBytes += encodedByteLength(value);
  if (type === 'manifest') metrics.manifestMessages += 1;
  if (type === 'frame') metrics.frameMessages += 1;
}

export function recordViewerFrameGap(count: number): void {
  metrics.droppedFrameGaps += Math.max(0, Math.trunc(count));
}

export function recordViewerResyncRequest(): void {
  metrics.resyncRequests += 1;
}

export function recordViewerMaterialization(durationMs: number): void {
  const duration = Math.max(0, durationMs);
  metrics.materializations += 1;
  metrics.materializationTotalMs += duration;
  metrics.materializationMaxMs = Math.max(metrics.materializationMaxMs, duration);
}

export function recordViewerReactCommit(durationMs: number): void {
  const duration = Math.max(0, durationMs);
  metrics.reactCommits += 1;
  metrics.reactCommitTotalMs += duration;
  metrics.reactCommitMaxMs = Math.max(metrics.reactCommitMaxMs, duration);
}

export function recordViewerRenderFrame(nowMs: number): void {
  metrics.renderedFrames += 1;
  if (metrics.lastRenderAtMs !== null) {
    if (metrics.renderIntervalsMs.length >= MAX_RENDER_INTERVAL_SAMPLES) {
      metrics.renderIntervalsMs.shift();
    }
    metrics.renderIntervalsMs.push(Math.max(0, nowMs - metrics.lastRenderAtMs));
  }
  metrics.lastRenderAtMs = nowMs;
}

export function recordViewerRenderWork(
  drawCalls: number,
  triangles: number,
  canvasPixels: number,
): void {
  metrics.drawCallsMax = Math.max(metrics.drawCallsMax, Math.max(0, Math.trunc(drawCalls)));
  metrics.trianglesMax = Math.max(metrics.trianglesMax, Math.max(0, Math.trunc(triangles)));
  metrics.canvasPixelsMax = Math.max(
    metrics.canvasPixelsMax,
    Math.max(0, Math.trunc(canvasPixels)),
  );
}

export function recordViewerTopologyRebuild(count = 1): void {
  metrics.topologyRebuilds += Math.max(0, Math.trunc(count));
}

export function recordViewerTransformApply(count = 1): void {
  metrics.transformApplies += Math.max(0, Math.trunc(count));
}

export function takeViewerPerformanceWindow(
  visibility: 'focused' | 'visible' | 'collapsed' | 'hidden',
): ViewerClientDiagnosticsControlV1 {
  const nowMs = performance.now();
  const intervals = metrics.renderIntervalsMs;
  const heap = (
    performance as Performance & { memory?: { usedJSHeapSize?: number } }
  ).memory?.usedJSHeapSize;
  const result: ViewerClientDiagnosticsControlV1 = {
    type: 'client_diagnostics',
    schema_version: 'manyforge.viewer.control.v1',
    visibility,
    window_duration_ms: Math.min(60_000, Math.max(0, nowMs - metrics.startedAtMs)),
    websocket_messages: metrics.websocketMessages,
    websocket_bytes: metrics.websocketBytes,
    manifest_messages: metrics.manifestMessages,
    frame_messages: metrics.frameMessages,
    dropped_frame_gaps: metrics.droppedFrameGaps,
    resync_requests: metrics.resyncRequests,
    materializations: metrics.materializations,
    materialization_total_ms: metrics.materializationTotalMs,
    materialization_max_ms: metrics.materializationMaxMs,
    react_commits: metrics.reactCommits,
    react_commit_total_ms: metrics.reactCommitTotalMs,
    react_commit_max_ms: metrics.reactCommitMaxMs,
    rendered_frames: metrics.renderedFrames,
    draw_calls_max: metrics.drawCallsMax,
    triangles_max: metrics.trianglesMax,
    canvas_pixels_max: metrics.canvasPixelsMax,
    render_interval_p95_ms: percentile95(intervals),
    render_interval_max_ms: intervals.length > 0 ? Math.max(...intervals) : null,
    long_task_supported: longTaskSupported,
    long_tasks: metrics.longTasks,
    long_task_total_ms: metrics.longTaskTotalMs,
    long_task_max_ms: metrics.longTaskMaxMs,
    heap_used_bytes: typeof heap === 'number' && Number.isFinite(heap) ? heap : null,
    topology_rebuilds: metrics.topologyRebuilds,
    transform_applies: metrics.transformApplies,
    renderer_vendor: rendererProfile.vendor,
    renderer_name: rendererProfile.renderer,
    renderer_software: rendererProfile.software,
    effective_dpr: reportedEffectiveDpr,
    requested_rate_hz: rendererProfile.requestedRateHz,
  };
  metrics = newWindow(nowMs);
  return result;
}

export const __viewerPerformanceTest = {
  encodedByteLength,
  isSoftwareRenderer,
  percentile95,
};
