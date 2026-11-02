import { describe, expect, it } from 'vitest';
import {
  consumeLatestFrame,
  emptyLatestFrameSlot,
  invalidateLatestFrame,
  ownsConnectionGeneration,
  publishLatestFrame,
} from './latestFrameSlot';

describe('protocol-independent latest frame slot', () => {
  it('keeps only the newest sequence and consumes it once', () => {
    let slot = emptyLatestFrameSlot<{ value: number }>();
    slot = publishLatestFrame(slot, { streamIdentity: 'owner.1/1', sequence: 1n, frame: { value: 1 } });
    slot = publishLatestFrame(slot, { streamIdentity: 'owner.1/1', sequence: 3n, frame: { value: 3 } });
    slot = publishLatestFrame(slot, { streamIdentity: 'owner.1/1', sequence: 2n, frame: { value: 2 } });
    const first = consumeLatestFrame(slot);
    expect(first.changed).toBe(true);
    expect(first.frame).toEqual({ value: 3 });
    expect(consumeLatestFrame(first.state).changed).toBe(false);
  });

  it('accepts a fresh epoch and represents clear without a wire flag', () => {
    let slot = publishLatestFrame(emptyLatestFrameSlot<string>(), {
      streamIdentity: 'owner.1/1', sequence: 50n, frame: 'old',
    });
    slot = publishLatestFrame(slot, {
      streamIdentity: 'owner.2/1', sequence: 1n, frame: 'fresh',
    });
    expect(consumeLatestFrame(slot).frame).toBe('fresh');
    slot = publishLatestFrame(slot, {
      streamIdentity: 'owner.2/1', sequence: 2n, frame: null,
    });
    expect(consumeLatestFrame(slot).frame).toBeNull();
  });

  it('invalidates identity and rejects stale connection callbacks', () => {
    const invalid = invalidateLatestFrame(publishLatestFrame(
      emptyLatestFrameSlot<string>(),
      { streamIdentity: 'owner.1/1', sequence: 1n, frame: 'frame' },
    ));
    expect(invalid.streamIdentity).toBeNull();
    expect(invalid.frame).toBeNull();
    const current = {};
    expect(ownsConnectionGeneration(current, current, 4, 4)).toBe(true);
    expect(ownsConnectionGeneration({}, current, 4, 4)).toBe(false);
    expect(ownsConnectionGeneration(current, current, 3, 4)).toBe(false);
  });
});
