import { readFileSync } from 'node:fs';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { NoticeItem, NoticeRow, WorkbenchNotificationStack } from '../components/WorkbenchNotifications';
import {
  previousSessionAlarmHandoff,
  activeViewerIncidentKinds,
  classifyWorkbenchNotices,
  noticeRowSummary,
  noticesOfSeverity,
  type WorkbenchNotice,
} from './workbenchNotices';

function notice(overrides: Partial<WorkbenchNotice> & Pick<WorkbenchNotice, 'id' | 'severity'>): WorkbenchNotice {
  return {
    domain: 'viewer',
    title: overrides.id,
    message: `${overrides.id} detail`,
    ...overrides,
  };
}

describe('workbench notification model', () => {
  it('orders errors, warnings, then information while keeping producer order', () => {
    const ordered = classifyWorkbenchNotices([
      notice({ id: 'info.a', severity: 'info' }),
      notice({ id: 'warning.a', severity: 'warning' }),
      notice({ id: 'error.a', severity: 'error' }),
      notice({ id: 'info.b', severity: 'info' }),
      notice({ id: 'error.b', severity: 'error' }),
    ], null);
    expect(ordered.map((row) => row.id)).toEqual([
      'error.a', 'error.b', 'warning.a', 'info.a', 'info.b',
    ]);
    expect(noticesOfSeverity(ordered, 'warning').map((row) => row.id)).toEqual(['warning.a']);
  });

  it('presents only the expected related gap of a controlled transition as information', () => {
    const expected = notice({
      id: 'viewer.scene', severity: 'error', expectedDuringTransition: true,
      incidentKind: 'viewer_scene_unverified',
    });
    const failure = notice({
      id: 'viewer.model', severity: 'error', incidentKind: 'viewer_model_unavailable',
    });
    const outside = classifyWorkbenchNotices([expected, failure], null);
    expect(outside.map((row) => row.severity)).toEqual(['error', 'error']);
    expect(activeViewerIncidentKinds(outside)).toEqual([
      'viewer_scene_unverified', 'viewer_model_unavailable',
    ]);
    const during = classifyWorkbenchNotices([expected, failure], { key: 'apply.1:1', preexisting: new Set() });
    // The initialization/unrelated failure stays red and is still reported.
    expect(during.find((row) => row.id === 'viewer.model')?.severity).toBe('error');
    expect(during.find((row) => row.id === 'viewer.scene')?.severity).toBe('info');
    expect(activeViewerIncidentKinds(during)).toEqual(['viewer_model_unavailable']);
  });

  it('never downgrades a condition that was already present when the transition began', () => {
    const scene = notice({
      id: 'viewer.scene', severity: 'error', expectedDuringTransition: true,
      incidentKind: 'viewer_scene_unverified',
    });
    const during = classifyWorkbenchNotices([scene], {
      key: 'apply.1:1', preexisting: new Set(['viewer.scene']),
    });
    expect(during[0].severity).toBe('error');
    expect(activeViewerIncidentKinds(during)).toEqual(['viewer_scene_unverified']);
  });

  it('scopes a transition only while its key is present and captures earlier conditions', () => {
    const source = readFileSync(new URL('../App.tsx', import.meta.url), 'utf8');
    // Only a read pending owner status is evidence; no advertised, submitted
    // or unobservable operation scopes a transition.
    expect(source).toContain("&& activationStatus?.state === 'pending'");
    expect(source).toContain('&& !activationUnobservable');
    expect(source).not.toContain('maintenance !== null\n    || activationPending');
    expect(source).not.toContain('deploymentController.applying;\n');
    expect(source).toContain('useTransitionScope(transitionKey, workbenchNotices)');
  });

  it('reports each viewer incident kind once regardless of repeated notices', () => {
    expect(activeViewerIncidentKinds([
      notice({ id: 'a', severity: 'error', incidentKind: 'viewer_geometry_unavailable' }),
      notice({ id: 'b', severity: 'error', incidentKind: 'viewer_geometry_unavailable' }),
      notice({ id: 'c', severity: 'warning', incidentKind: 'viewer_model_unavailable' }),
    ])).toEqual(['viewer_geometry_unavailable']);
  });

  it('summarizes a row in one line', () => {
    expect(noticeRowSummary([], { one: 'warning', many: 'warnings' })).toBe('');
    expect(noticeRowSummary([notice({ id: 'one', severity: 'warning' })], {
      one: 'warning', many: 'warnings',
    })).toBe('one — one detail');
    expect(noticeRowSummary([
      notice({ id: 'first', severity: 'info' }),
      notice({ id: 'second', severity: 'info' }),
    ], { one: 'activity', many: 'activities' })).toBe('2 activities · first');
  });

  it('renders warning and activity rows without acknowledgement but with source actions', () => {
    const retry = vi.fn();
    const warning = renderToStaticMarkup(<NoticeRow severity="warning" notices={[
      notice({
        id: 'viewer.point-cloud', severity: 'warning', title: 'Point-cloud layer unavailable',
        message: 'Frame source offline.',
        actions: [{ id: 'retry', label: 'Retry layer', run: retry }],
      }),
    ]} />);
    expect(warning).toContain('Warnings');
    expect(warning).toContain('Point-cloud layer unavailable — Frame source offline.');
    expect(warning).toContain('>Retry layer</button>');
    expect(warning).not.toMatch(/Acknowledge|Dismiss/);
    const activity = renderToStaticMarkup(<NoticeRow severity="info" notices={[
      notice({ id: 'activation', severity: 'info', title: 'Activation in progress', busy: true }),
    ]} />);
    expect(activity).toContain('Activity');
    expect(activity).toContain('notification-busy');
    expect(renderToStaticMarkup(<NoticeRow severity="info" notices={[]} />)).toBe('');
  });

  it('shows every row with content and nothing when all are empty', () => {
    expect(renderToStaticMarkup(
      <WorkbenchNotificationStack errorRow={null} warnings={[]} activity={[]} />,
    )).toBe('');
    const markup = renderToStaticMarkup(<WorkbenchNotificationStack
      errorRow={<section className="error-row-marker" />}
      warnings={[notice({ id: 'w', severity: 'warning' })]}
      activity={[notice({ id: 'i', severity: 'info' })]}
    />);
    const order = ['error-row-marker', 'aria-label="Warnings"', 'aria-label="Activity"']
      .map((token) => markup.indexOf(token));
    expect(order.every((index) => index >= 0)).toBe(true);
    expect([...order].sort((left, right) => left - right)).toEqual(order);
  });
});


describe('previous-session alarm presentation handoff', () => {
  const session = 'a'.repeat(32);
  it.each(['registered', 'acknowledged', 'quiet'])(
    'hands off only the exact current archive after a %s receipt', (status) => {
      expect(previousSessionAlarmHandoff(session, null, { session_id: session, status })).toBe(session);
    },
  );
  it.each([undefined, null, {}, { session_id: 'b'.repeat(32), status: 'registered' },
    { session_id: 'a'.repeat(32), status: 'failed' },
    { session_id: 'a'.repeat(32), status: 'registered', invented: true }])(
    'keeps the fallback without an exact successful receipt: %j', (receipt) => {
      expect(previousSessionAlarmHandoff(session, null, receipt)).toBeNull();
    },
  );
  it('keeps the proven handoff when the acknowledged alarm disappears or transport is lost', () => {
    const registered = previousSessionAlarmHandoff(session, null, { session_id: session, status: 'registered' });
    expect(previousSessionAlarmHandoff(session, registered, undefined)).toBe(session);
    expect(previousSessionAlarmHandoff(session, registered, null)).toBe(session);
    expect(previousSessionAlarmHandoff('b'.repeat(32), registered, null)).not.toBe('b'.repeat(32));
  });
  it('keeps technical details collapsed without hiding the plain message', () => {
    const markup = renderToStaticMarkup(<NoticeItem notice={{
      id: 'cold-session.a', severity: 'warning', domain: 'recovery',
      title: 'ManyForge did not close correctly last time.',
      message: 'The cell started empty; nothing from the previous session was resumed.',
      details: 'Session a; program_cycle; not_imported; failure time unavailable.',
    }} />);
    expect(markup).toContain('The cell started empty');
    expect(markup).toContain('<details><summary>Details</summary>');
    expect(markup).not.toContain('<details open');
    expect(markup).toContain('not_imported');
  });
});
