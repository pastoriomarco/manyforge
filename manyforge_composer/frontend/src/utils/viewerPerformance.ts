import type { WebGLRenderer } from 'three';

const MAX_RENDER_INTERVAL_SAMPLES = 512;
const DEFAULT_REQUESTED_RATE_HZ = 15;
const VIEWER_RATE_STORAGE_KEY = 'manyforge.viewer.requested-rate-hz.v1';
export const VIEWER_RATE_OPTIONS_HZ = [2, 5, 10, 15, 30] as const;
export const VIEWER_MAX_REQUESTED_RATE_HZ = 30;
const SOFTWARE_RENDERER_TOKENS = [
  'llvmpipe',
  'softpipe',
  'swiftshader',
  'software rasterizer',
];

interface ViewerMetricWindow {
  startedAtMs: number;
  modelBuilds: number;
  modelBuildTotalMs: number;
  modelBuildMaxMs: number;
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
}

export interface ViewerRendererInspection {
  vendor: string | null;
  renderer: string | null;
  software: boolean;
}

export interface ViewerPresentationProfile {
  effectiveDpr: number;
  antialias: boolean;
  preserveDrawingBuffer: boolean;
}

export interface ViewerPerformanceSnapshot {
  windowDurationMs: number;
  modelBuilds: number;
  modelBuildTotalMs: number;
  modelBuildMaxMs: number;
  reactCommits: number;
  reactCommitTotalMs: number;
  reactCommitMaxMs: number;
  renderedFrames: number;
  drawCallsMax: number;
  trianglesMax: number;
  canvasPixelsMax: number;
  renderIntervalP95Ms: number | null;
  renderIntervalMaxMs: number | null;
  longTaskSupported: boolean;
  longTasks: number;
  longTaskTotalMs: number;
  longTaskMaxMs: number;
}

/*
 * Browser-local presentation preference retained from source blob
 * 24e7e692df7864293d4572c0ef3fe6e876b9de6f and test blob
 * a02c7500c92219c1cf857edade687af214b9bda7. The old renderer-profile lease and
 * ViewerClientDiagnosticsControlV1 publication are intentionally absent: I9
 * supplies the future data-plane client.  Persisting an operator's local
 * cadence choice does not assert that an unavailable runtime has applied it.
 */
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

let requestedRatePreferenceHz = loadViewerRatePreference();
const ratePreferenceListeners = new Set<() => void>();

export function getAutomaticViewerRateHz(): number {
  return DEFAULT_REQUESTED_RATE_HZ;
}

export function getViewerRatePreferenceHz(): number | null {
  return requestedRatePreferenceHz;
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
    // Storage can be disabled by browser policy. Keep the page-local choice.
  }
  for (const listener of ratePreferenceListeners) listener();
}

export function subscribeViewerRatePreference(listener: () => void): () => void {
  ratePreferenceListeners.add(listener);
  return () => ratePreferenceListeners.delete(listener);
}

function newWindow(nowMs = performance.now()): ViewerMetricWindow {
  return {
    startedAtMs: nowMs,
    modelBuilds: 0,
    modelBuildTotalMs: 0,
    modelBuildMaxMs: 0,
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
  };
}

export function isSoftwareRenderer(renderer: string | null): boolean {
  const normalized = renderer?.toLowerCase() ?? '';
  return SOFTWARE_RENDERER_TOKENS.some((token) => normalized.includes(token));
}

export function inspectViewerRenderer(
  gl: WebGLRenderer,
): ViewerRendererInspection {
  const context = gl.getContext();
  const debugInfo = context.getExtension('WEBGL_debug_renderer_info');
  const vendor = String(
    context.getParameter(debugInfo?.UNMASKED_VENDOR_WEBGL ?? context.VENDOR) ?? '',
  );
  const renderer = String(
    context.getParameter(debugInfo?.UNMASKED_RENDERER_WEBGL ?? context.RENDERER) ?? '',
  );
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
      effectiveDpr: 1,
      antialias: true,
      preserveDrawingBuffer: true,
    };
  }
  return {
    effectiveDpr: Math.min(finiteDpr, 1.5),
    antialias: true,
    preserveDrawingBuffer: false,
  };
}

function percentile95(values: number[]): number | null {
  if (values.length === 0) return null;
  const ordered = [...values].sort((left, right) => left - right);
  return ordered[Math.ceil(ordered.length * 0.95) - 1] ?? null;
}

let metrics = newWindow();
let observer: PerformanceObserver | null = null;
let monitoring = false;
const longTaskSupported = typeof PerformanceObserver !== 'undefined'
  && Array.isArray(PerformanceObserver.supportedEntryTypes)
  && PerformanceObserver.supportedEntryTypes.includes('longtask');

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

export function recordViewerModelBuild(durationMs: number): void {
  const duration = Math.max(0, durationMs);
  metrics.modelBuilds += 1;
  metrics.modelBuildTotalMs += duration;
  metrics.modelBuildMaxMs = Math.max(metrics.modelBuildMaxMs, duration);
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

export function takeViewerPerformanceWindow(): ViewerPerformanceSnapshot {
  const nowMs = performance.now();
  const result: ViewerPerformanceSnapshot = {
    windowDurationMs: Math.min(60_000, Math.max(0, nowMs - metrics.startedAtMs)),
    modelBuilds: metrics.modelBuilds,
    modelBuildTotalMs: metrics.modelBuildTotalMs,
    modelBuildMaxMs: metrics.modelBuildMaxMs,
    reactCommits: metrics.reactCommits,
    reactCommitTotalMs: metrics.reactCommitTotalMs,
    reactCommitMaxMs: metrics.reactCommitMaxMs,
    renderedFrames: metrics.renderedFrames,
    drawCallsMax: metrics.drawCallsMax,
    trianglesMax: metrics.trianglesMax,
    canvasPixelsMax: metrics.canvasPixelsMax,
    renderIntervalP95Ms: percentile95(metrics.renderIntervalsMs),
    renderIntervalMaxMs: metrics.renderIntervalsMs.length > 0
      ? Math.max(...metrics.renderIntervalsMs)
      : null,
    longTaskSupported,
    longTasks: metrics.longTasks,
    longTaskTotalMs: metrics.longTaskTotalMs,
    longTaskMaxMs: metrics.longTaskMaxMs,
  };
  metrics = newWindow(nowMs);
  return result;
}
