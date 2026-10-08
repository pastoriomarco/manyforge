import { DEFAULT_COMPOSER_SETTINGS } from '../state/composerSettings';
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  VIEWER_QUALIFICATION_GLOBAL,
  VIEWER_QUALIFICATION_QUERY_KEY,
  connectViewerQualification,
  inspectViewerRenderer,
  isSoftwareRenderer,
  pageViewerRendererInspection,
  recordViewerModelBuild,
  recordViewerObservationApplied,
  recordViewerObservationReceived,
  recordViewerReactCommit,
  recordViewerRenderFrame,
  recordViewerRenderWork,
  rememberViewerRendererInspection,
  resetViewerRendererInspectionForTest,
  selectViewerPresentationProfile,
  startViewerPerformanceMonitoring,
  stopViewerPerformanceMonitoring,
  takeViewerPerformanceWindow,
} from './viewerPerformance';

describe('viewer presentation and performance', () => {
  afterEach(() => {
    stopViewerPerformanceMonitoring();
    vi.unstubAllGlobals();
  });

  it('selects a bounded software or hardware presentation profile', () => {
    expect(isSoftwareRenderer('llvmpipe (LLVM 20.1.2)')).toBe(true);
    expect(isSoftwareRenderer('NVIDIA Tegra')).toBe(false);
    // The defaults reproduce the former fixed software and GPU choices.
    expect(selectViewerPresentationProfile(2, DEFAULT_COMPOSER_SETTINGS.viewerGraphics.software)).toEqual({
      effectiveDpr: 1,
      antialias: false,
    });
    expect(selectViewerPresentationProfile(2, DEFAULT_COMPOSER_SETTINGS.viewerGraphics.gpu)).toEqual({
      effectiveDpr: 1.5,
      antialias: true,
    });
    // The cap never raises the resolution above the screen's own.
    expect(selectViewerPresentationProfile(1, { maxPixelRatio: 2, antialias: true }).effectiveDpr).toBe(1);
  });

  it('detects Firefox software rendering beneath a sanitized debug identity', () => {
    const debugInfo = {
      UNMASKED_VENDOR_WEBGL: 1,
      UNMASKED_RENDERER_WEBGL: 2,
    };
    const context = {
      VENDOR: 3,
      RENDERER: 4,
      getExtension: vi.fn(() => debugInfo),
      getParameter: vi.fn((parameter: number) => ({
        1: 'NVIDIA Corporation',
        2: 'NVIDIA GeForce GTX 980, or similar',
        4: 'llvmpipe, or similar',
      })[parameter] ?? ''),
    };
    const gl = {
      getContext: () => context,
    } as never;

    expect(inspectViewerRenderer(gl)).toEqual({
      vendor: 'NVIDIA Corporation',
      renderer: 'llvmpipe, or similar',
      software: true,
    });
  });

  it('inspects the page renderer once, before any viewer Canvas exists', () => {
    resetViewerRendererInspectionForTest();
    const context = {
      VENDOR: 3,
      RENDERER: 4,
      getExtension: vi.fn(() => null),
      getParameter: vi.fn((parameter: number) => ({
        3: 'Mesa',
        4: 'llvmpipe (LLVM 15.0.7, 128 bits)',
      })[parameter] ?? ''),
    };
    const getContext = vi.fn(() => context);
    const createCanvas = vi.fn(() => ({ getContext }) as never);

    const first = pageViewerRendererInspection(createCanvas);
    expect(first).toEqual({
      vendor: 'Mesa',
      renderer: 'llvmpipe (LLVM 15.0.7, 128 bits)',
      software: true,
    });
    // Later mounts reuse the page result: no second probe context.
    expect(pageViewerRendererInspection(createCanvas)).toBe(first);
    expect(createCanvas).toHaveBeenCalledTimes(1);
    expect(getContext).toHaveBeenCalledWith('webgl2', expect.objectContaining({ alpha: false }));
    // An equal Canvas inspection keeps the cached identity (no state flip).
    expect(rememberViewerRendererInspection({ ...first! })).toBe(first);

    resetViewerRendererInspectionForTest();
    expect(pageViewerRendererInspection(() => ({ getContext: () => null }) as never)).toBeNull();
    const created = rememberViewerRendererInspection({
      vendor: 'NVIDIA', renderer: 'Tegra', software: false,
    });
    expect(pageViewerRendererInspection(createCanvas)).toBe(created);
    expect(createCanvas).toHaveBeenCalledTimes(1);
    resetViewerRendererInspectionForTest();
  });

  it('reports only local render work and resets the window', () => {
    startViewerPerformanceMonitoring();
    recordViewerModelBuild(2.5);
    recordViewerReactCommit(1.25);
    recordViewerRenderFrame(100);
    recordViewerRenderFrame(116);
    recordViewerRenderWork(17, 12_345, 640_000);

    const first = takeViewerPerformanceWindow();
    expect(first.modelBuildTotalMs).toBe(2.5);
    expect(first.reactCommitTotalMs).toBe(1.25);
    expect(first.renderedFrames).toBe(2);
    expect(first.renderIntervalP95Ms).toBe(16);
    expect(first.drawCallsMax).toBe(17);
    expect(first.trianglesMax).toBe(12_345);
    expect(first.canvasPixelsMax).toBe(640_000);

    const second = takeViewerPerformanceWindow();
    expect(second.modelBuilds).toBe(0);
    expect(second.renderedFrames).toBe(0);
    expect(second.renderIntervalP95Ms).toBeNull();
  });

  it('installs one frozen qualification object only for the exact URL opt-in', () => {
    const fakeWindow = {
      location: { href: `http://composer.test/?${VIEWER_QUALIFICATION_QUERY_KEY}=1` },
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
    } as unknown as Window & Record<string, unknown>;
    vi.stubGlobal('window', fakeWindow);

    const disconnect = connectViewerQualification();
    const qualification = fakeWindow[VIEWER_QUALIFICATION_GLOBAL] as {
      schemaVersion: string;
      status: () => unknown;
      snapshot: () => unknown;
      take: () => unknown;
      reset: () => void;
      observationFlow: () => unknown;
      resetObservationFlow: () => void;
    };
    expect(qualification.schemaVersion).toBe('manyforge.viewer.qualification.v1');
    expect(Object.isFrozen(qualification)).toBe(true);
    expect(qualification.status()).toEqual(
      expect.objectContaining({
        wireframeDefault: false,
        canvasCount: 0,
      }),
    );
    expect(qualification.snapshot()).toEqual(
      expect.objectContaining({
        schemaVersion: 'manyforge.viewer.qualification.v1',
        performance: expect.any(Object),
      }),
    );
    qualification.reset();
    qualification.resetObservationFlow();
    recordViewerObservationReceived({
      ownerEpoch: 'owner.1',
      sourceEpoch: 'source.1',
      sourceSequence: 7,
      receivedAtComposerMonotonicUs: 123,
    }, false);
    recordViewerObservationApplied({
      ownerEpoch: 'owner.1',
      sourceEpoch: 'source.1',
      sourceSequence: 7,
      receivedAtComposerMonotonicUs: 123,
    });
    expect(qualification.observationFlow()).toEqual(expect.objectContaining({
      receivedFrames: 1,
      appliedFrames: 1,
      replacedBeforeApply: 0,
      maxUnappliedFrames: 1,
      lastApplied: expect.objectContaining({ sourceSequence: 7 }),
    }));
    disconnect();
    expect(fakeWindow[VIEWER_QUALIFICATION_GLOBAL]).toBeUndefined();

    vi.stubGlobal(
      'window',
      {
        location: {
          href: `http://composer.test/?${VIEWER_QUALIFICATION_QUERY_KEY}=1&${VIEWER_QUALIFICATION_QUERY_KEY}=1`,
        },
        addEventListener: vi.fn(),
        removeEventListener: vi.fn(),
      },
    );
    const noDuplicateOptIn = connectViewerQualification();
    expect((window as unknown as Record<string, unknown>)[
      VIEWER_QUALIFICATION_GLOBAL
    ]).toBeUndefined();
    noDuplicateOptIn();
  });
});
