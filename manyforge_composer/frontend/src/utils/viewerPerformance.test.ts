import { afterEach, describe, expect, it } from 'vitest';
import {
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
});
