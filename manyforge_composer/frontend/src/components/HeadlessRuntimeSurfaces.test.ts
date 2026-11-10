import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import {
  HeadlessRuntimeSurfacesView,
  attentionAlarmsNewestFirst,
  errorAlarms,
  historianEventView,
  operatorAlarmView,
  partitionRepresentedAlarms,
  redAcknowledgementMembers,
  runtimeAttentionSummary,
  runtimeErrorItemCount,
  warningAlarms,
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
    // Unresolved conditions lead the summary and count, but never enter the
    // acknowledgement fence (see the rendered rail below).
    expect(runtimeAttentionSummary([], [{
      id: 'recovery.cell', severity: 'error', domain: 'recovery',
      title: 'Cell recovery required', message: 'physical rest unproven.',
    }])).toBe('Cell recovery required · physical rest unproven.');
  });

  it('shows a lifecycle-fault alarm inside the incident item only for that component', () => {
    const lifecycle = (subjectId: string): OperatorAlarmRecord => ({
      conditionId: `condition-${subjectId}`, activationId: `activation-${subjectId}`,
      recordRevision: 1, activatedAtNs: 100,
      definitionId: 'manyforge.runtime.component_lifecycle_fault',
      lifecycleState: 'active_unacknowledged', active: true, acknowledged: false,
      priority: 'high', alarmClass: 'runtime_health', title: 'Controlled-component lifecycle fault',
      message: `Lifecycle fault on ${subjectId}`, consequence: '', operatorResponse: '',
      occurrenceCount: 1, acknowledgementRequired: true, requiresMotionAttention: true,
      subject: { subjectKind: 'controllable_component', subjectId, resourceIds: [] }, details: {},
    });
    const incident = {
      id: 'recovery.cell', severity: 'error' as const, domain: 'recovery' as const,
      title: 'Open incident on lite6_robot', message: 'Press Recover cell.',
      alarmDefinitionIds: ['manyforge.motion.stop_fault'],
      alarmSubjects: [{ definitionId: 'manyforge.runtime.component_lifecycle_fault', subjectId: 'lite6_robot' }],
    };
    const { standalone, represented } = partitionRepresentedAlarms(
      [lifecycle('lite6_robot'), lifecycle('gripper')], [incident],
    );
    expect(represented.get('recovery.cell')?.map((alarm) => alarm.subject.subjectId)).toEqual(['lite6_robot']);
    expect(standalone.map((alarm) => alarm.subject.subjectId)).toEqual(['gripper']);
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
      acknowledgementRequired: true,
      requiresMotionAttention: true,
      subject: { subjectKind: 'cell', subjectId: 'cell-1', resourceIds: [] },
      details: {},
    };

    const view = operatorAlarmView(alarm);

    expect(view.lifecycleLabel).toBe('Resolved');
    expect(view.severity).toBe('critical');
    expect(view.details.occurrence_count).toBe(2);
  });

  it('labels the state Active or Resolved and never adds a motion-gating message', () => {
    const alarm: OperatorAlarmRecord = {
      conditionId: 'condition-scope',
      activationId: 'activation-scope',
      recordRevision: 1,
      activatedAtNs: 100,
      definitionId: 'manyforge.runtime.alarm_scope_unavailable',
      lifecycleState: 'active_acknowledged',
      active: true,
      acknowledged: true,
      priority: 'high',
      alarmClass: 'runtime_health',
      title: 'Fault alarm scope unavailable',
      message: 'A current fault could not be raised as its own alarm.',
      consequence: 'Motion is blocked while it is active.',
      operatorResponse: 'Resolve the refused faults.',
      occurrenceCount: 1,
      acknowledgementRequired: false,
      requiresMotionAttention: true,
      subject: { subjectKind: 'cell', subjectId: 'cell-1', resourceIds: [] },
      details: {},
    };

    expect(operatorAlarmView(alarm).lifecycleLabel).toBe('Active · Acknowledged');
    expect(operatorAlarmView(alarm).operatorResponse).toBe('Resolve the refused faults.');
    expect(operatorAlarmView({ ...alarm, lifecycleState: 'active_unacknowledged' }).lifecycleLabel)
      .toBe('Active');
    expect(operatorAlarmView({ ...alarm, lifecycleState: 'returned_acknowledged', active: false })
      .lifecycleLabel).toBe('Resolved · Acknowledged');
    // Colour is severity: high/critical are red errors, the rest yellow warnings.
    expect(operatorAlarmView(alarm).severity).toBe('error');
    expect(operatorAlarmView({ ...alarm, priority: 'medium' }).severity).toBe('warning');
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
      acknowledgementRequired: true,
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
      acknowledgementRequired: true,
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
      acknowledgementRequired: true,
      requiresMotionAttention: false,
    };
    const snapshot: ComposerHeadlessState = {
      schemaVersion: 'manyforge.composer_headless_state.v2',
      complete: true,
      executionControlStatus: null,
      componentOverview: {
        schemaVersion: 'manyforge.component_overview.v2',
        overviewRevision: `sha256:${'a'.repeat(64)}`,
        readiness: 'ready',
        summaryLabel: 'Application surfaces ready',
        profileLabel: 'standard',
        authorityLabel: 'owner-issued scoped facts',
        userMessage: 'Current.',
        capabilities: [],
        components: [],
        maintenanceOperation: null,
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
      heldRunDraftEditAllowed: false,
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
        resetPhase: null, resetOptions: null,
        resetRobotPosition: { available: false, reason: 'unsupported' },
        allowedCommands: [],
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
      authoringWorkspaceContainment: null,
      receiverEpoch: 'receiver.epoch.1',
      snapshotSequence: 1,
      runtime: {
        schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
        activeConfiguration: null,
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
        motionAttentionMembership: [{
          conditionId: attentionAlarm.conditionId,
          activationId: attentionAlarm.activationId,
          recordRevision: attentionAlarm.recordRevision,
          lifecycleState: attentionAlarm.lifecycleState,
          definitionId: attentionAlarm.definitionId,
          subject: attentionAlarm.subject,
        }],
        unacknowledgedMembership: [{
          conditionId: attentionAlarm.conditionId,
          activationId: attentionAlarm.activationId,
          recordRevision: attentionAlarm.recordRevision,
        }],
        alarms: [attentionAlarm, standingAlarm],
        truncated: false,
        kernelAttention: null,
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
    }));

    // Composer request failures are warnings now: never in the red row.
    expect(html).toContain('Planner unavailable');
    expect(html).toContain('Attention 1');
    expect(html).toContain('Active 1');
    expect(html).toContain('>History</button>');
    expect(html).not.toContain('Reduced capability · Active · Acknowledged');
    expect(html).toContain('Reconnect headless projection');
    expect(html).not.toContain('Composer error');
    expect(html).not.toContain('Acknowledge all');
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
      expanded: false,
      onExpandedChange: vi.fn(),
      onClose: vi.fn(),
    }));
    expect(collapsed).toContain('Planner unavailable');
    expect(collapsed).toContain('>Acknowledge</button>');

    // An unresolved condition is shown and counted with its source action,
    // but it is never acknowledged/dismissed and never enters the fence.
    const recover = vi.fn();
    const withCondition = renderToStaticMarkup(createElement(HeadlessRuntimeSurfacesView, {
      enabled: true,
      headlessState: {
        snapshot: {
          ...snapshot,
          alarms: {
            ...snapshot.alarms,
            alarms: [],
            unacknowledgedCount: 0,
            unacknowledgedMembership: [],
          },
        },
        error: null,
        transportStatus: 'connected',
        transportError: null,
        semanticStatus: 'current',
        semanticReason: null,
        reconnect,
      },
      conditions: [{
        id: 'recovery.cell', severity: 'error', domain: 'recovery',
        title: 'Cell recovery required', message: 'physical rest unproven.',
        actions: [{ id: 'recover-cell', label: 'Retry recovery', run: recover }],
      }],
      expanded: false,
      onExpandedChange: vi.fn(),
    }));
    expect(withCondition).toContain('Cell recovery required · physical rest unproven.');
    expect(withCondition).toContain('>Retry recovery</button>');
    expect(withCondition).not.toMatch(/>(Acknowledge|Dismiss|Acknowledge all)</);
    expect(withCondition).toContain('attention-error');
    expect(collapsed).not.toContain('Runtime observability sections');

    const previousAlarm: OperatorAlarmRecord = {
      ...attentionAlarm,
      definitionId: 'manyforge.runtime.previous_session_unclean',
      lifecycleState: 'returned_unacknowledged', active: false,
      priority: 'medium', requiresMotionAttention: false,
      title: 'ManyForge did not close correctly last time',
      message: 'ManyForge did not close correctly last time. The cell started empty; nothing from the previous session was resumed. Its records are kept; details in History.',
      details: { previous_session: { session_id: 'a'.repeat(32), journal_families: ['program_cycle'], history_coverage: 'not_imported' }, failure_time_status: 'unavailable' },
    };
    const previousState = {
      snapshot: { ...snapshot, alarms: { ...snapshot.alarms, alarms: [previousAlarm],
        unacknowledgedCount: 1, liveCount: 1, motionAttentionCount: 0,
        motionAttentionMembership: [], unacknowledgedMembership: [],
      } },
      error: null, transportStatus: 'connected' as const, transportError: null,
      semanticStatus: 'current' as const, semanticReason: null, reconnect,
    };
    // A warning (yellow) is never in the red row.
    expect(warningAlarms(previousState.snapshot.alarms)).toEqual([previousAlarm]);
    expect(errorAlarms(previousState.snapshot.alarms)).toEqual([]);
    expect(runtimeErrorItemCount(previousState.snapshot.alarms, [])).toBe(0);
    const previous = renderToStaticMarkup(createElement(HeadlessRuntimeSurfacesView, {
      enabled: true, headlessState: previousState, expanded: false,
    }));
    expect(previous).not.toContain('has-attention');
    expect(previous).not.toContain(previousAlarm.message);

    // One item per failure: the Program recovery notice represents the
    // operation-failure alarm, shows it inside with its exact Acknowledge
    // and counts once; without the notice the alarm stands alone.
    const failureAlarm: OperatorAlarmRecord = {
      ...attentionAlarm,
      definitionId: 'manyforge.program.operation_failure',
      title: 'Program operation failed',
      message: 'Program operation failed',
      details: {
        failure: { code: 'ik_failed', message: 'RoboPlan could not resolve the pose goal' },
        node_instance_id: 'unreachable_hold',
      },
    };
    const failureState = {
      ...previousState,
      snapshot: { ...snapshot, alarms: { ...snapshot.alarms, alarms: [failureAlarm] } },
    };
    const programRecovery = {
      id: 'recovery.program', severity: 'error' as const, domain: 'recovery' as const,
      title: 'Program recovery required', message: 'Node unreachable_hold failed.',
      alarmDefinitionIds: ['manyforge.program.operation_failure'],
    };
    expect(runtimeAttentionSummary([failureAlarm], [programRecovery]))
      .toBe('Program recovery required · RoboPlan could not resolve the pose goal');
    const merged = renderToStaticMarkup(createElement(HeadlessRuntimeSurfacesView, {
      enabled: true, headlessState: failureState, conditions: [programRecovery], expanded: true,
    }));
    expect(merged).toContain('Program recovery required · RoboPlan could not resolve the pose goal');
    expect(merged).toContain('Attention 1');
    expect(merged).toMatch(
      /data-notice-id="recovery.program".*<div class="workbench-notice-alarms"><div class="operator-alarm-panel">.*>Acknowledge alarm<\/button><\/div><\/div><\/div><\/div>/,
    );
    expect(merged.match(/class="operator-alarm-panel"/g)).toHaveLength(1);
    const alone = renderToStaticMarkup(createElement(HeadlessRuntimeSurfacesView, {
      enabled: true, headlessState: failureState, expanded: true,
    }));
    expect(alone).toContain('Attention 1');
    expect(alone).not.toContain('workbench-notice-alarms');
    expect(alone).toContain('<summary>RoboPlan could not resolve the pose goal · Program operation failed · node unreachable_hold');

    // An error stays red until acknowledged, Active or Resolved; once
    // acknowledged it leaves the red row (History keeps it). Acknowledgement
    // never gates anything.
    const resolved = { ...attentionAlarm, lifecycleState: 'returned_unacknowledged' as const, active: false };
    expect(errorAlarms({ ...snapshot.alarms, alarms: [resolved] })).toEqual([resolved]);
    const acknowledged: OperatorAlarmRecord = {
      ...attentionAlarm, lifecycleState: 'active_acknowledged', acknowledged: true,
    };
    const acknowledgedAlarms = { ...snapshot.alarms, alarms: [acknowledged] };
    expect(errorAlarms(acknowledgedAlarms)).toEqual([]);
    expect(runtimeErrorItemCount(acknowledgedAlarms, [])).toBe(0);
    // Red "Acknowledge all" names exactly the red row's alarms, by exact revision.
    expect(redAcknowledgementMembers({
      ...snapshot.alarms,
      alarms: [attentionAlarm, { ...previousAlarm, activationId: 'activation-previous' }, acknowledged],
    })).toEqual([{ conditionId: 'condition-attention', activationId: 'activation-attention', recordRevision: 3 }]);
  });
});
