import { act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import { HISTORY_FOLLOW_INTERVAL_MS, useHistoryFollow } from './ExecutionHistoryPanel';

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

// Minimal React DOM host (see state/workbenchNotices.brief.test.tsx).
function stubDom() {
  const node = (tag: string): Record<string, unknown> => ({
    nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
    setAttribute: () => undefined, removeAttribute: () => undefined,
  });
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {}, hidden: false,
    addEventListener: () => undefined, removeEventListener: () => undefined,
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = {
    document, HTMLIFrameElement: class {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout,
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return { host: node('div'), document };
}

it('reads now, then 2 s after each read ends, never two at once, and stops when inactive', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  const { host, document } = stubDom();
  let finish = () => {};
  const read = vi.fn(() => new Promise<void>((resolve) => { finish = resolve; }));
  function Probe({ active }: { active: boolean }) {
    useHistoryFollow(active, read);
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (active: boolean) => act(() => { root.render(<Probe active={active} />); });
  const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });

  await render(true);
  expect(read).toHaveBeenCalledTimes(1);
  // A slow read is never overlapped.
  await advance(10 * HISTORY_FOLLOW_INTERVAL_MS);
  expect(read).toHaveBeenCalledTimes(1);
  await act(async () => { finish(); });
  await advance(HISTORY_FOLLOW_INTERVAL_MS - 1);
  expect(read).toHaveBeenCalledTimes(1);
  await advance(1);
  expect(read).toHaveBeenCalledTimes(2);
  await act(async () => { finish(); });
  // A hidden browser page is not read.
  document.hidden = true;
  await advance(3 * HISTORY_FOLLOW_INTERVAL_MS);
  expect(read).toHaveBeenCalledTimes(2);
  document.hidden = false;
  await advance(HISTORY_FOLLOW_INTERVAL_MS);
  expect(read).toHaveBeenCalledTimes(3);
  await act(async () => { finish(); });
  await render(false);
  await advance(5 * HISTORY_FOLLOW_INTERVAL_MS);
  expect(read).toHaveBeenCalledTimes(3);
  await act(async () => { root.unmount(); });
});
