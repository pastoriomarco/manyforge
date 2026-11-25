import { act, useEffect } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import {
  BRIEF_NOTICE_MS,
  RECENT_NOTICE_LIMIT,
  recordRecentNotice,
  useBriefNotice,
  useRecentNotices,
  type RecentNotice,
  type WorkbenchNotice,
} from './workbenchNotices';

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

// Minimal React DOM host (see workbenchNotices.hold.test.tsx).
function stubDom() {
  const node = (tag: string): Record<string, unknown> => ({
    nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
    setAttribute: () => undefined, removeAttribute: () => undefined,
  });
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {},
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
  return node('div');
}

const completed = (applyId: number): WorkbenchNotice => ({
  id: `activation.completed.${applyId}`,
  severity: 'info',
  domain: 'activation',
  title: 'Activation completed',
  message: `Manager confirmed 3f9a/apply${applyId}.`,
});

it('presents an event-triggered notice for BRIEF_NOTICE_MS, never re-presents it, and presents a new one', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  const host = stubDom();
  let shown = null as WorkbenchNotice | null;
  function Probe({ notice }: { notice: WorkbenchNotice | null }) {
    const presented = useBriefNotice(notice);
    useEffect(() => { shown = presented; });
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (notice: WorkbenchNotice | null) => act(() => {
    root.render(<Probe notice={notice} />);
  });

  await render(completed(1));
  expect(shown?.title).toBe('Activation completed');
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS - 1); });
  expect(shown?.id).toBe('activation.completed.1');
  await act(() => { vi.advanceTimersByTime(1); });
  expect(shown).toBeNull();

  // The same notice again is the same event: it does not re-present.
  await render(completed(1));
  expect(shown).toBeNull();

  await render(completed(2));
  expect(shown?.id).toBe('activation.completed.2');
  await act(() => root.unmount());
});

it('keeps each presented brief notice with its time, newest first, at most RECENT_NOTICE_LIMIT', async () => {
  const host = stubDom();
  let recent: readonly RecentNotice[] = [];
  function Probe({ notice }: { notice: WorkbenchNotice | null }) {
    useBriefNotice(notice);
    const list = useRecentNotices();
    useEffect(() => { recent = list; });
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (notice: WorkbenchNotice | null) => act(() => {
    root.render(<Probe notice={notice} />);
  });
  await render({ ...completed(7), history: { programRunId: 3 } });
  const presented = recent.length;
  expect(recent[0]).toMatchObject({
    severity: 'info', title: 'Activation completed', message: 'Manager confirmed 3f9a/apply7.',
    history: { programRunId: 3 },
  });
  expect(recent[0].presentedAtMs).toBeGreaterThan(0);
  // The same event re-rendered is not a new presentation.
  await render({ ...completed(7), history: { programRunId: 3 } });
  expect(recent.length).toBe(presented);
  for (let id = 100; id < 100 + RECENT_NOTICE_LIMIT + 3; id += 1) recordRecentNotice(completed(id));
  await render(null);
  expect(recent.length).toBe(RECENT_NOTICE_LIMIT);
  expect(recent[0].message).toBe(`Manager confirmed 3f9a/apply${100 + RECENT_NOTICE_LIMIT + 2}.`);
  await act(() => root.unmount());
});
