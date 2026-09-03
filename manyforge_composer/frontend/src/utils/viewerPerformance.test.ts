import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  VIEWER_QUALIFICATION_GLOBAL,
  VIEWER_QUALIFICATION_QUERY_KEY,
  connectViewerQualification,
  inspectViewerRenderer,
  isSoftwareRenderer,
  recordViewerModelBuild,
  recordViewerObservationApplied,
  recordViewerObservationReceived,
  recordViewerReactCommit,
  recordViewerRenderFrame,
  recordViewerRenderWork,
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
    expect(selectViewerPresentationProfile(true, 2)).toEqual({
      effectiveDpr: 1,
      antialias: true,
      preserveDrawingBuffer: true,
    });
    expect(selectViewerPresentationProfile(false, 2)).toEqual({
      effectiveDpr: 1.5,
      antialias: true,
      preserveDrawingBuffer: false,
    });
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
