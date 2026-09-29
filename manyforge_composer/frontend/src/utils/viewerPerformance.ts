import type { ViewerGraphicsProfile } from '../state/composerSettings';
import type { WebGLRenderer } from 'three';

const MAX_RENDER_INTERVAL_SAMPLES = 512;
const MAX_OBSERVATION_INTERVAL_SAMPLES = 512;
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

interface ViewerObservationMetricWindow {
  receivedFrames: number;
  appliedFrames: number;
  replacedBeforeApply: number;
  maxUnappliedFrames: number;
  lastReceived: ViewerObservationIdentity | null;
  lastApplied: ViewerObservationIdentity | null;
  receivedAtMs: number | null;
  appliedAtMs: number | null;
  applyIntervalsMs: number[];
  receiveToApplyMs: number[];
}

export interface ViewerObservationIdentity {
  ownerEpoch: string;
  sourceEpoch: string;
  sourceSequence: number;
  receivedAtComposerMonotonicUs: number;
}

export interface ViewerObservationPerformanceSnapshot {
  receivedFrames: number;
  appliedFrames: number;
  replacedBeforeApply: number;
  maxUnappliedFrames: number;
  lastReceived: ViewerObservationIdentity | null;
  lastApplied: ViewerObservationIdentity | null;
  applyIntervalP95Ms: number | null;
  applyIntervalMaxMs: number | null;
  receiveToApplyP95Ms: number | null;
  receiveToApplyMaxMs: number | null;
}

export interface ViewerRendererInspection {
  vendor: string | null;
  renderer: string | null;
  software: boolean;
}

export interface ViewerPresentationProfile {
  effectiveDpr: number;
  antialias: boolean;
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

export const VIEWER_QUALIFICATION_QUERY_KEY = 'manyforge_viewer_qualification';
export const VIEWER_QUALIFICATION_GLOBAL = 'manyforgeViewerQualification';
const VIEWER_QUALIFICATION_SCHEMA = 'manyforge.viewer.qualification.v1';
const MAX_QUALIFICATION_ERRORS = 16;
const MAX_QUALIFICATION_ERROR_LENGTH = 240;

export interface ViewerQualificationTransportState {
  socketPath: '/ws/scene/viewer';
  sourceCount: number;
  activeSockets: number;
  socketCreations: number;
  maxActiveSockets: number;
}

export interface ViewerQualificationStatus {
  schemaVersion: typeof VIEWER_QUALIFICATION_SCHEMA;
  viewerActive: boolean;
  canvasCount: number;
  wireframeDefault: boolean;
  wireframeVisible: boolean;
  model: {
    ready: boolean;
    geometryCount: number;
  };
  renderer: (ViewerRendererInspection & ViewerPresentationProfile) | null;
  browser: {
    userAgent: string;
    platform: string;
  };
  viewport: {
    width: number;
    height: number;
    devicePixelRatio: number;
  };
  headless: ViewerQualificationTransportState;
  errors: string[];
}

export interface ViewerQualificationSnapshot {
  schemaVersion: typeof VIEWER_QUALIFICATION_SCHEMA;
  status: ViewerQualificationStatus;
  performance: ViewerPerformanceSnapshot;
}

export interface ViewerQualificationObject {
  schemaVersion: typeof VIEWER_QUALIFICATION_SCHEMA;
  status: () => ViewerQualificationStatus;
  snapshot: () => ViewerQualificationSnapshot;
  take: () => ViewerQualificationSnapshot;
  reset: () => void;
  observationFlow: () => ViewerObservationPerformanceSnapshot;
  resetObservationFlow: () => void;
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

function newObservationWindow(): ViewerObservationMetricWindow {
  return {
    receivedFrames: 0,
    appliedFrames: 0,
    replacedBeforeApply: 0,
    maxUnappliedFrames: 0,
    lastReceived: null,
    lastApplied: null,
    receivedAtMs: null,
    appliedAtMs: null,
    applyIntervalsMs: [],
    receiveToApplyMs: [],
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
  // Firefox may privacy-sanitize the debug renderer to a generic NVIDIA name
  // even when WebGL is running through llvmpipe. The base renderer remains an
  // accurate software signal in that case, so use both without weakening the
  // hardware identity reported when neither is software-rendered.
  const baseRenderer = String(context.getParameter(context.RENDERER) ?? '');
  const softwareRenderer = [renderer, baseRenderer].find(isSoftwareRenderer);
  return {
    vendor: vendor || null,
    renderer: softwareRenderer || renderer || baseRenderer || null,
    software: softwareRenderer !== undefined,
  };
}

/** The renderer settings for this browser, from its graphics profile. */
export function selectViewerPresentationProfile(
  devicePixelRatio: number,
  graphics: Pick<ViewerGraphicsProfile, 'maxPixelRatio' | 'antialias'>,
): ViewerPresentationProfile {
  const finiteDpr = Number.isFinite(devicePixelRatio) && devicePixelRatio > 0
    ? devicePixelRatio
    : 1;
  return {
    effectiveDpr: Math.min(finiteDpr, graphics.maxPixelRatio),
    antialias: graphics.antialias,
  };
}

function percentile95(values: number[]): number | null {
  if (values.length === 0) return null;
  const ordered = [...values].sort((left, right) => left - right);
  return ordered[Math.ceil(ordered.length * 0.95) - 1] ?? null;
}

let metrics = newWindow();
let observationMetrics = newObservationWindow();
let observer: PerformanceObserver | null = null;
let monitoring = false;
let observationMonitoring = false;
let pendingModelBuild: number | null = null;
let qualificationViewerActive = false;
let qualificationCanvasMounted = false;
let qualificationModelReady = false;
let qualificationGeometryCount = 0;
let qualificationRenderer:
  (ViewerRendererInspection & ViewerPresentationProfile) | null = null;
let qualificationWireframeDefault = false;
let qualificationTransport: ViewerQualificationTransportState = {
  socketPath: '/ws/scene/viewer',
  sourceCount: 0,
  activeSockets: 0,
  socketCreations: 0,
  maxActiveSockets: 0,
};
let qualificationErrors: string[] = [];
const longTaskSupported = typeof PerformanceObserver !== 'undefined'
  && Array.isArray(PerformanceObserver.supportedEntryTypes)
  && PerformanceObserver.supportedEntryTypes.includes('longtask');

function isViewerQualificationOptedIn(): boolean {
  if (typeof window === 'undefined') return false;
  try {
    const values = new URL(window.location.href).searchParams.getAll(
      VIEWER_QUALIFICATION_QUERY_KEY,
    );
    return values.length === 1 && values[0] === '1';
  } catch {
    return false;
  }
}

export function viewerQualificationOptedIn(): boolean {
  return isViewerQualificationOptedIn();
}

function boundedErrorMessage(value: unknown): string {
  const message = value instanceof Error ? value.message : String(value);
  return message.slice(0, MAX_QUALIFICATION_ERROR_LENGTH);
}

export function reportViewerQualificationError(error: unknown): void {
  if (!isViewerQualificationOptedIn()) return;
  const message = boundedErrorMessage(error);
  if (message.length === 0 || qualificationErrors.includes(message)) return;
  qualificationErrors = [...qualificationErrors, message].slice(0, MAX_QUALIFICATION_ERRORS);
}

export function setViewerQualificationViewerActive(active: boolean): void {
  qualificationViewerActive = active;
}

export function setViewerQualificationCanvasMounted(mounted: boolean): void {
  qualificationCanvasMounted = mounted;
}

export function setViewerQualificationModel(
  ready: boolean,
  geometryCount: number,
): void {
  qualificationModelReady = ready;
  qualificationGeometryCount = Math.max(0, Math.trunc(geometryCount));
}

export function setViewerQualificationRenderer(
  renderer: (ViewerRendererInspection & ViewerPresentationProfile) | null,
): void {
  qualificationRenderer = renderer === null
    ? null
    : {
        ...renderer,
        vendor: renderer.vendor?.slice(0, 240) ?? null,
        renderer: renderer.renderer?.slice(0, 240) ?? null,
      };
}

export function setViewerQualificationTransport(
  transport: ViewerQualificationTransportState,
): void {
  qualificationTransport = {
    socketPath: transport.socketPath,
    sourceCount: Math.min(1_000_000, Math.max(0, Math.trunc(transport.sourceCount))),
    activeSockets: Math.min(1_000_000, Math.max(0, Math.trunc(transport.activeSockets))),
    socketCreations: Math.min(
      1_000_000_000,
      Math.max(0, Math.trunc(transport.socketCreations)),
    ),
    maxActiveSockets: Math.min(
      1_000_000,
      Math.max(0, Math.trunc(transport.maxActiveSockets)),
    ),
  };
}

function currentQualificationStatus(): ViewerQualificationStatus {
  const browser = typeof navigator === 'undefined'
    ? { userAgent: '', platform: '' }
    : {
        userAgent: typeof navigator.userAgent === 'string'
          ? navigator.userAgent.slice(0, 240)
          : '',
        platform: typeof navigator.platform === 'string'
          ? navigator.platform.slice(0, 120)
          : '',
      };
  const canvasCount = typeof document === 'undefined'
    ? qualificationCanvasMounted ? 1 : 0
    : document.querySelectorAll('.viewer-stage canvas').length;
  const viewport = typeof window === 'undefined'
    ? { width: 0, height: 0, devicePixelRatio: 1 }
    : {
        width: Number.isFinite(window.innerWidth)
          ? Math.max(0, Math.trunc(window.innerWidth))
          : 0,
        height: Number.isFinite(window.innerHeight)
          ? Math.max(0, Math.trunc(window.innerHeight))
          : 0,
        devicePixelRatio: Number.isFinite(window.devicePixelRatio)
          ? window.devicePixelRatio
          : 1,
      };
  return {
    schemaVersion: VIEWER_QUALIFICATION_SCHEMA,
    viewerActive: qualificationViewerActive,
    canvasCount,
    wireframeDefault: qualificationWireframeDefault,
    wireframeVisible: qualificationWireframeVisible,
    model: {
      ready: qualificationModelReady,
      geometryCount: qualificationGeometryCount,
    },
    renderer: qualificationRenderer === null ? null : { ...qualificationRenderer },
    browser,
    viewport,
    headless: { ...qualificationTransport },
    errors: [...qualificationErrors],
  };
}

let qualificationWireframeVisible = false;

export function setViewerQualificationWireframeDefault(defaultVisible: boolean): void {
  qualificationWireframeDefault = defaultVisible;
}

export function setViewerQualificationWireframeVisible(visible: boolean): void {
  qualificationWireframeVisible = visible;
}

function performanceSnapshot(nowMs = performance.now()): ViewerPerformanceSnapshot {
  return {
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
}

function boundedSample(samples: number[], value: number): void {
  if (samples.length >= MAX_OBSERVATION_INTERVAL_SAMPLES) samples.shift();
  samples.push(Math.min(60_000, Math.max(0, value)));
}

function observationPerformanceSnapshot(): ViewerObservationPerformanceSnapshot {
  return {
    receivedFrames: observationMetrics.receivedFrames,
    appliedFrames: observationMetrics.appliedFrames,
    replacedBeforeApply: observationMetrics.replacedBeforeApply,
    maxUnappliedFrames: observationMetrics.maxUnappliedFrames,
    lastReceived: observationMetrics.lastReceived === null
      ? null : { ...observationMetrics.lastReceived },
    lastApplied: observationMetrics.lastApplied === null
      ? null : { ...observationMetrics.lastApplied },
    applyIntervalP95Ms: percentile95(observationMetrics.applyIntervalsMs),
    applyIntervalMaxMs: observationMetrics.applyIntervalsMs.length === 0
      ? null : Math.max(...observationMetrics.applyIntervalsMs),
    receiveToApplyP95Ms: percentile95(observationMetrics.receiveToApplyMs),
    receiveToApplyMaxMs: observationMetrics.receiveToApplyMs.length === 0
      ? null : Math.max(...observationMetrics.receiveToApplyMs),
  };
}

export function connectViewerQualification(): (() => void) {
  if (!isViewerQualificationOptedIn() || typeof window === 'undefined') {
    return () => undefined;
  }
  const onError = (event: ErrorEvent) => {
    const message = event.message || event.error;
    if (/(canvas|webgl|three|viewer|scene|model|shader)/i.test(String(message))) {
      reportViewerQualificationError(message);
    }
  };
  const onUnhandledRejection = (event: PromiseRejectionEvent) => {
    if (/(canvas|webgl|three|viewer|scene|model|shader)/i.test(String(event.reason))) {
      reportViewerQualificationError(event.reason);
    }
  };
  const originalConsoleError = console.error;
  observationMonitoring = true;
  const onConsoleError = (...values: unknown[]) => {
    const message = values.map((value) => String(value)).join(' ');
    if (/(canvas|webgl|three|viewer|scene|model|shader)/i.test(message)) {
      reportViewerQualificationError(message);
    }
    originalConsoleError(...values);
  };
  window.addEventListener('error', onError);
  window.addEventListener('unhandledrejection', onUnhandledRejection);
  console.error = onConsoleError;
  const qualification: ViewerQualificationObject = Object.freeze({
    schemaVersion: VIEWER_QUALIFICATION_SCHEMA,
    status: currentQualificationStatus,
    snapshot: (): ViewerQualificationSnapshot => ({
      schemaVersion: VIEWER_QUALIFICATION_SCHEMA,
      status: currentQualificationStatus(),
      performance: performanceSnapshot(),
    }),
    take: (): ViewerQualificationSnapshot => {
      const nowMs = performance.now();
      const result: ViewerQualificationSnapshot = {
        schemaVersion: VIEWER_QUALIFICATION_SCHEMA,
        status: currentQualificationStatus(),
        performance: performanceSnapshot(nowMs),
      };
      metrics = newWindow(nowMs);
      return result;
    },
    reset: () => {
      metrics = newWindow();
    },
    observationFlow: observationPerformanceSnapshot,
    resetObservationFlow: () => {
      observationMetrics = newObservationWindow();
    },
  });
  Object.defineProperty(window, VIEWER_QUALIFICATION_GLOBAL, {
    configurable: true,
    enumerable: false,
    writable: false,
    value: qualification,
  });
  return () => {
    window.removeEventListener('error', onError);
    window.removeEventListener('unhandledrejection', onUnhandledRejection);
    if (console.error === onConsoleError) console.error = originalConsoleError;
    observationMonitoring = false;
    const windowValues = window as unknown as Window & Record<string, unknown>;
    if (windowValues[VIEWER_QUALIFICATION_GLOBAL] === qualification) {
      delete windowValues[VIEWER_QUALIFICATION_GLOBAL];
    }
  };
}

export function recordViewerObservationReceived(
  identity: ViewerObservationIdentity,
  replacedBeforeApply: boolean,
): void {
  if (!observationMonitoring) return;
  observationMetrics.receivedFrames += 1;
  if (replacedBeforeApply) observationMetrics.replacedBeforeApply += 1;
  observationMetrics.maxUnappliedFrames = Math.max(
    observationMetrics.maxUnappliedFrames,
    1,
  );
  observationMetrics.lastReceived = { ...identity };
  observationMetrics.receivedAtMs = performance.now();
}

export function recordViewerObservationApplied(
  identity: ViewerObservationIdentity,
): void {
  if (!observationMonitoring) return;
  const nowMs = performance.now();
  observationMetrics.appliedFrames += 1;
  if (observationMetrics.appliedAtMs !== null) {
    boundedSample(
      observationMetrics.applyIntervalsMs,
      nowMs - observationMetrics.appliedAtMs,
    );
  }
  if (observationMetrics.receivedAtMs !== null) {
    boundedSample(
      observationMetrics.receiveToApplyMs,
      nowMs - observationMetrics.receivedAtMs,
    );
  }
  observationMetrics.appliedAtMs = nowMs;
  observationMetrics.lastApplied = { ...identity };
}

export function startViewerPerformanceMonitoring(): void {
  if (monitoring) return;
  monitoring = true;
  const retainedModelBuild = metrics.modelBuilds > 0
    ? {
        count: metrics.modelBuilds,
        totalMs: metrics.modelBuildTotalMs,
        maxMs: metrics.modelBuildMaxMs,
      }
    : null;
  metrics = newWindow();
  if (retainedModelBuild !== null) {
    metrics.modelBuilds = retainedModelBuild.count;
    metrics.modelBuildTotalMs = retainedModelBuild.totalMs;
    metrics.modelBuildMaxMs = retainedModelBuild.maxMs;
  } else if (pendingModelBuild !== null) {
    metrics.modelBuilds = 1;
    metrics.modelBuildTotalMs = pendingModelBuild;
    metrics.modelBuildMaxMs = pendingModelBuild;
    pendingModelBuild = null;
  }
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
  const duration = Math.min(60_000, Math.max(0, durationMs));
  if (!monitoring) {
    pendingModelBuild = duration;
    return;
  }
  metrics.modelBuilds += 1;
  metrics.modelBuildTotalMs += duration;
  metrics.modelBuildMaxMs = Math.max(metrics.modelBuildMaxMs, duration);
}

export function recordViewerReactCommit(durationMs: number): void {
  if (!monitoring) return;
  const duration = Math.min(60_000, Math.max(0, durationMs));
  metrics.reactCommits += 1;
  metrics.reactCommitTotalMs += duration;
  metrics.reactCommitMaxMs = Math.max(metrics.reactCommitMaxMs, duration);
}

export function recordViewerRenderFrame(nowMs: number): void {
  if (!monitoring) return;
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
  if (!monitoring) return;
  metrics.drawCallsMax = Math.max(metrics.drawCallsMax, Math.max(0, Math.trunc(drawCalls)));
  metrics.trianglesMax = Math.max(metrics.trianglesMax, Math.max(0, Math.trunc(triangles)));
  metrics.canvasPixelsMax = Math.max(
    metrics.canvasPixelsMax,
    Math.max(0, Math.trunc(canvasPixels)),
  );
}

export function takeViewerPerformanceWindow(): ViewerPerformanceSnapshot {
  const nowMs = performance.now();
  const result = performanceSnapshot(nowMs);
  metrics = newWindow(nowMs);
  return result;
}

export function peekViewerPerformanceWindow(): ViewerPerformanceSnapshot {
  return performanceSnapshot();
}
