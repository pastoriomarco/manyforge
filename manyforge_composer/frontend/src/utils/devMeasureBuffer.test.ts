import { expect, it, vi } from 'vitest';
import { boundDevMeasureBuffer, DEV_MEASURE_CLEAR_INTERVAL_MS } from './devMeasureBuffer';

it('periodically clears the user-timing measures React development builds accumulate', () => {
  const timeline = { clearMeasures: vi.fn() };
  let tick: (() => void) | null = null;
  const every = vi.fn((callback: () => void) => { tick = callback; });
  boundDevMeasureBuffer(timeline, every);
  expect(every).toHaveBeenCalledWith(expect.any(Function), DEV_MEASURE_CLEAR_INTERVAL_MS);
  expect(timeline.clearMeasures).not.toHaveBeenCalled();
  tick!();
  tick!();
  expect(timeline.clearMeasures).toHaveBeenCalledTimes(2);
  expect(timeline.clearMeasures).toHaveBeenCalledWith();
});
