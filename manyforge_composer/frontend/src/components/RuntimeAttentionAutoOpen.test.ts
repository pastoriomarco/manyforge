import { describe, expect, it } from 'vitest';

import {
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
});
