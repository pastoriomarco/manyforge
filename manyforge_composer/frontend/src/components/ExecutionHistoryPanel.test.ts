import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import {
  ExecutionHistoryPanel,
  historyFilterQuery,
  historyStartOptions,
  historyRunEnd,
  type ExecutionHistoryEventView,
} from './ExecutionHistoryPanel';
import type { HistorianDetail } from '../types/headless';

const CURRENT_START = '3f9a000000000001';
const EARLIER_START = '1b2c000000000002';

const events: ExecutionHistoryEventView[] = [
  {
    id: '1', timestampNs: '1', severity: 'INFO', channel: 'execution',
    message: 'Motion started', executionId: 7, type: 'execute_started',
    programRunId: 1, operationId: 7,
  },
  {
    id: '2', timestampNs: '2', severity: 'WARN', channel: 'runtime',
    message: 'Offer changed', executionId: 7, type: 'recovery_rejected',
  },
  {
    id: '3', timestampNs: '3', severity: 'ERROR', channel: 'fault',
    message: 'Collision with wall', executionId: 8, type: 'collision',
    details: { condition: 'condition-wall' },
  },
  {
    id: '4', timestampNs: '4', severity: 'INFO', channel: 'alarm',
    message: 'Alarm activated', executionId: 8, type: 'alarm_event',
    alarmPhase: 'activation', details: { condition: 'condition-wall' },
  },
];

describe('execution history filtering', () => {
  it('sends every predicate to the backend without depending on loaded events or browser time', () => {
    expect(historyFilterQuery({
      severity: 'error', alarmView: 'acknowledgement', timeWindow: 'minute',
      component: ' manyforge.alarm ', eventType: 'operator_alarm_acknowledged',
      search: ' collision wall ', programRunId: '3f9a/run1', operationId: '7',
      applicationStartId: CURRENT_START,
      correlations: { conditionId: 'condition-wall', actionSpanId: 'action-7' },
    })).toEqual({
      limit: 32, recentSeconds: 60, severities: ['ERROR', 'CRITICAL'],
      alarmPhase: 'acknowledged', components: ['manyforge.alarm'],
      eventTypes: ['operator_alarm_acknowledged'], search: 'collision wall',
      programRunId: 1, operationId: 7, applicationStartId: CURRENT_START,
      conditionId: 'condition-wall', actionSpanId: 'action-7',
    });
  });

  const idQuery = (programRunId: string, operationId = '') => historyFilterQuery({
    severity: 'all', alarmView: 'all', timeWindow: 'all', component: '', eventType: '',
    search: '', programRunId, operationId, applicationStartId: CURRENT_START,
  });

  it('accepts the display form, the full form and the bare number for run and operation filters', () => {
    expect(idQuery('3f9a/run3')).toMatchObject({ programRunId: 3, applicationStartId: CURRENT_START });
    expect(idQuery(' 3F9A/RUN3 ')).toMatchObject({ programRunId: 3, applicationStartId: CURRENT_START });
    expect(idQuery('3')).toMatchObject({ programRunId: 3, applicationStartId: CURRENT_START });
    expect(idQuery('', '3f9a/op7')).toMatchObject({ operationId: 7, applicationStartId: CURRENT_START });
    // The full form names its own start, also an earlier one.
    expect(idQuery(`${EARLIER_START}/run3`)).toMatchObject({ programRunId: 3, applicationStartId: EARLIER_START });
    expect(idQuery('', `${CURRENT_START}/op7`)).toMatchObject({ operationId: 7, applicationStartId: CURRENT_START });
  });

  it('sends the application-start id only with a run or operation filter', () => {
    expect(idQuery('')?.applicationStartId).toBeUndefined();
    expect(idQuery('')?.programRunId).toBeUndefined();
  });

  it('refuses filter text that names no run or operation of a known start', () => {
    for (const text of ['run3', 'program-run-1', '3f9a/op3', '0', '1b2c/run3', `${CURRENT_START}/run`]) {
      expect(idQuery(text)).toBeNull();
    }
    // The run names one start and the operation another.
    expect(idQuery(`${EARLIER_START}/run3`, '3f9a/op7')).toBeNull();
    // Without a current start only the full form names one.
    expect(historyFilterQuery({
      severity: 'all', alarmView: 'all', timeWindow: 'all', component: '', eventType: '',
      search: '', programRunId: '3', operationId: '', applicationStartId: null,
    })).toBeNull();
  });

  it('keeps a query without a run or operation filter valid when no start is known', () => {
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true, onClose: vi.fn(), events: [], onReload: vi.fn(),
      currentProgramRunId: 3, applicationStartId: null,
    }));
    expect(markup).toContain('Export filtered diagnostic bundle');
    expect(markup).not.toMatch(/disabled="">Apply filters/);
  });

  it('renders the server result without a second, page-local filter', () => {
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true, onClose: vi.fn(), events, currentProgramRunId: 2,
    }));
    expect(markup.match(/class="history-event /g)).toHaveLength(4);
    expect(markup).toContain('Recorded UTC unavailable');
  });

  it('renders the largest supported public history page within the presentation budget', () => {
    const page = Array.from({ length: 32 }, (_, index): ExecutionHistoryEventView => ({
      id: `event-${index + 1}`,
      timestampNs: String(index + 1),
      timestamp: `2026-08-09T12:00:${String(index).padStart(2, '0')}Z`,
      severity: index % 5 === 0 ? 'WARN' : 'INFO',
      channel: index % 2 === 0 ? 'execution' : 'runtime',
      message: `Retained event ${index + 1}`,
      executionId: 42,
      type: index % 2 === 0 ? 'operation_progressed' : 'owner_status_refreshed',
      details: { sequence: index + 1 },
    }));
    const started = performance.now();
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true,
      onClose: vi.fn(),
      events: page,
      currentProgramRunId: 42,
      onReload: vi.fn(),
      onLoadOlder: vi.fn(),
      hasOlder: true,
    }));
    const elapsedMs = performance.now() - started;

    expect(markup.match(/class="history-event /g)).toHaveLength(32);
    expect(markup).toContain('Retained event 1');
    expect(markup).toContain('Retained event 32');
    expect(markup).toContain('Program run');
    expect(markup).toContain('Operation');
    expect(markup).toContain('Older');
    expect(elapsedMs).toBeLessThan(1000);
  });

  it('lists the current start first, then earlier starts newest first, never as a raw id', () => {
    const newer = '0bad000000000003';
    expect(historyStartOptions(CURRENT_START, [CURRENT_START, newer, EARLIER_START])).toEqual([
      { value: '', label: 'Current start', title: CURRENT_START },
      { value: newer, label: 'Earlier start 0bad (previous start)', title: newer },
      { value: EARLIER_START, label: 'Earlier start 1b2c', title: EARLIER_START },
    ]);
    // Once a start's last row is loaded its label shows that row's time;
    // before, or when its read failed (nothing recorded), the fallback stays.
    const lastNs = '1785931200000000000';
    const time = new Date(Number(BigInt(lastNs) / 1_000_000n)).toLocaleString();
    expect(historyStartOptions(CURRENT_START, [newer, EARLIER_START], { [newer]: lastNs, [EARLIER_START]: null })
      .map((option) => option.label)).toEqual([
      'Current start',
      `Earlier start, last event ${time} (previous start)`,
      'Earlier start 1b2c',
    ]);
    expect(historyStartOptions(CURRENT_START, [EARLIER_START], { [EARLIER_START]: lastNs })[1]?.label)
      .toBe(`Earlier start, last event ${time} (previous start)`);
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true, onClose: vi.fn(), events: [], applicationStartId: CURRENT_START,
    }));
    expect(markup).toContain('<span>Start</span>');
    expect(markup).toContain('Current start');
  });

  it('filters by the selected start and clears it for the current start', () => {
    const query = (startFilter: string | null, programRunId = '') => historyFilterQuery({
      severity: 'all', alarmView: 'all', timeWindow: 'all', component: '', eventType: '',
      search: '', programRunId, operationId: '', applicationStartId: CURRENT_START, startFilter,
    });
    // The whole selected start, and its numbers.
    expect(query(EARLIER_START)?.applicationStartId).toBe(EARLIER_START);
    expect(query(EARLIER_START, '3')).toMatchObject({ programRunId: 3, applicationStartId: EARLIER_START });
    expect(query(EARLIER_START, '1b2c/run3')).toMatchObject({ programRunId: 3, applicationStartId: EARLIER_START });
    // A full form naming another start than the selected one names nothing.
    expect(query(EARLIER_START, `${CURRENT_START}/run3`)).toBeNull();
    // "Current start": no start filter; numbers mean the current start.
    expect(query(null)?.applicationStartId).toBeUndefined();
    expect(query(null, '3')).toMatchObject({ programRunId: 3, applicationStartId: CURRENT_START });
  });

  it('shows a row\'s start only where it differs from the selected start', () => {
    const row = (id: string, applicationStartId: string): ExecutionHistoryEventView => ({
      id, timestampNs: '1', channel: 'manyforge.runtime', message: id, type: 'operation_progressed',
      applicationStartId,
    });
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true,
      onClose: vi.fn(),
      applicationStartId: '3f9a0000000000a1',
      events: [row('current row', '3f9a0000000000a1'), row('earlier row', '0bad0000000000b2')],
    }));
    expect(markup).toContain('start 0bad');
    expect(markup).not.toContain('start 3f9a');
  });

  it('never mounts more than one bounded backend page', () => {
    const merged = Array.from({ length: 64 }, (_, index): ExecutionHistoryEventView => ({
      id: `merged-${index + 1}`,
      timestampNs: String(64 - index),
      severity: 'INFO',
      channel: 'manyforge.composer',
      message: `Merged event ${index + 1}`,
      type: 'client_incident_resolved',
    }));

    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true,
      onClose: vi.fn(),
      events: merged,
    }));

    expect(markup.match(/class="history-event /g)).toHaveLength(32);
    expect(markup).toContain('Merged event 32');
    expect(markup).not.toContain('Merged event 33');
  });

  it('never reparses backend-rejected UTC and disables old-page selection during replacement', () => {
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true, onClose: vi.fn(), loading: true,
      events: [{ id: 'invalid-wall', timestampNs: '7', timestampUtcNs: null,
        timestamp: '2026-02-30T00:00:00Z', channel: 'test', message: 'Fault', type: 'error' }],
    }));
    expect(markup).toContain('Recorded UTC unavailable');
    expect(markup).toContain('Original recorded wall time: 2026-02-30T00:00:00Z');
    expect(markup).toContain('disabled="" class="history-event');
  });

  it('keeps error severity on a selected sequenced row', () => {
    const selected: HistorianDetail = {
      schemaVersion: 'manyforge.historian_detail.v3',
      sequence: 9,
      event: {
        schemaVersion: 'manyforge.historian_event.v3',
        eventId: 'event-9',
        component: 'manyforge.composer',
        eventType: 'client_incident_activated',
        severity: 'ERROR',
        occurredAtNs: '9',
        occurredAtWall: '2026-09-04T10:00:00Z',
        payload: {},
      },
      sections: {
        incident: { available: false, reason: 'not published', value: null },
        executionDigest: { available: false, reason: 'not published', value: null },
        blackboard: { available: false, reason: 'not published', value: null },
        latency: { available: false, reason: 'not published', value: null },
      },
    };
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true,
      onClose: vi.fn(),
      events: [{
        id: 'event-9',
        sequence: 9,
        timestampNs: '9',
        severity: 'ERROR',
        channel: 'manyforge.composer',
        message: 'Control connection stale',
        type: 'client_incident_activated',
      }],
      selectedDetail: selected,
    }));

    expect(markup).toContain('history-event severity-error selected');
    expect(markup).not.toContain('run-end');
  });

  it('shows a Program run\'s end reason in its event detail', () => {
    expect(historyRunEnd({})).toBeNull();
    expect(historyRunEnd({ run_end: { end_reason: 'completed', details: {} } }))
      .toEqual({ label: 'Completed', tone: 'neutral', text: 'Completed' });
    const selected: HistorianDetail = {
      schemaVersion: 'manyforge.historian_detail.v3',
      sequence: 10,
      event: {
        schemaVersion: 'manyforge.historian_event.v3',
        eventId: 'event-10',
        component: 'manyforge.program_runner',
        eventType: 'program_run_ended',
        severity: 'WARN',
        occurredAtNs: '10',
        occurredAtWall: '2026-11-01T10:00:00Z',
        payload: { run_end: { end_reason: 'stopped', details: { stopped_by: 'rule', rule_id: 'door' } } },
      },
      sections: {
        incident: { available: false, reason: 'not published', value: null },
        executionDigest: { available: false, reason: 'not published', value: null },
        blackboard: { available: false, reason: 'not published', value: null },
        latency: { available: false, reason: 'not published', value: null },
      },
    };
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true, onClose: vi.fn(), events: [], selectedDetail: selected,
    }));
    expect(markup).toContain('<p class="run-end tone-warning" role="status"><strong>Stopped</strong> · Stopped by rule door</p>');
  });
});
