import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import {
  HeadlessRuntimeSurfacesView,
  attentionAlarmsNewestFirst,
  historianEventView,
  operatorAlarmView,
  runtimeAttentionSummary,
} from './HeadlessRuntimeSurfaces';
import type {
  ComposerHeadlessState,
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
  it('keeps automatic attention to one concise summary line', () => {
    expect(runtimeAttentionSummary([], [])).toBe('No messages require attention');
    expect(runtimeAttentionSummary([], [{
      id: 'composer.error.1',
      title: 'Program command failed',
      message: 'Exact response mismatch.',
      active: true,
      acknowledgementRequired: false,
    }])).toBe('Program command failed');
    expect(runtimeAttentionSummary([], [
      {
        id: 'composer.error.1', title: 'First', message: 'First detail',
        active: true, acknowledgementRequired: false,
      },
      {
        id: 'composer.error.2', title: 'Second', message: 'Second detail',
        active: false, acknowledgementRequired: true,
      },
    ])).toBe('2 messages require attention');
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
        schemaVersion: 'manyforge.historian_event_summary.v2',
        conditionSeverity: 'ERROR',
        clockOwner: 'headless:test-epoch',
        wallClockRegressed: false,
        occurredAtUtcNs: '1785931200000000000',
        eventId: 'event-11',
        component: 'manyforge.alarm',
        eventType: 'operator_alarm_acknowledged',
        severity: 'INFO',
        occurredAtNs: '100',
        occurredAtWall: '2026-08-05T12:00:00Z',
        message: 'operator alarm acknowledged',
        conditionId: 'condition-1',
        programRunId: 'program-run-1',
        operationId: 'operation-1',
      },
    };

    const view = historianEventView(stored);

    expect(view.alarmPhase).toBe('acknowledgement');
    expect(view.severity).toBe('ERROR');
    expect(view.message).toContain('condition-1');
    expect(view.programRunId).toBe('program-run-1');
    expect(view.operationId).toBe('operation-1');
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
      schemaVersion: 'manyforge.composer_headless_state.v2',
      complete: true,
      executionControlStatus: null,
      componentOverview: {
        schemaVersion: 'manyforge.component_overview.v1',
        overviewRevision: `sha256:${'a'.repeat(64)}`,
        readiness: 'ready',
        summaryLabel: 'Application surfaces ready',
        profileLabel: 'native',
        authorityLabel: 'owner-issued scoped facts',
        userMessage: 'Current.',
        capabilities: [],
        components: [],
        basis: {
          runtimeProjectionOwnerEpoch: null,
          runtimeProjectionGeneration: null,
          managerSourceOwnerEpoch: null,
          logicalSceneSourceOwnerEpoch: null,
          logicalSceneSourceGeneration: null,
          sceneRuntimeSourceOwnerEpoch: null,
          sceneRuntimeSourceGeneration: null,
          alarmOwnerEpoch: null,
          alarmGeneration: null,
          alarmStateRevision: null,
          admissionGeneration: null,
          programRunnerOwnerEpoch: null,
          programCycleGeneration: null,
          appliedProgramOwnerEpoch: null,
          appliedProgramGeneration: null,
          executionOwnerEpoch: null,
          executionPublicationGeneration: null,
        },
      },
      authoringAdmission: { authoringAllowed: true, admissionGeneration: 1 },
      programCycle: {
        programRunnerOwnerEpoch: 'runner.epoch.1',
        cycleGeneration: 1,
        state: 'idle_after_reset',
        programRunId: null,
        cycleClaimGeneration: null,
        terminalOrSettlementReference: null,
        runRevision: null,
        nodeStatusRevision: null,
        runtimeControlRevision: null,
        executionControlRevision: 1,
        causalCorrelationId: null,
        causalSessionId: null,
        causalClientSequence: null,
        currentCommandKind: null,
        failure: null,
        resetPhase: null,
        allowedCommands: [],
        recoveryOffer: null,
        run: null,
        runtimeControls: null,
      },
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1',
        workspaceRevision: 1,
        programDraftId: 'draft-1',
        programDraftRevision: 1,
        deploymentDraftId: 'deployment-draft-1',
        deploymentDraftRevision: 1,
      },
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
        transportStatus: 'disconnected',
        transportError: 'Headless projection disconnected.',
        semanticStatus: 'current',
        semanticReason: null,
        reconnect,
      },
      clientErrors: [{
        id: 'composer.program_runtime',
        title: 'Program command or response failed',
        message: 'Invalid response from /api/program/runs/program-run.1',
        active: false,
        acknowledgementRequired: true,
      }],
      onAcknowledgeClientError: vi.fn(),
    }));

    expect(html).toContain('Planner unavailable');
    expect(html).toContain('Attention 2');
    expect(html).toContain('Active 1');
    expect(html).toContain('>History</button>');
    expect(html).not.toContain('ACTIVE — ACKNOWLEDGED · Reduced capability');
    expect(html).toContain('Reconnect headless projection');
    expect(html).toContain('COMPOSER ERROR — RESOLVED · DISMISSAL REQUIRED');
    expect(html).toContain('Invalid response from /api/program/runs/program-run.1');
    expect(html).toContain('Acknowledge message');
    expect(html).toContain('Acknowledge all');
    expect(reconnect).not.toHaveBeenCalled();

    const collapsed = renderToStaticMarkup(createElement(HeadlessRuntimeSurfacesView, {
      enabled: true,
      headlessState: {
        snapshot,
        error: null,
        transportStatus: 'connected',
        transportError: null,
        semanticStatus: 'current',
        semanticReason: null,
        reconnect,
      },
      clientErrors: [{
        id: 'composer.program_runtime',
        title: 'Program command or response failed',
        message: 'Invalid response from /api/program/runs/program-run.1',
        active: false,
        acknowledgementRequired: true,
      }],
      expanded: false,
      onExpandedChange: vi.fn(),
      onClose: vi.fn(),
    }));
    expect(collapsed).toContain('2 messages require attention');
    expect(collapsed).toContain('Acknowledge all');
    expect(collapsed).not.toContain('Runtime observability sections');
    expect(collapsed).not.toContain('Planner unavailable');

    const singleClientFailure = renderToStaticMarkup(createElement(HeadlessRuntimeSurfacesView, {
      enabled: true,
      headlessState: {
        snapshot: {
          ...snapshot,
          alarms: {
            ...snapshot.alarms,
            liveCount: 1,
            unacknowledgedCount: 0,
            activeCount: 1,
            motionAttentionCount: 0,
            unacknowledgedMembership: [],
            alarms: [standingAlarm],
          },
        },
        error: null,
        transportStatus: 'connected',
        transportError: null,
        semanticStatus: 'current',
        semanticReason: null,
        reconnect,
      },
      clientErrors: [{
        id: 'composer.source_model',
        title: 'Viewer source-model response failed',
        message: 'Projection contract mismatch.',
        active: true,
        acknowledgementRequired: false,
      }],
      onAcknowledgeClientError: vi.fn(),
      expanded: false,
      onExpandedChange: vi.fn(),
    }));
    expect(singleClientFailure).toContain(
      'runtime-observability-surface collapsed has-attention attention-error',
    );
    expect(singleClientFailure).toContain('Viewer source-model response failed');
    expect(singleClientFailure).toContain('>Dismiss</button>');
    expect(singleClientFailure).not.toContain('Acknowledge all');
  });
});
