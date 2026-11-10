import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import { reactPropsHost } from '../test/reactPropsHost';
import type { MotionPreviewPath } from '../types/robotControl';
import { MotionPreviewDots, previewDotRuns, previewRunSampleAt, previewSampleColor } from './MotionPreviewDots';

type Sample = MotionPreviewPath['samples'][number];
const sample = (status: Sample['status'], ...dispositions: Sample['contacts'][number]['disposition'][]): Sample => ({
  timeFromStartS: 0, positionsRad: [], status,
  contacts: dispositions.map((disposition, pairIndex) => ({ pairIndex, disposition })),
});

describe('motion preview sample markers', () => {
  it('prioritizes refused, unknown, allowed and clear in that order', () => {
    expect(previewSampleColor(sample('checked'))).toBe('clear');
    expect(previewSampleColor(sample('checked', 'allowed'))).toBe('allowed');
    expect(previewSampleColor(sample('checked', 'allowed', 'refused'))).toBe('refused');
    expect(previewSampleColor(sample('checked', 'refused', 'unknown'))).toBe('refused');
    expect(previewSampleColor(sample('unknown', 'refused'))).toBe('refused');
    expect(previewSampleColor(sample('checked', 'allowed', 'unknown'))).toBe('unknown');
    expect(previewSampleColor(sample('unknown'))).toBe('unknown');
    const clearance = { ...sample('unknown'), clearanceViolations: [{ pairIndex: 0, distanceM: 0.001, requiredClearanceM: 0.002 }] };
    expect(previewSampleColor(clearance)).toBe('clearance');
    expect(previewSampleColor({ ...clearance, contacts: [{ pairIndex: 0, disposition: 'refused' }] })).toBe('refused');
  });
  it('merges only adjacent same-colour samples when the track is too narrow', () => {
    const samples = [sample('checked'), sample('checked'), sample('checked', 'refused'),
      sample('checked', 'refused'), sample('checked')];
    expect(previewDotRuns(samples, 120)).toHaveLength(5);
    expect(previewDotRuns(samples, 20)).toEqual([
      { start: 0, end: 1, color: 'clear' }, { start: 2, end: 3, color: 'refused' },
      { start: 4, end: 4, color: 'clear' },
    ]);
    expect(previewDotRuns(samples, 0)).toHaveLength(5);
  });
  it('maps a merged marker click to the sample under the pointer', () => {
    expect(previewRunSampleAt(4, 8, 100, 40, 100, false)).toBe(4);
    expect(previewRunSampleAt(4, 8, 100, 40, 120, false)).toBe(6);
    expect(previewRunSampleAt(4, 8, 100, 40, 140, false)).toBe(8);
    expect(previewRunSampleAt(4, 8, 100, 40, 0, true)).toBe(6);
  });
  it('labels individual dots and makes a dot jump to its exact sample', async () => {
    const samples = [{ ...sample('checked'), timeFromStartS: 0 },
      { ...sample('checked', 'allowed'), timeFromStartS: 1.25 }];
    const onSelect = vi.fn();
    const markup = renderToStaticMarkup(<MotionPreviewDots samples={samples} selectedIndex={1} onSelect={onSelect} />);
    expect(markup).toContain('Sample 2, 1.25 s, allowed');
    expect(markup).toContain('aria-current="true"');
    const { host, find } = reactPropsHost();
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      await act(() => root.render(<MotionPreviewDots samples={samples} selectedIndex={1} onSelect={onSelect} />));
      await act(() => find((props) => props['aria-label'] === 'Sample 1, 0.00 s, clear').onClick?.());
      expect(onSelect).toHaveBeenCalledWith(0);
    } finally { await act(() => root.unmount()); vi.unstubAllGlobals(); }
  });
});
