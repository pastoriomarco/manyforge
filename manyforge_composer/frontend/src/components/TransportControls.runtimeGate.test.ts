import { describe, expect, it } from 'vitest';

import { runtimeAllowsMotion } from './TransportControls';

describe('TransportControls robot lifecycle admission', () => {
  it('disables motion whenever runtime admission is explicitly closed', () => {
    expect(runtimeAllowsMotion({ canExecute: false })).toBe(false);
  });

  it('fails closed until an authoritative runtime exists', () => {
    expect(runtimeAllowsMotion(null)).toBe(false);
  });

  it('allows motion after runtime admission opens', () => {
    expect(runtimeAllowsMotion({ canExecute: true })).toBe(true);
  });
});
