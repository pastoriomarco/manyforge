import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import {
  ExecutionHistoryPanel,
  historyFilterQuery,
  type ExecutionHistoryEventView,
} from './ExecutionHistoryPanel';
import type { HistorianDetail } from '../types/headless';

const events: ExecutionHistoryEventView[] = [
  {
    id: '1', timestampNs: '1', severity: 'INFO', channel: 'execution',
    message: 'Motion started', executionId: 7, type: 'execute_started',
    programRunId: 'run-1', operationId: 'operation-1',
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
      search: ' collision wall ', programRunId: 'run-1', operationId: 'operation-1',
      correlations: { conditionId: 'condition-wall', actionSpanId: 'action-7' },
    })).toEqual({
      limit: 32, recentSeconds: 60, severities: ['ERROR', 'CRITICAL'],
      alarmPhase: 'acknowledged', components: ['manyforge.alarm'],
      eventTypes: ['operator_alarm_acknowledged'], search: 'collision wall',
      programRunId: 'run-1', operationId: 'operation-1',
      conditionId: 'condition-wall', actionSpanId: 'action-7',
    });
  });

  it('renders the server result without a second, page-local filter', () => {
    const markup = renderToStaticMarkup(createElement(ExecutionHistoryPanel, {
      open: true, onClose: vi.fn(), events, currentProgramRunId: 'another-run',
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
      currentProgramRunId: 'run-42',
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
      schemaVersion: 'manyforge.historian_detail.v2',
      sequence: 9,
      event: {
        schemaVersion: 'manyforge.historian_event.v2',
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
  });
});
