import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  VIEWER_QUALIFICATION_GLOBAL,
  VIEWER_QUALIFICATION_QUERY_KEY,
  connectViewerQualification,
  getAutomaticViewerRateHz,
  getViewerRatePreferenceHz,
  isSoftwareRenderer,
  recordViewerModelBuild,
  recordViewerReactCommit,
  recordViewerRenderFrame,
  recordViewerRenderWork,
  selectViewerPresentationProfile,
  setViewerRatePreferenceHz,
  startViewerPerformanceMonitoring,
  stopViewerPerformanceMonitoring,
  subscribeViewerRatePreference,
  takeViewerPerformanceWindow,
} from './viewerPerformance';

describe('viewer presentation and performance', () => {
  afterEach(() => {
    stopViewerPerformanceMonitoring();
    setViewerRatePreferenceHz(null);
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

  it('retains the bounded browser-local cadence preference without claiming it is applied', () => {
    let notifications = 0;
    const unsubscribe = subscribeViewerRatePreference(() => {
      notifications += 1;
    });

    expect(getAutomaticViewerRateHz()).toBe(15);
    expect(getViewerRatePreferenceHz()).toBeNull();
    setViewerRatePreferenceHz(10);
    expect(getViewerRatePreferenceHz()).toBe(10);
    expect(notifications).toBe(1);
    expect(() => setViewerRatePreferenceHz(31)).toThrow(RangeError);

    unsubscribe();
    setViewerRatePreferenceHz(null);
    expect(getViewerRatePreferenceHz()).toBeNull();
    expect(notifications).toBe(1);
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
