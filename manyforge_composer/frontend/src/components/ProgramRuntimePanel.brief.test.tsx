import { act, useEffect } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import { BRIEF_NOTICE_MS, type WorkbenchNotice } from '../state/workbenchNotices';
import type {
  AutomaticRetryAuditEvent,
  RuntimeControlAuditEvent,
  TakeoverFallbackAuditEvent,
} from '../types/api';
import {
  useBriefAutomaticRetry,
  useBriefIoSourceUnavailable,
  useBriefTakeoverFallback,
} from './ProgramRuntimePanel';

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

function retry(eventRevision: number, attempt: number, cause: string): AutomaticRetryAuditEvent {
  return {
    eventRevision,
    kind: 'program_node_automatic_retry',
    facts: {
      nodeInstanceId: 'move', attempt, maxAttempts: 4, cause, delayS: 1, failure: null,
    },
  };
}

it('shows a brief warning for each new automatic retry, and never re-shows one it has seen', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  const host = stubDom();
  let shown = null as WorkbenchNotice | null;
  function Probe({ runId, audit }: { runId: string | null; audit: RuntimeControlAuditEvent[] | null }) {
    const notice = useBriefAutomaticRetry(runId, audit);
    useEffect(() => { shown = notice; });
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (runId: string | null, audit: RuntimeControlAuditEvent[] | null) => act(() => {
    root.render(<Probe runId={runId} audit={audit} />);
  });

  // Events already in the history when the panel starts show nothing.
  const history = [retry(2, 2, 'collision_stop_nominal')];
  await render('run.1', history);
  expect(shown).toBeNull();

  // A new retry shows one warning (amber, no acknowledgement) for BRIEF_NOTICE_MS.
  await render('run.1', [...history, retry(3, 3, 'planning_no_effect')]);
  expect(shown).toMatchObject({
    severity: 'warning',
    domain: 'runtime',
    title: 'Planning failed on move · attempt 3 of 4',
  });
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS - 1); });
  expect(shown).not.toBeNull();
  await act(() => { vi.advanceTimersByTime(1); });
  expect(shown).toBeNull();

  // The same events again, as a fresh snapshot: nothing re-shows.
  await render('run.1', [...history, retry(3, 3, 'planning_no_effect')]);
  expect(shown).toBeNull();

  // A newer retry re-triggers, and restarts the timer.
  await render('run.1', [...history, retry(3, 3, 'planning_no_effect'), retry(4, 4, 'collision_stop_nominal')]);
  expect(shown?.title).toBe('Collision Stop on move · attempt 4 of 4');
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS - 1); });
  expect(shown?.title).toBe('Collision Stop on move · attempt 4 of 4');
  await act(() => { vi.advanceTimersByTime(1); });
  expect(shown).toBeNull();

  // The first retry of a new run is new too.
  await render('run.2', [retry(2, 2, 'collision_stop_nominal')]);
  expect(shown?.title).toBe('Collision Stop on move · attempt 2 of 4');
  await act(() => root.unmount());
});

it('shows the first retry of a run that started with none, and nothing before the audit loads', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  const host = stubDom();
  let shown = null as WorkbenchNotice | null;
  function Probe({ runId, audit }: { runId: string | null; audit: RuntimeControlAuditEvent[] | null }) {
    const notice = useBriefAutomaticRetry(runId, audit);
    useEffect(() => { shown = notice; });
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (runId: string | null, audit: RuntimeControlAuditEvent[] | null) => act(() => {
    root.render(<Probe runId={runId} audit={audit} />);
  });

  // Controls not loaded yet: nothing to announce, and nothing to baseline.
  await render('run.1', null);
  expect(shown).toBeNull();
  // The first loaded audit has no retry: it is the baseline.
  await render('run.1', []);
  expect(shown).toBeNull();
  // The first retry of the run is new, and is announced.
  await render('run.1', [retry(2, 2, 'planning_no_effect')]);
  expect(shown?.title).toBe('Planning failed on move · attempt 2 of 4');
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS); });
  expect(shown).toBeNull();
  await act(() => root.unmount());
});

it('shows a brief warning when an io wait source becomes unavailable, not when it recovers', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  const host = stubDom();
  let shown = null as WorkbenchNotice | null;
  function Probe({ audit }: { audit: RuntimeControlAuditEvent[] | null }) {
    const notice = useBriefIoSourceUnavailable('run.1', audit);
    useEffect(() => { shown = notice; });
    return null;
  }
  const io = (eventRevision: number, available: boolean): RuntimeControlAuditEvent => ({
    eventRevision,
    kind: available ? 'program_node_io_source_available' : 'program_node_io_source_unavailable',
    facts: { node_instance_id: 'wait_part', node_attempt_id: 'a1', quality: available ? 'fresh' : 'stale' },
  });
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (audit: RuntimeControlAuditEvent[] | null) => act(() => {
    root.render(<Probe audit={audit} />);
  });
  await render([io(2, false)]);
  expect(shown).toBeNull();
  await render([io(2, false), io(3, true)]);
  expect(shown).toBeNull();
  await render([io(2, false), io(3, true), io(4, false)]);
  expect(shown).toMatchObject({
    severity: 'warning',
    domain: 'runtime',
    title: 'Input source unavailable on wait_part · still waiting',
  });
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS); });
  expect(shown).toBeNull();
  await act(() => root.unmount());
});

it('shows a brief warning for a new takeover fallback on either path', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  const host = stubDom();
  let shown = null as WorkbenchNotice | null;
  function Probe({ audit }: { audit: RuntimeControlAuditEvent[] | null }) {
    const notice = useBriefTakeoverFallback('run.1', audit);
    useEffect(() => { shown = notice; });
    return null;
  }
  const fallback = (
    eventRevision: number,
    path: TakeoverFallbackAuditEvent['facts']['path'],
    code: string | null,
    failureStage: string | null = null,
  ): TakeoverFallbackAuditEvent => ({
    eventRevision,
    kind: 'program_node_takeover_fallback',
    facts: {
      nodeInstanceId: 'move', incumbentOperationId: 'op.1', path,
      code, message: code === null ? null : 'late', failureStage, preparationMs: 40.5, budgetMs: 30,
    },
  });
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = (audit: RuntimeControlAuditEvent[] | null) => act(() => {
    root.render(<Probe audit={audit} />);
  });
  // A smooth takeover records no event, so the history alone shows nothing.
  await render([]);
  expect(shown).toBeNull();
  await render([fallback(2, 'spring_back_hold', 'takeover_preparation_late')]);
  expect(shown).toMatchObject({
    severity: 'warning',
    domain: 'runtime',
    title: 'Spring-back fallback on move · takeover preparation late (prep 41 of 30 ms)',
    message: 'Recorded in the runtime-control events.',
  });
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS); });
  expect(shown).toBeNull();
  await render([
    fallback(2, 'spring_back_hold', 'takeover_preparation_late'),
    fallback(3, 'ordinary_containment', null),
  ]);
  expect(shown?.title).toBe('Stop fallback on move (prep 41 of 30 ms)');
  expect(shown?.severity).toBe('warning');
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS); });
  expect(shown).toBeNull();
  // The stage where the time ran out, when the Kernel names it.
  await render([
    fallback(2, 'spring_back_hold', 'takeover_preparation_late'),
    fallback(3, 'ordinary_containment', null),
    fallback(4, 'spring_back_hold', 'planning_timeout', 'spring_back_planning'),
  ]);
  expect(shown?.title).toBe(
    'Spring-back fallback on move · planning timeout during spring_back_planning (prep 41 of 30 ms)',
  );
  await act(() => { vi.advanceTimersByTime(BRIEF_NOTICE_MS); });
  expect(shown).toBeNull();
  await act(() => root.unmount());
});
