import { act, useEffect } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import {
  activeViewerIncidentKinds,
  useViewerConditionHold,
  VIEWER_CONDITION_HOLD_MS,
  type WorkbenchNotice,
} from './workbenchNotices';

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

// Minimal React DOM host (the pattern of MeasuredRobotFrameDriver.test.tsx):
// real effects and timers, no DOM package.
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

const scene: WorkbenchNotice = {
  id: 'viewer.scene', severity: 'error', domain: 'viewer',
  title: 'Applied Scene not verified', message: 'stale', incidentKind: 'viewer_scene_unverified',
};
const alarm: WorkbenchNotice = {
  id: 'activation.1', severity: 'error', domain: 'activation', title: 'Activation failed', message: 'x',
};

it('turns a viewer condition into a reported error only after it lasts past the hold', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance'] });
  const host = stubDom();
  let presented: WorkbenchNotice[] = [];
  const capture = (notices: WorkbenchNotice[]) => { presented = notices; };
  function Probe({ notices, onPresented }: {
    notices: WorkbenchNotice[];
    onPresented: (notices: WorkbenchNotice[]) => void;
  }) {
    const held = useViewerConditionHold(notices);
    useEffect(() => onPresented(held));
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (notices: WorkbenchNotice[]) => act(() => {
    root.render(<Probe notices={notices} onPresented={capture} />);
  });

  // A short catch-up gap: information, never an error, never reported.
  await render([scene, alarm]);
  expect(presented.find((n) => n.id === 'viewer.scene')).toMatchObject({ severity: 'info', busy: true });
  expect(presented.find((n) => n.id === 'activation.1')?.severity).toBe('error');
  expect(activeViewerIncidentKinds(presented)).toEqual([]);
  await act(() => { vi.advanceTimersByTime(VIEWER_CONDITION_HOLD_MS - 100); });
  await render([alarm]);
  await act(() => { vi.advanceTimersByTime(VIEWER_CONDITION_HOLD_MS); });
  expect(presented.map((n) => n.id)).toEqual(['activation.1']);

  // A lasting condition: an error with its real onset, reported once.
  const onset = performance.now();
  await render([scene]);
  await act(() => { vi.advanceTimersByTime(VIEWER_CONDITION_HOLD_MS); });
  expect(presented[0]).toMatchObject({ severity: 'error', incidentKind: 'viewer_scene_unverified' });
  expect(presented[0].firstObservedMs).toBe(onset);
  expect(activeViewerIncidentKinds(presented)).toEqual(['viewer_scene_unverified']);

  // It clears, then returns: a new occurrence is held again.
  await render([]);
  await render([scene]);
  expect(presented[0].severity).toBe('info');
  await act(() => { root.unmount(); });
});
