import { describe, expect, it } from 'vitest';
import { balancedOptionsPerRow } from './useCardPanelLayout';

describe('balancedOptionsPerRow', () => {
  // Options 100 px wide with a 4 px gap: n options need 104n - 4 px.
  const perRow = (count: number, inner: number) => balancedOptionsPerRow(count, 100, inner);

  it('splits five options 3 + 2, never 4 + 1', () => {
    expect(perRow(5, 420)).toBe(3);
    expect(perRow(5, 310)).toBe(3);
    expect(perRow(5, 300)).toBe(2);
    expect(perRow(5, 150)).toBe(1);
  });

  it('keeps six options in even rows, never 4 + 2 or 5 + 1', () => {
    expect(perRow(6, 515)).toBe(3);
    expect(perRow(6, 300)).toBe(2);
  });

  it('allows a last row one short', () => {
    expect(perRow(7, 420)).toBe(4);
  });
});
