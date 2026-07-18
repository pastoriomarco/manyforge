import { afterEach, describe, expect, it } from 'vitest';

import {
  recordViewerFrameGap,
  recordViewerMaterialization,
  recordViewerReactCommit,
  recordViewerRenderFrame,
  recordViewerRenderWork,
  recordViewerEffectiveDpr,
  recordViewerTopologyRebuild,
  recordViewerTransformApply,
  recordViewerWebSocketMessage,
  startViewerPerformanceMonitoring,
  stopViewerPerformanceMonitoring,
  takeViewerPerformanceWindow,
  publishViewerRendererProfile,
  getAutomaticViewerRateHz,
  getViewerRatePreferenceHz,
  selectViewerPresentationProfile,
  setViewerRatePreferenceHz,
  subscribeViewerRendererProfile,
} from './viewerPerformance';

describe('viewer performance windows', () => {
  afterEach(() => {
    stopViewerPerformanceMonitoring();
    setViewerRatePreferenceHz(null);
  });

  it('reports bounded work that actually occurred and resets the window', () => {
    startViewerPerformanceMonitoring();
    const message = '{"type":"frame","label":"caf\u00e9"}';
    recordViewerWebSocketMessage(message, 'frame');
    recordViewerFrameGap(3);
    recordViewerMaterialization(2.5);
    recordViewerReactCommit(1.25);
    recordViewerRenderFrame(100);
    recordViewerRenderFrame(116);
    recordViewerRenderWork(17, 12_345, 640_000);
    recordViewerTopologyRebuild(2);
    recordViewerTransformApply(7);

    const first = takeViewerPerformanceWindow('focused');
    expect(first.websocket_messages).toBe(1);
    expect(first.websocket_bytes).toBe(new TextEncoder().encode(message).byteLength);
    expect(first.frame_messages).toBe(1);
    expect(first.dropped_frame_gaps).toBe(3);
    expect(first.materialization_total_ms).toBe(2.5);
    expect(first.react_commit_total_ms).toBe(1.25);
    expect(first.rendered_frames).toBe(2);
    expect(first.render_interval_p95_ms).toBe(16);
    expect(first.draw_calls_max).toBe(17);
    expect(first.triangles_max).toBe(12_345);
    expect(first.canvas_pixels_max).toBe(640_000);
    expect(first.topology_rebuilds).toBe(2);
    expect(first.transform_applies).toBe(7);

    const second = takeViewerPerformanceWindow('visible');
    expect(second.websocket_messages).toBe(0);
    expect(second.rendered_frames).toBe(0);
    expect(second.render_interval_p95_ms).toBeNull();
  });

  it('preserves the automatic cadence without permitting incomplete low-resolution frames', () => {
    const softwarePresentation = selectViewerPresentationProfile(true, 2);
    expect(softwarePresentation).toEqual({
      effectiveDpr: 1,
      antialias: true,
      preserveDrawingBuffer: true,
    });
    const software = publishViewerRendererProfile({
      vendor: 'Mesa',
      renderer: 'llvmpipe (LLVM 20.1.2)',
      software: true,
      ...softwarePresentation,
    });
    expect(software.requestedRateHz).toBe(15);
    recordViewerEffectiveDpr(0.8);
    expect(takeViewerPerformanceWindow('focused').effective_dpr).toBe(0.8);

    const hardwarePresentation = selectViewerPresentationProfile(false, 2);
    expect(hardwarePresentation).toEqual({
      effectiveDpr: 1.5,
      antialias: true,
      preserveDrawingBuffer: false,
    });
    const hardware = publishViewerRendererProfile({
      vendor: 'NVIDIA',
      renderer: 'NVIDIA Tegra',
      software: false,
      ...hardwarePresentation,
    });
    expect(hardware.requestedRateHz).toBe(15);
  });

  it('supports an explicit bounded client rate without changing renderer selection', () => {
    let profileNotifications = 0;
    const unsubscribe = subscribeViewerRendererProfile(() => {
      profileNotifications += 1;
    });
    const softwarePresentation = selectViewerPresentationProfile(true, 2);
    publishViewerRendererProfile({
      vendor: 'Mesa',
      renderer: 'llvmpipe',
      software: true,
      ...softwarePresentation,
    });
    expect(getAutomaticViewerRateHz()).toBe(15);

    setViewerRatePreferenceHz(10);
    expect(getViewerRatePreferenceHz()).toBe(10);
    expect(profileNotifications).toBe(2);
    expect(publishViewerRendererProfile({
      vendor: 'Mesa',
      renderer: 'llvmpipe',
      software: true,
      ...softwarePresentation,
    }).requestedRateHz).toBe(10);

    setViewerRatePreferenceHz(null);
    expect(getViewerRatePreferenceHz()).toBeNull();
    expect(getAutomaticViewerRateHz()).toBe(15);
    expect(() => setViewerRatePreferenceHz(31)).toThrow(RangeError);
    unsubscribe();
  });
});
