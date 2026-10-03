import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  parseSnapshotObserverRequest,
  subscribeSnapshotObserver,
} from './snapshotObserverClient';

function request(): Record<string, unknown> {
  return {
    type: 'snapshot_request',
    requestId: '0123456789abcdef0123456789abcdef',
    width: 640,
    height: 480,
    mime: 'image/jpeg',
    requiredFrame: { ownerEpoch: 'headless.epoch.1', generation: 19 },
    timeoutMs: 2_000,
  };
}

describe('snapshot observer wire', () => {
  it('accepts only the exact bounded owner-frame request', () => {
    expect(parseSnapshotObserverRequest(request()).requiredFrame).toEqual({
      ownerEpoch: 'headless.epoch.1',
      generation: 19,
    });
    for (const mutate of [
      (value: Record<string, unknown>) => { value.extra = true; },
      (value: Record<string, unknown>) => { value.width = 63; },
      (value: Record<string, unknown>) => { value.timeoutMs = 2_001; },
      (value: Record<string, unknown>) => {
        value.requiredFrame = { ownerEpoch: '', generation: 19 };
      },
      (value: Record<string, unknown>) => {
        value.requiredFrame = { ownerEpoch: 'headless.epoch.1', generation: -1 };
      },
    ]) {
      const invalid = request();
      mutate(invalid);
      expect(() => parseSnapshotObserverRequest(invalid)).toThrow();
    }
  });
});

describe('snapshot observer reconnect', () => {
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it('backs off while another workbench holds the single observer binding', () => {
    vi.useFakeTimers();
    const sockets: Array<{ onclose: ((event: { code: number }) => void) | null }> = [];
    class FakeSocket {
      onmessage = null;
      onclose: ((event: { code: number }) => void) | null = null;
      constructor() { sockets.push(this); }
      close() {}
    }
    vi.stubGlobal('WebSocket', FakeSocket);
    vi.stubGlobal('window', { location: { href: 'http://composer.test/' } });
    const unsubscribe = subscribeSnapshotObserver(() => undefined);
    const closeLatest = (code: number) => sockets.at(-1)!.onclose?.({ code });

    const delays: number[] = [];
    for (let attempt = 0; attempt < 6; attempt += 1) {
      const before = sockets.length;
      closeLatest(1013);
      let waited = 0;
      while (sockets.length === before) {
        vi.advanceTimersByTime(1_000);
        waited += 1_000;
      }
      delays.push(waited);
    }
    expect(delays).toEqual([2_000, 4_000, 8_000, 16_000, 30_000, 30_000]);

    // Any other close is an ordinary transport loss: reconnect promptly.
    const before = sockets.length;
    closeLatest(1006);
    vi.advanceTimersByTime(1_000);
    expect(sockets.length).toBe(before + 1);
    unsubscribe();
  });
});
