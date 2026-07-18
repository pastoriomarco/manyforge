import { afterEach, describe, expect, it } from 'vitest';

import { viewerProtocolFixture as fixture } from '../types/viewerProtocol.fixture.generated';
import type {
  ViewerDiagnosticsV1,
  ViewerFrameV1,
  ViewerManifestV1,
} from '../types/viewerProtocol.generated';
import {
  __viewerStoreTest,
  getAuthoritativeViewerState,
  useSceneStateStore,
} from './sceneStateStore';
import {
  publishViewerFrame,
  publishViewerManifest,
  resetViewerFrameStore,
} from './viewerFrameStore';

describe('viewer v1 preservation fixture', () => {
  afterEach(() => {
    resetViewerFrameStore();
    useSceneStateStore.setState({
      manifest: null,
      serverDiagnostics: null,
      connectionState: 'disconnected',
      frameAvailable: false,
    });
  });
  it('keeps collapsed demand passive unless another authority needs frames', () => {
    expect(__viewerStoreTest.viewerVisibility(true, true, 0, 1)).toBe('collapsed');
    expect(__viewerStoreTest.viewerVisibility(true, true, 1, 1)).toBe('focused');
    expect(__viewerStoreTest.viewerVisibility(true, false, 1, 1)).toBe('visible');
    expect(__viewerStoreTest.viewerVisibility(false, true, 1, 1)).toBe('hidden');
  });

  it('rejects callbacks from a replaced viewer socket generation', () => {
    const oldSocket = {};
    const currentSocket = {};

    expect(__viewerStoreTest.ownsViewerConnection(
      currentSocket,
      currentSocket,
      4,
      4,
    )).toBe(true);
    expect(__viewerStoreTest.ownsViewerConnection(oldSocket, currentSocket, 4, 4)).toBe(false);
    expect(__viewerStoreTest.ownsViewerConnection(currentSocket, currentSocket, 3, 4)).toBe(false);
  });

  it('materializes the generated ordered generation exactly', () => {
    const materialized = __viewerStoreTest.materializeViewerState(
      fixture.manifest as ViewerManifestV1,
      fixture.frame as ViewerFrameV1,
    );

    // JSON round-tripping mirrors the network/store boundary and intentionally
    // removes optional properties materialized as undefined.
    expect(JSON.parse(JSON.stringify(materialized))).toEqual(fixture.expected_legacy);
  });

  it('rejects a frame whose ordered entity block is incomplete', () => {
    const malformed = {
      ...fixture.frame,
      entity_transforms: fixture.frame.entity_transforms.slice(1),
    } as ViewerFrameV1;

    expect(() => __viewerStoreTest.materializeViewerState(
      fixture.manifest as ViewerManifestV1,
      malformed,
    )).toThrow('entity transform count');
  });

  it('stores diagnostics without changing frame freshness or drop accounting', () => {
    useSceneStateStore.setState({
      connectionState: 'connected',
      frameAvailable: true,
      droppedFrames: 7,
      lastUpdatedAtMs: 1234,
      serverDiagnostics: null,
    });
    const diagnostics: ViewerDiagnosticsV1 = {
      type: 'diagnostics',
      schema_version: 'manyforge.viewer.diagnostics.v1',
      producer_epoch: 'producer-1',
      timestamp_ns: 5678,
      producer_healthy: true,
      publisher_running: true,
      worker_alive: true,
      worker_restarts: 2,
    };

    __viewerStoreTest.applyViewerServerDiagnostics(diagnostics);

    const state = useSceneStateStore.getState();
    expect(state.serverDiagnostics).toEqual(diagnostics);
    expect(state.connectionState).toBe('connected');
    expect(state.frameAvailable).toBe(true);
    expect(state.droppedFrames).toBe(7);
    expect(state.lastUpdatedAtMs).toBe(1234);
  });

  it('marks retained visual evidence stale when producer health fails', () => {
    publishViewerManifest(fixture.manifest as ViewerManifestV1);
    publishViewerFrame(fixture.frame as ViewerFrameV1);
    useSceneStateStore.setState({
      connectionState: 'connected',
      frameAvailable: true,
      serverDiagnostics: null,
    });
    const diagnostics: ViewerDiagnosticsV1 = {
      type: 'diagnostics',
      schema_version: 'manyforge.viewer.diagnostics.v1',
      producer_epoch: 'producer-1',
      timestamp_ns: Number.MAX_SAFE_INTEGER,
      producer_healthy: false,
      producer_health_reason: 'worker_not_running',
      publisher_running: true,
      worker_alive: false,
    };

    __viewerStoreTest.applyViewerServerDiagnostics(diagnostics);

    const state = useSceneStateStore.getState();
    expect(state.connectionState).toBe('stale');
    expect(state.frameAvailable).toBe(true);
    expect(state.serverDiagnostics?.producer_health_reason).toBe('worker_not_running');
    expect(getAuthoritativeViewerState()).toBeNull();
  });

  it('keeps unchanged frames authoritative while transport messages remain live', () => {
    expect(__viewerStoreTest.transportLivenessExpired(1_000, 16_000, 16_000)).toBe(false);
    expect(__viewerStoreTest.transportLivenessExpired(1_000, 17_001, 16_000)).toBe(true);
    expect(__viewerStoreTest.transportLivenessExpired(null, 99_000, 16_000)).toBe(false);
  });
});
