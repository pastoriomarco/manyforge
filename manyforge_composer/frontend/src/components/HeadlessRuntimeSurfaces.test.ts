import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import {
  HeadlessRuntimeSurfacesView,
  attentionAlarmsNewestFirst,
  historianEventView,
  operatorAlarmView,
  runtimeStatusView,
} from './HeadlessRuntimeSurfaces';
import type {
  ComposerHeadlessState,
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
    expect(view.components).toContainEqual(expect.objectContaining({
      id: 'execution_owner',
      status: 'warning',
    }));
    expect(view.capabilities).not.toContain('robot ready');
  });

  it('reports ready only when runtime and addressed execution-owner projections are present', () => {
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

    const ready = runtimeStatusView(runtime, {
      unacknowledged: 0,
      activeAcknowledged: 0,
      programCycleState: 'idle_after_reset',
      executionOwnerAvailable: true,
    });
    const terminal = runtimeStatusView(runtime, {
      unacknowledged: 0,
      activeAcknowledged: 0,
      programCycleState: 'terminal_awaiting_reset',
      executionOwnerAvailable: true,
    });

    expect(ready.readiness).toBe('ready');
    expect(terminal.readiness).toBe('degraded');
    expect(terminal.components).toContainEqual(expect.objectContaining({
      id: 'program_cycle',
      status: 'warning',
    }));
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

  it('presents owner status, attention and standing alarms, history access, and reconnect together', () => {
    const attentionAlarm: OperatorAlarmRecord = {
      conditionId: 'condition-attention',
      activationId: 'activation-attention',
      recordRevision: 3,
      activatedAtNs: 300,
      definitionId: 'manyforge.test.attention',
      lifecycleState: 'active_unacknowledged',
      active: true,
      acknowledged: false,
      priority: 'high',
      alarmClass: 'runtime_health',
      title: 'Planner unavailable',
      message: 'The selected planner is unavailable.',
      consequence: 'New planning is unavailable.',
      operatorResponse: 'Inspect the planner provider.',
      occurrenceCount: 1,
      requiresMotionAttention: true,
      subject: { subjectKind: 'component', subjectId: 'planner-1', resourceIds: [] },
      details: {},
    };
    const standingAlarm: OperatorAlarmRecord = {
      ...attentionAlarm,
      conditionId: 'condition-standing',
      activationId: 'activation-standing',
      recordRevision: 5,
      activatedAtNs: 200,
      definitionId: 'manyforge.test.standing',
      lifecycleState: 'active_acknowledged',
      acknowledged: true,
      priority: 'medium',
      title: 'Reduced capability',
      requiresMotionAttention: false,
    };
    const snapshot: ComposerHeadlessState = {
      schemaVersion: 'manyforge.composer_headless_state.v1',
      complete: true,
      authoringAdmission: { authoringAllowed: true, admissionGeneration: 1 },
      programCycle: {
        programRunnerOwnerEpoch: 'runner.epoch.1',
        cycleGeneration: 1,
        state: 'idle_after_reset',
        programRunId: null,
        cycleClaimGeneration: null,
        terminalOrSettlementReference: null,
        runtimeControlRevision: null,
        recoveryOffer: null,
      },
      draftIdentity: { draftId: 'draft-1', draftRevision: 1 },
      receiverEpoch: 'receiver.epoch.1',
      snapshotSequence: 1,
      runtime: {
        schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
        complete: true,
        projectionGeneration: 7,
        projectionOwnerEpoch: 'projection-epoch',
        projectionOwnerId: 'headless-api',
        reasons: [],
        unavailableSections: [],
        sections: {
          manager: source(),
          logicalScene: source(),
          sceneRuntime: source(),
        },
      },
      alarms: {
        schemaVersion: 'manyforge.operator_alarm_snapshot.v2',
        ownerId: 'alarm-owner',
        ownerEpoch: 'alarm-epoch',
        generation: 9,
        stateRevision: 11,
        reconciled: true,
        healthState: 'available',
        liveCount: 2,
        unacknowledgedCount: 1,
        activeCount: 2,
        activeAcknowledgedCount: 1,
        motionAttentionCount: 1,
        unacknowledgedMembership: [{
          conditionId: attentionAlarm.conditionId,
          activationId: attentionAlarm.activationId,
          recordRevision: attentionAlarm.recordRevision,
        }],
        alarms: [attentionAlarm, standingAlarm],
        truncated: false,
      },
    };
    const reconnect = vi.fn();

    const html = renderToStaticMarkup(createElement(HeadlessRuntimeSurfacesView, {
      enabled: true,
      headlessState: {
        snapshot,
        error: 'Headless projection disconnected.',
        reconnect,
      },
    }));

    expect(html).toContain('Planner unavailable');
    expect(html).toContain('1 acknowledgement required');
    expect(html).toContain('ACTIVE — ACKNOWLEDGED · Reduced capability');
    expect(html).toContain('Messages / history');
    expect(html).toContain('Reconnect headless projection');
    expect(reconnect).not.toHaveBeenCalled();
  });
});
