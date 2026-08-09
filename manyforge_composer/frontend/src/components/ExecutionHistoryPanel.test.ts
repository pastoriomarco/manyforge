import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import {
  ExecutionHistoryPanel,
  filterExecutionHistoryEvents,
  type ExecutionHistoryEventView,
} from './ExecutionHistoryPanel';

const events: ExecutionHistoryEventView[] = [
  {
    id: '1', timestampNs: 1, severity: 'INFO', channel: 'execution',
    message: 'Motion started', executionId: 7, type: 'execute_started',
  },
  {
    id: '2', timestampNs: 2, severity: 'WARN', channel: 'runtime',
    message: 'Offer changed', executionId: 7, type: 'recovery_rejected',
  },
  {
    id: '3', timestampNs: 3, severity: 'ERROR', channel: 'fault',
    message: 'Collision with wall', executionId: 8, type: 'collision',
    details: { condition: 'condition-wall' },
  },
  {
    id: '4', timestampNs: 4, severity: 'INFO', channel: 'alarm',
    message: 'Alarm activated', executionId: 8, type: 'alarm_event',
    alarmPhase: 'activation', details: { condition: 'condition-wall' },
  },
];

describe('execution history filtering', () => {
  it('combines severity, execution, and text filters', () => {
    expect(filterExecutionHistoryEvents(events, {
      severity: 'error', eventType: '', channel: '', search: 'wall', executionId: 8,
    }).map((event) => event.id)).toEqual(['3']);
    expect(filterExecutionHistoryEvents(events, {
      severity: 'warning', eventType: '', channel: '', search: '', executionId: 7,
    }).map((event) => event.id)).toEqual(['2']);
  });

  it('uses an adapter-supplied alarm phase instead of old event IDs', () => {
    expect(filterExecutionHistoryEvents(events, {
      severity: 'all', eventType: '', channel: '', search: '', alarmView: 'lifecycle',
    }).map((event) => event.id)).toEqual(['4']);
    expect(filterExecutionHistoryEvents(events, {
      severity: 'all', eventType: '', channel: '', search: '', alarmView: 'raw',
    }).map((event) => event.id)).toEqual(['1', '2', '3']);
  });

  it('combines retained event type, channel, and condition identity filters', () => {
    expect(filterExecutionHistoryEvents(events, {
      severity: 'all',
      eventType: 'alarm_event',
      channel: 'alarm',
      search: '',
      conditionIdentity: 'condition-wall',
    }).map((event) => event.id)).toEqual(['4']);
    expect(filterExecutionHistoryEvents(events, {
      severity: 'all',
      eventType: 'alarm_event',
      channel: 'alarm',
      search: '',
      conditionIdentity: 'condition-missing',
    })).toEqual([]);
  });

  it('renders the largest supported public history page within the presentation budget', () => {
    const page = Array.from({ length: 32 }, (_, index): ExecutionHistoryEventView => ({
      id: `event-${index + 1}`,
      timestampNs: index + 1,
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
      currentExecutionId: 42,
    }));
    const elapsedMs = performance.now() - started;

    expect(markup.match(/<article/g)).toHaveLength(32);
    expect(markup).toContain('Retained event 1');
    expect(markup).toContain('Retained event 32');
    expect(elapsedMs).toBeLessThan(1000);
  });
});
