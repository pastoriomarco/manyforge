import { describe, expect, it } from 'vitest';

import {
  retainActiveRuntimeAttention,
  shouldCloseAutomaticRuntimeAttention,
  unpresentedRuntimeAttention,
  type RuntimeAttentionItem,
} from './RuntimeAttentionAutoOpen';

const backend: RuntimeAttentionItem = {
  identity: 'backend:activation-1',
};
const composer: RuntimeAttentionItem = {
  identity: 'composer:error-1',
};

describe('runtime attention automatic presentation', () => {
  it('opens only for attention identities not already presented', () => {
    expect(unpresentedRuntimeAttention(new Set(), [backend, composer])).toEqual([
      backend,
      composer,
    ]);
    expect(unpresentedRuntimeAttention(
      new Set([backend.identity]),
      [backend, composer],
    )).toEqual([composer]);
  });

  it('does not turn ordinary warning state into an attention item', () => {
    expect(unpresentedRuntimeAttention(new Set(), [])).toEqual([]);
  });

  it('bounds presented identities to active conditions and permits reactivation', () => {
    const presented = new Set([backend.identity, composer.identity]);
    retainActiveRuntimeAttention(presented, [backend]);
    expect([...presented]).toEqual([backend.identity]);
    expect(unpresentedRuntimeAttention(presented, [backend, composer])).toEqual([
      composer,
    ]);
  });

  it('closes an automatically opened empty rail but preserves manual panels', () => {
    expect(shouldCloseAutomaticRuntimeAttention(true, 'runtime', 0)).toBe(true);
    expect(shouldCloseAutomaticRuntimeAttention(false, 'runtime', 0)).toBe(false);
    expect(shouldCloseAutomaticRuntimeAttention(true, 'runtime', 1)).toBe(false);
    expect(shouldCloseAutomaticRuntimeAttention(true, 'assistant', 0)).toBe(false);
  });
});
