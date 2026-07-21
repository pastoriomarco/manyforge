import { describe, expect, it } from 'vitest';

import type { ExecutionHistoryEvent } from '../types/api';
import { filterExecutionHistoryEvents } from './ExecutionHistoryPanel';

const EVENTS: ExecutionHistoryEvent[] = [
  {
    event_index: 1,
    sequence: 1,
    timestamp_ns: 1,
    severity: 'INFO',
    channel: 'execution_lifecycle',
    message: 'Motion started',
    execution_id: 7,
    type: 'execute_started',
    details: {},
  },
  {
    event_index: 2,
    sequence: 2,
    timestamp_ns: 2,
    severity: 'WARN',
    channel: 'command_runtime',
    message: 'Recovery offer changed',
    execution_id: 7,
    type: 'recovery_action_rejected',
    details: { reason: 'stale' },
  },
  {
    event_index: 3,
    sequence: 3,
    timestamp_ns: 3,
    severity: 'ERROR',
    channel: 'fault_safety_stop',
    message: 'Collision with wall',
    execution_id: 8,
    type: 'collision_detected',
    details: { object: 'wall' },
  },
  {
    event_index: 4,
    sequence: 4,
    timestamp_ns: 4,
    severity: 'INFO',
    channel: 'command_runtime',
    message: 'Operator alarm activated',
    execution_id: 8,
    type: 'operator_alarm_activated',
    details: { condition_id: 'condition-wall', activation_id: 'activation-1' },
  },
  {
    event_index: 5,
    sequence: 5,
    timestamp_ns: 5,
    severity: 'INFO',
    channel: 'command_runtime',
    message: 'Operator alarm acknowledged',
    execution_id: 8,
    type: 'operator_alarm_acknowledged',
    details: { condition_id: 'condition-wall', activation_id: 'activation-1' },
  },
];

describe('execution history filtering', () => {
  it('separates messages, warnings, and errors', () => {
    expect(filterExecutionHistoryEvents(EVENTS, {
      severity: 'warning', eventType: '', channel: '', search: '',
    }).map((event) => event.type)).toEqual(['recovery_action_rejected']);
    expect(filterExecutionHistoryEvents(EVENTS, {
      severity: 'error', eventType: '', channel: '', search: '',
    }).map((event) => event.type)).toEqual(['collision_detected']);
  });

  it('combines execution, type, channel, and text filters', () => {
    expect(filterExecutionHistoryEvents(EVENTS, {
      severity: 'all',
      eventType: 'collision_detected',
      channel: 'fault_safety_stop',
      search: 'wall',
      executionId: 8,
    })).toEqual([EVENTS[2]]);
    expect(filterExecutionHistoryEvents(EVENTS, {
      severity: 'all',
      eventType: 'collision_detected',
      channel: 'fault_safety_stop',
      search: 'wall',
      executionId: 7,
    })).toEqual([]);
  });

  it('filters lifecycle phases and exact condition correlations', () => {
    expect(filterExecutionHistoryEvents(EVENTS, {
      severity: 'all', eventType: '', channel: '', search: '', alarmView: 'lifecycle',
    }).map((event) => event.type)).toEqual([
      'operator_alarm_activated',
      'operator_alarm_acknowledged',
    ]);
    expect(filterExecutionHistoryEvents(EVENTS, {
      severity: 'all',
      eventType: '',
      channel: '',
      search: '',
      alarmView: 'activation',
      conditionIdentity: 'condition-wall',
    }).map((event) => event.type)).toEqual(['operator_alarm_activated']);
    expect(filterExecutionHistoryEvents(EVENTS, {
      severity: 'all', eventType: '', channel: '', search: '', alarmView: 'raw',
    }).map((event) => event.type)).toEqual([
      'execute_started',
      'recovery_action_rejected',
      'collision_detected',
    ]);
  });
});
