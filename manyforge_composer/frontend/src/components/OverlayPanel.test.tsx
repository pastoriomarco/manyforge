import { act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import { reactPropsHost } from '../test/reactPropsHost';
import { OverlayPanel } from './OverlayPanel';

afterEach(() => vi.unstubAllGlobals());

it('skips only closed-to-closed updates and uses the latest props when opened', async () => {
  const compare = (OverlayPanel as unknown as { compare: (a: { open: boolean }, b: { open: boolean }) => boolean }).compare;
  expect(compare({ open: false }, { open: false })).toBe(true);
  expect(compare({ open: false }, { open: true })).toBe(false);
  expect(compare({ open: true }, { open: false })).toBe(false);
  expect(compare({ open: true }, { open: true })).toBe(false);

  const { host, find } = reactPropsHost();
  (window as unknown as { requestAnimationFrame: (callback: () => void) => number }).requestAnimationFrame = () => 1;
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const oldClose = vi.fn();
  const latestClose = vi.fn();
  try {
    await act(() => root.render(<OverlayPanel open={false} title="Old" titleId="old" onClose={oldClose}>old</OverlayPanel>));
    await act(() => root.render(<OverlayPanel open={false} title="New" titleId="new" onClose={latestClose}>new</OverlayPanel>));
    await act(() => root.render(<OverlayPanel open title="New" titleId="new" onClose={latestClose}>new</OverlayPanel>));
    const button = find((props) => props['aria-label'] === 'Close New');
    await act(() => button.onClick?.());
    expect(latestClose).toHaveBeenCalledOnce();
    expect(oldClose).not.toHaveBeenCalled();
  } finally {
    await act(() => root.unmount());
  }
});
