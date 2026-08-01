import { describe, expect, it } from 'vitest';
import {
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
});
