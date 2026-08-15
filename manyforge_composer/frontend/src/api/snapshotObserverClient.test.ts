import { describe, expect, it } from 'vitest';

import { parseSnapshotObserverRequest } from './snapshotObserverClient';

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
