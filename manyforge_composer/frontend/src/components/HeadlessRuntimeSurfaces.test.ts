import { describe, expect, it } from 'vitest';
import {
  attentionAlarmsNewestFirst,
  historianEventView,
  operatorAlarmView,
  runtimeStatusView,
} from './HeadlessRuntimeSurfaces';
import type {
  HeadlessRuntimeSnapshot,
  HistorianStoredEvent,
  OperatorAlarmRecord,
} from '../types/headless';

function source() {
  return {
    available: true,
    freshness: 'fresh' as const,
    knownGaps: [],
    payload: {},
    sourceGeneration: 1,
    sourceOwnerEpoch: 'source-epoch',
    sourceOwnerId: 'source-owner',
    receiverAgeNs: 10,
  };
}

describe('headless Composer presentation adapters', () => {
  it('never presents an H0 projection as execution or robot readiness', () => {
    const runtime: HeadlessRuntimeSnapshot = {
      schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
      complete: true,
      projectionGeneration: 2,
      projectionOwnerEpoch: 'projection-epoch',
      projectionOwnerId: 'headless-api',
      reasons: [],
      unavailableSections: [],
      sections: {
        manager: source(),
        logicalScene: source(),
        sceneRuntime: source(),
      },
    };

    const view = runtimeStatusView(runtime);

    expect(view.readiness).toBe('degraded');
    expect(view.profileLabel).toBe('H0 projection');
    expect(view.userMessage).toContain('Program execution and motion remain gated');
    expect(view.capabilities).not.toContain('robot ready');
  });

  it('maps only the four-state owner record into alarm presentation', () => {
    const alarm: OperatorAlarmRecord = {
      conditionId: 'condition-1',
      activationId: 'activation-1',
      recordRevision: 4,
      activatedAtNs: 100,
      definitionId: 'manyforge.test.alarm',
      lifecycleState: 'returned_unacknowledged',
      active: false,
      acknowledged: false,
      priority: 'critical',
      alarmClass: 'runtime_health',
      title: 'Provider unavailable',
      message: 'The provider disappeared.',
      consequence: 'Runtime facts are incomplete.',
      operatorResponse: 'Inspect the provider.',
      occurrenceCount: 2,
      requiresMotionAttention: true,
      subject: { subjectKind: 'cell', subjectId: 'cell-1', resourceIds: [] },
      details: {},
    };

    const view = operatorAlarmView(alarm);

    expect(view.lifecycleLabel).toBe('RESOLVED — ACKNOWLEDGEMENT REQUIRED');
    expect(view.severity).toBe('critical');
    expect(view.details.occurrence_count).toBe(2);
  });

  it('selects attention panels by newest activation rather than server priority', () => {
    const base: OperatorAlarmRecord = {
      conditionId: 'condition-base',
      activationId: 'activation-base',
      recordRevision: 1,
      activatedAtNs: 100,
      definitionId: 'manyforge.test.alarm',
      lifecycleState: 'active_unacknowledged',
      active: true,
      acknowledged: false,
      priority: 'critical',
      alarmClass: 'runtime_health',
      title: 'Test alarm',
      message: 'A condition requires attention.',
      consequence: 'Runtime facts are incomplete.',
      operatorResponse: 'Inspect the provider.',
      occurrenceCount: 1,
      requiresMotionAttention: true,
      subject: { subjectKind: 'cell', subjectId: 'cell-1', resourceIds: [] },
      details: {},
    };
    const alarms = [
      base,
      { ...base, conditionId: 'condition-new', activatedAtNs: 300, priority: 'low' as const },
      { ...base, conditionId: 'condition-middle', activatedAtNs: 200, priority: 'high' as const },
      { ...base, conditionId: 'condition-standing', activatedAtNs: 400, lifecycleState: 'active_acknowledged' as const },
    ];

    expect(attentionAlarmsNewestFirst(alarms).map((alarm) => alarm.conditionId))
      .toEqual(['condition-new', 'condition-middle', 'condition-base']);
  });

  it('uses exact historian event types for lifecycle filtering', () => {
    const stored: HistorianStoredEvent = {
      sequence: 11,
      event: {
        schemaVersion: 'manyforge.historian_event.v1',
        eventId: 'event-11',
        component: 'manyforge.alarm',
        eventType: 'operator_alarm_acknowledged',
        severity: 'INFO',
        occurredAtNs: 100,
        occurredAtWall: '2026-08-05T12:00:00Z',
        payload: { condition_id: 'condition-1' },
      },
    };

    const view = historianEventView(stored);

    expect(view.alarmPhase).toBe('acknowledgement');
    expect(view.message).toContain('condition-1');
    expect(view.details).toEqual({ condition_id: 'condition-1' });
  });
});
