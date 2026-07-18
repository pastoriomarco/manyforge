import { describe, expect, it, vi } from 'vitest';

import { canvasToBlobWithTimeout, flipRgbaRowsInPlace } from './snapshotCapture';

describe('snapshot capture utilities', () => {
  it('flips WebGL bottom-up rows without changing pixels within a row', () => {
    const pixels = new Uint8Array([
      1, 2, 3, 4, 5, 6, 7, 8,
      9, 10, 11, 12, 13, 14, 15, 16,
      17, 18, 19, 20, 21, 22, 23, 24,
    ]);
    flipRgbaRowsInPlace(pixels, 2, 3);
    expect(Array.from(pixels)).toEqual([
      17, 18, 19, 20, 21, 22, 23, 24,
      9, 10, 11, 12, 13, 14, 15, 16,
      1, 2, 3, 4, 5, 6, 7, 8,
    ]);
  });

  it('rejects a missing encoded blob', async () => {
    vi.useFakeTimers();
    const canvas = {
      toBlob: (callback: BlobCallback) => callback(null),
    } as unknown as HTMLCanvasElement;
    await expect(canvasToBlobWithTimeout(canvas, 'image/png', 0.9, 100)).rejects.toThrow(
      'returned no image',
    );
    vi.useRealTimers();
  });

  it('bounds a renderer that never completes encoding', async () => {
    vi.useFakeTimers();
    const canvas = {
      toBlob: () => undefined,
    } as unknown as HTMLCanvasElement;
    const result = canvasToBlobWithTimeout(canvas, 'image/png', 0.9, 100);
    const assertion = expect(result).rejects.toThrow('timed out');
    await vi.advanceTimersByTimeAsync(101);
    await assertion;
    vi.useRealTimers();
  });
});
