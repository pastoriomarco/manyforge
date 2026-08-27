import { StrictMode, act, createElement } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { ComposerHeadlessState } from '../types/headless';
import {
  HEADLESS_SHELL_PUBLICATION_MS,
  HeadlessStateSource,
  admitHeadlessState,
  getHeadlessStateQualificationSnapshot,
  sameHeadlessProjection,
  sameViewerProjection,
  useHeadlessStateSource,
} from './useHeadlessState';

function state(
  runtimeEpoch: string,
  runtimeGeneration: number,
  alarmEpoch = 'alarms.epoch.1',
  alarmGeneration = 3,
  alarmStateRevision = alarmGeneration,
): ComposerHeadlessState {
  return {
    schemaVersion: 'manyforge.composer_headless_state.v1',
    complete: true,
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 1 },
    programCycle: {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 1,
      state: 'idle_after_reset',
      programRunId: null,
      cycleClaimGeneration: null,
      terminalOrSettlementReference: null,
      runtimeControlRevision: null,
      recoveryOffer: null,
    },
    draftIdentity: { draftId: 'draft-1', draftRevision: 1 },
    receiverEpoch: runtimeEpoch,
    snapshotSequence: Math.max(1, runtimeGeneration),
    runtime: {
      schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
      complete: true,
      projectionGeneration: runtimeGeneration,
      projectionOwnerEpoch: runtimeEpoch,
      projectionOwnerId: 'headless.owner',
      reasons: [],
      unavailableSections: [],
      sections: {
        manager: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'manager.epoch.1',
          sourceOwnerId: 'manager.owner',
          receiverAgeNs: 1,
        },
        logicalScene: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'scene.epoch.1',
          sourceOwnerId: 'scene.owner',
          receiverAgeNs: 1,
        },
        sceneRuntime: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'runtime.epoch.1',
          sourceOwnerId: 'runtime.owner',
          receiverAgeNs: 1,
        },
      },
    },
    alarms: {
      schemaVersion: 'manyforge.operator_alarm_snapshot.v2',
      ownerId: 'alarm.owner',
      ownerEpoch: alarmEpoch,
      generation: alarmGeneration,
      stateRevision: alarmStateRevision,
      reconciled: true,
      healthState: 'healthy',
      liveCount: 0,
      unacknowledgedCount: 0,
      activeCount: 0,
      activeAcknowledgedCount: 0,
      motionAttentionCount: 0,
      unacknowledgedMembership: [],
      alarms: [],
      truncated: false,
    },
  };
}

describe('Headless owner-frame admission', () => {
  it('treats transport-only sequence updates as the same UI projection', () => {
    const current = state('receiver.epoch.1', 2);
    const next = structuredClone(current);
    next.snapshotSequence += 1;
    expect(sameHeadlessProjection(current, next)).toBe(true);
    next.alarms.stateRevision += 1;
    expect(sameHeadlessProjection(current, next)).toBe(true);
    next.alarms.generation += 1;
    expect(sameHeadlessProjection(current, next)).toBe(false);
    next.alarms.generation -= 1;
    next.draftIdentity.draftRevision += 1;
    expect(sameHeadlessProjection(current, next)).toBe(false);
  });

  it('publishes owner runtime-control revisions without invalidating the viewer', () => {
    const current = state('receiver.epoch.1', 2);
    const next = structuredClone(current);
    next.snapshotSequence += 1;
    next.programCycle.runtimeControlRevision = 2;
    expect(sameHeadlessProjection(current, next)).toBe(false);
    expect(sameViewerProjection(current, next)).toBe(true);
  });

  it('does not publish Manager observation heartbeats as UI or viewer changes', () => {
    const current = state('receiver.epoch.1', 2);
    const heartbeat = structuredClone(current);
    heartbeat.snapshotSequence += 1;
    heartbeat.runtime.projectionGeneration += 1;
    heartbeat.runtime.sections.manager.sourceGeneration =
      (heartbeat.runtime.sections.manager.sourceGeneration ?? 0) + 1;
    heartbeat.runtime.sections.manager.receiverAgeNs =
      (heartbeat.runtime.sections.manager.receiverAgeNs ?? 0) + 1;
    expect(sameHeadlessProjection(current, heartbeat)).toBe(true);
    expect(sameViewerProjection(current, heartbeat)).toBe(true);

    heartbeat.runtime.sections.logicalScene.sourceGeneration =
      (heartbeat.runtime.sections.logicalScene.sourceGeneration ?? 0) + 1;
    expect(sameHeadlessProjection(current, heartbeat)).toBe(false);
    expect(sameViewerProjection(current, heartbeat)).toBe(false);
  });

  it('accepts forward stream frames and a replacement receiver epoch', () => {
    expect(admitHeadlessState(null, state('runtime.epoch.1', 2)).accepted).toBe(true);
    expect(admitHeadlessState(
      state('runtime.epoch.1', 2),
      state('runtime.epoch.1', 3),
    ).accepted).toBe(true);
    expect(admitHeadlessState(
      state('runtime.epoch.1', 20, 'alarms.epoch.1', 30),
      state('runtime.epoch.2', 0, 'alarms.epoch.2', 0),
    ).accepted).toBe(true);
  });

  it('does not duplicate nested owner-generation adjudication in the browser', () => {
    const current = state('receiver.epoch.1', 2, 'alarms.epoch.1', 6, 290);
    const candidate = state('receiver.epoch.1', 1, 'alarms.epoch.1', 5, 291);
    candidate.snapshotSequence = current.snapshotSequence + 1;
    expect(admitHeadlessState(current, candidate)).toEqual({
      accepted: true,
      reason: null,
    });
  });

  it('requires a strictly forward sequence within an epoch but accepts a fresh barrier',
    () => {
      const current = state('runtime.epoch.1', 2);
      expect(admitHeadlessState(current, state('runtime.epoch.1', 2)).accepted)
        .toBe(false);
      expect(admitHeadlessState(current, state('runtime.epoch.2', 99)).accepted)
        .toBe(true);
    });
});

function wireState(generation: number): Record<string, unknown> {
  const section = {
    available: true,
    freshness: 'fresh',
    known_gaps: [],
    payload: {},
    source_generation: generation,
    source_owner_epoch: 'source.epoch.1',
    source_owner_id: 'source.owner',
    receiver_age_ns: 1,
  };
  return {
    schema_version: 'manyforge.composer_headless_state.v1',
    authoring_admission: { admission_generation: 1, authoring_allowed: true },
    program_cycle: {
      program_runner_owner_epoch: 'runner.epoch.1',
      cycle_generation: 1,
      state: 'idle_after_reset',
      program_run_id: null,
      cycle_claim_generation: null,
      terminal_or_settlement_reference: null,
      runtime_control_revision: null,
      recovery_offer: null,
    },
    draft_identity: { draft_id: 'draft-1', draft_revision: 1 },
    receiver_epoch: 'receiver.epoch.1',
    snapshot_sequence: Math.max(1, generation + 1),
    complete: true,
    runtime: {
      schema_version: 'manyforge.headless_runtime_snapshot.v1',
      complete: true,
      projection_generation: generation,
      projection_owner_epoch: 'runtime.epoch.1',
      projection_owner_id: 'headless.owner',
      reasons: [],
      unavailable_sections: [],
      sections: {
        manager: section,
        logical_scene: section,
        scene_runtime: section,
      },
    },
    alarms: {
      schema_version: 'manyforge.operator_alarm_snapshot.v2',
      owner_id: 'alarm.owner',
      owner_epoch: 'alarms.epoch.1',
      generation,
      state_revision: generation,
      reconciled: true,
      health: { state: 'healthy' },
      live_count: 0,
      unacknowledged_count: 0,
      active_count: 0,
      active_acknowledged_count: 0,
      motion_attention_count: 0,
      unacknowledged_membership: [],
      alarms: [],
      truncated: false,
    },
  };
}

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

describe('HeadlessStateSource delivery scheduling', () => {
  it('uses one read-only state endpoint without a client authority identity', () => {
    vi.useFakeTimers();
    const urls: string[] = [];
    class FakeWebSocket {
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) { urls.push(url); }
      close(): void {}
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(), cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    source.reconnect();
    expect(urls).toHaveLength(2);
    expect(new URL(urls[0]!).pathname).toBe('/ws/state');
    expect(new URL(urls[0]!).search).toBe('');
    expect(urls[1]).toBe(urls[0]);
    source.dispose();
  });

  it('publishes admission changes to the shell without waking the viewer', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static instance: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) {
        void url;
        FakeWebSocket.instance = this;
      }
      close(): void {}
      send(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);
    const source = new HeadlessStateSource();
    source.setEnabled(true);
    const first = wireState(1);
    const second = structuredClone(first);
    second.snapshot_sequence = 3;
    second.authoring_admission = {
      admission_generation: 2, authoring_allowed: true,
    };
    FakeWebSocket.instance.send(first);
    expect(animationFrames).toHaveLength(1);
    animationFrames[0]!(16);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    const viewerBaseline = source.getViewerSnapshot();
    FakeWebSocket.instance.send(second);
    expect(animationFrames).toHaveLength(1);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS);
    expect(source.getViewerSnapshot()).toBe(viewerBaseline);
    expect(source.getShellSnapshot().snapshot?.snapshotSequence).toBe(3);
    expect(source.getShellSnapshot().snapshot?.authoringAdmission.admissionGeneration)
      .toBe(2);
    source.dispose();
  });

  it('advances exact capture identity across visually equivalent heartbeats', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static instance: FakeWebSocket;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      constructor(url: string) { void url; FakeWebSocket.instance = this; }
      close(): void {}
      send(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback); return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    source.setEnabled(true);
    const first = wireState(1);
    FakeWebSocket.instance.send(first);
    animationFrames[0]!(16);
    const presented = source.getViewerSnapshot().snapshot;
    source.markViewerPresented(presented);
    expect(source.getPresentedRuntimeFrame()).toEqual({
      ownerEpoch: 'runtime.epoch.1', generation: 1,
    });

    const heartbeat = structuredClone(first);
    heartbeat.snapshot_sequence = 3;
    const heartbeatRuntime = heartbeat.runtime as Record<string, unknown>;
    heartbeatRuntime.projection_generation = 2;
    const sections = heartbeatRuntime.sections as Record<string, Record<string, unknown>>;
    sections.manager = { ...sections.manager, source_generation: 2 };
    FakeWebSocket.instance.send(heartbeat);
    expect(animationFrames).toHaveLength(1);
    expect(source.getPresentedRuntimeFrame()).toEqual({
      ownerEpoch: 'runtime.epoch.1', generation: 2,
    });

    const sceneChange = structuredClone(heartbeat);
    sceneChange.snapshot_sequence = 4;
    const sceneRuntime = sceneChange.runtime as Record<string, unknown>;
    sceneRuntime.projection_generation = 3;
    const sceneSections = sceneRuntime.sections as Record<
      string, Record<string, unknown>
    >;
    sceneSections.logical_scene = {
      ...sceneSections.logical_scene,
      source_generation: 2,
    };
    FakeWebSocket.instance.send(sceneChange);
    expect(animationFrames).toHaveLength(2);
    expect(source.getPresentedRuntimeFrame()?.generation).toBe(2);
    animationFrames[1]!(32);
    source.markViewerPresented(source.getViewerSnapshot().snapshot);
    expect(source.getPresentedRuntimeFrame()).toEqual({
      ownerEpoch: 'runtime.epoch.1', generation: 3,
    });
    source.dispose();
  });

  it('keeps one socket and coalesces a 100-frame burst by delivery lane', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      close(): void {}

      open(): void {
        this.onopen?.();
      }

      sendFrame(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: (callback: FrameRequestCallback) => {
        animationFrames.push(callback);
        return animationFrames.length;
      },
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    let viewerNotifications = 0;
    let shellNotifications = 0;
    source.subscribeViewer(() => { viewerNotifications += 1; });
    source.subscribeShell(() => { shellNotifications += 1; });
    source.setEnabled(true);

    expect(FakeWebSocket.instances).toHaveLength(1);
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        sourceCount: 1,
        activeSockets: 0,
      }),
    );
    const socket = FakeWebSocket.instances[0]!;
    socket.open();
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        activeSockets: 1,
        maxActiveSockets: 1,
      }),
    );
    const viewerBaseline = viewerNotifications;
    const shellBaseline = shellNotifications;
    for (let generation = 1; generation <= 100; generation += 1) {
      socket.sendFrame(wireState(generation));
    }
    expect(animationFrames).toHaveLength(1);
    expect(source.getViewerSnapshot().error).toBeNull();
    expect(viewerNotifications).toBe(viewerBaseline);
    expect(shellNotifications).toBe(shellBaseline);

    animationFrames[0]!(16);
    expect(viewerNotifications).toBe(viewerBaseline + 1);
    expect(source.getViewerSnapshot().snapshot?.runtime.projectionGeneration).toBe(100);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS - 1);
    expect(shellNotifications).toBe(shellBaseline);
    vi.advanceTimersByTime(1);
    expect(shellNotifications).toBe(shellBaseline + 1);
    expect(source.getShellSnapshot().snapshot?.runtime.projectionGeneration).toBe(100);
    expect(FakeWebSocket.instances).toHaveLength(1);
    source.dispose();
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);
  });

  it('counts only opened sockets through a failed transport reconnect', () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        void url;
        FakeWebSocket.instances.push(this);
      }

      open(): void {
        this.onopen?.();
      }

      fail(): void {
        this.onerror?.();
        this.onclose?.();
      }

      close(): void {
        this.onclose?.();
      }
    }
    vi.stubGlobal('window', {
      location: { href: 'http://composer.test/' },
      requestAnimationFrame: vi.fn(),
      cancelAnimationFrame: vi.fn(),
    });
    vi.stubGlobal('WebSocket', FakeWebSocket);

    const source = new HeadlessStateSource();
    const initialCreations = getHeadlessStateQualificationSnapshot().socketCreations;
    source.setEnabled(true);
    expect(FakeWebSocket.instances).toHaveLength(1);
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        activeSockets: 0,
        socketCreations: initialCreations + 1,
      }),
    );

    FakeWebSocket.instances[0]!.open();
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(1);
    FakeWebSocket.instances[0]!.fail();
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);

    vi.advanceTimersByTime(1000);
    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);
    FakeWebSocket.instances[1]!.open();
    expect(getHeadlessStateQualificationSnapshot()).toEqual(
      expect.objectContaining({
        activeSockets: 1,
        maxActiveSockets: 1,
      }),
    );
    source.dispose();
    expect(getHeadlessStateQualificationSnapshot().activeSockets).toBe(0);
  });
});

describe('useHeadlessStateSource lifecycle', () => {
  it('keeps one live socket through the StrictMode probe and none after unmount', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      active = true;
      onopen: (() => void) | null = null;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      close(): void {
        this.active = false;
        this.onclose?.();
      }

      open(): void {
        this.onopen?.();
      }

      sendFrame(value: unknown): void {
        this.onmessage?.({ data: JSON.stringify(value) } as MessageEvent);
      }
    }

    type FakeNode = {
      nodeType: number;
      nodeName: string;
      tagName: string;
      namespaceURI: string;
      ownerDocument: FakeDocument;
      style: Record<string, string>;
      addEventListener: () => void;
      removeEventListener: () => void;
      appendChild: () => void;
      insertBefore: () => void;
      removeChild: () => void;
      setAttribute: () => void;
      removeAttribute: () => void;
    };
    type FakeDocument = {
      nodeType: number;
      documentElement: FakeNode | null;
      activeElement: null;
      defaultView: Record<string, unknown>;
      addEventListener: () => void;
      removeEventListener: () => void;
      createElement: (tagName: string) => FakeNode;
    };
    const document = {
      nodeType: 9,
      documentElement: null,
      activeElement: null,
      defaultView: {},
      addEventListener: () => undefined,
      removeEventListener: () => undefined,
    } as FakeDocument;
    const makeNode = (tagName: string): FakeNode => ({
      nodeType: 1,
      nodeName: tagName.toUpperCase(),
      tagName: tagName.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml',
      ownerDocument: document,
      style: {},
      addEventListener: () => undefined,
      removeEventListener: () => undefined,
      appendChild: () => undefined,
      insertBefore: () => undefined,
      removeChild: () => undefined,
      setAttribute: () => undefined,
      removeAttribute: () => undefined,
    });
    document.createElement = makeNode;
    document.documentElement = makeNode('html');
    const window = {
      document,
      location: { href: 'http://composer.test/' },
      HTMLIFrameElement: class {},
      addEventListener: () => undefined,
      removeEventListener: () => undefined,
    };
    document.defaultView = window;
    vi.stubGlobal('window', window);
    vi.stubGlobal('document', document);
    vi.stubGlobal('WebSocket', FakeWebSocket);
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);

    const { createRoot } = await import('react-dom/client');
    const root = createRoot(makeNode('div') as unknown as Element);
    function Harness() {
      useHeadlessStateSource(true);
      return null;
    }

    await act(async () => {
      root.render(createElement(StrictMode, null, createElement(Harness)));
    });

    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(FakeWebSocket.instances[0]?.active).toBe(false);
    expect(FakeWebSocket.instances.filter((socket) => socket.active)).toHaveLength(1);
    FakeWebSocket.instances[1]?.open();
    FakeWebSocket.instances[1]?.sendFrame(wireState(1));
    expect(vi.getTimerCount()).toBe(2);

    await act(async () => root.unmount());
    expect(FakeWebSocket.instances[1]?.active).toBe(false);
    expect(FakeWebSocket.instances.filter((socket) => socket.active)).toHaveLength(0);
    expect(vi.getTimerCount()).toBe(0);

    vi.advanceTimersByTime(10_000);
    expect(FakeWebSocket.instances).toHaveLength(2);
  });
});
