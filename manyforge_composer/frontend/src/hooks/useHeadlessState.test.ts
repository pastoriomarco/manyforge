import { StrictMode, act, createElement } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { ComposerHeadlessState } from '../types/headless';
import {
  HEADLESS_SHELL_PUBLICATION_MS,
  HeadlessStateSource,
  admitHeadlessState,
  useHeadlessStateSource,
} from './useHeadlessState';

function state(
  runtimeEpoch: string,
  runtimeGeneration: number,
  alarmEpoch = 'alarms.epoch.1',
  alarmGeneration = 3,
): ComposerHeadlessState {
  return {
    schemaVersion: 'manyforge.composer_headless_state.v1',
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
      stateRevision: alarmGeneration,
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
  it('accepts monotonic generations and replacement epochs', () => {
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

  it('rejects regressions and same-generation conflicts', () => {
    const current = state('runtime.epoch.1', 2);
    expect(admitHeadlessState(current, state('runtime.epoch.1', 1)).reason)
      .toContain('regressed');
    const conflict = state('runtime.epoch.1', 2);
    conflict.runtime.complete = false;
    expect(admitHeadlessState(current, conflict).reason).toContain('conflicted');
    expect(admitHeadlessState(
      state('runtime.epoch.1', 2, 'alarms.epoch.1', 3),
      state('runtime.epoch.1', 3, 'alarms.epoch.1', 2),
    ).reason).toContain('alarm projection generation regressed');
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
  it('keeps one socket and coalesces a 100-frame burst by delivery lane', () => {
    vi.useFakeTimers();
    const animationFrames: FrameRequestCallback[] = [];
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      onmessage: ((message: MessageEvent) => void) | null = null;
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;

      constructor(url: string) {
        this.url = url;
        FakeWebSocket.instances.push(this);
      }

      close(): void {}

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
    const socket = FakeWebSocket.instances[0]!;
    for (let generation = 0; generation < 100; generation += 1) {
      socket.sendFrame(wireState(generation));
    }
    expect(animationFrames).toHaveLength(1);
    expect(viewerNotifications).toBe(0);
    expect(shellNotifications).toBe(0);

    animationFrames[0]!(16);
    expect(viewerNotifications).toBe(1);
    expect(source.getViewerSnapshot().snapshot?.runtime.projectionGeneration).toBe(99);
    vi.advanceTimersByTime(HEADLESS_SHELL_PUBLICATION_MS - 1);
    expect(shellNotifications).toBe(0);
    vi.advanceTimersByTime(1);
    expect(shellNotifications).toBe(1);
    expect(source.getShellSnapshot().snapshot?.runtime.projectionGeneration).toBe(99);
    expect(FakeWebSocket.instances).toHaveLength(1);
    source.dispose();
  });
});

describe('useHeadlessStateSource lifecycle', () => {
  it('keeps one live socket through the StrictMode probe and none after unmount', async () => {
    vi.useFakeTimers();
    class FakeWebSocket {
      static instances: FakeWebSocket[] = [];
      readonly url: string;
      active = true;
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
