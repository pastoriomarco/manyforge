import { act, useEffect } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, expect, it, vi } from 'vitest';

import { manualPreviewRefusalNotice } from './App';
import { WorkbenchNotificationStack } from './components/WorkbenchNotifications';
import { readyState } from './testFixtures/robotControl';
import { robotControlReducer } from './state/robotControl';
import { BRIEF_NOTICE_MS, useBriefNotice, type WorkbenchNotice } from './state/workbenchNotices';

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

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

function previewRefusal() {
  const base = readyState();
  return robotControlReducer({ ...base, phase: 'planning' }, {
    type: 'preview_rejected', generation: base.generation,
    error: 'No valid path found for this target.',
    errorRefusal: {
      code: 'planning_failed', message: 'No valid path found for this target.',
      details: { reason: 'native_rrt_failed' }, origin: null,
    },
  });
}

it('shows a preview refusal briefly and clears it as soon as the next preview starts', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  const host = stubDom();
  const shown: { current: WorkbenchNotice | null } = { current: null };
  function Probe({ state }: { state: ReturnType<typeof previewRefusal> }) {
    const presented = useBriefNotice(manualPreviewRefusalNotice(state));
    useEffect(() => { shown.current = presented; });
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (state: ReturnType<typeof previewRefusal>) => act(() => {
    root.render(<Probe state={state} />);
  });

  const refused = previewRefusal();
  await render(refused);
  expect(shown.current).toMatchObject({ severity: 'info', title: 'Manual preview refused' });
  expect(shown.current?.actions).toBeUndefined();
  const activityMarkup = renderToStaticMarkup(
    <WorkbenchNotificationStack errorRow={null} warnings={[]}
      activity={shown.current === null ? [] : [shown.current]} />,
  );
  expect(activityMarkup).toContain('Manual preview refused');
  expect(activityMarkup).not.toContain('Acknowledge');

  const nextPreview = robotControlReducer(refused, { type: 'retry_preview' });
  expect(nextPreview.phase).toBe('planning');
  await render(nextPreview);
  expect(shown.current).toBeNull();

  const repeatedRefusal = robotControlReducer(nextPreview, {
    type: 'preview_rejected', generation: nextPreview.generation,
    error: 'No valid path found for this target.',
    errorRefusal: {
      code: 'planning_failed', message: 'No valid path found for this target.',
      details: { reason: 'native_rrt_failed' }, origin: null,
    },
  });
  await render(repeatedRefusal);
  expect(shown.current?.id).toBe(`robot-control.preview.${repeatedRefusal.status?.ownerEpoch}.${repeatedRefusal.generation}`);
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS); });
  expect(shown.current).toBeNull();
  await act(() => root.unmount());
});
