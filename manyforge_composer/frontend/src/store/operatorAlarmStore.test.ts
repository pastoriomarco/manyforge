import { beforeEach, describe, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import type {
  OperatorAlarm,
  OperatorAlarmAcknowledgeResponse,
  OperatorAlarmLifecycleState,
  OperatorAlarmSnapshot,
} from '../types/api';
import {
  acknowledgementFence,
  acknowledgementFenceMatches,
  hasNewAlarmActivation,
  operatorAttentionBlocksMotion,
  useOperatorAlarmStore,
} from './operatorAlarmStore';

vi.mock('../api/client', () => ({
  api: {
    execution: {
      alarms: vi.fn(),
      acknowledgeAlarm: vi.fn(),
      acknowledgeAllAlarms: vi.fn(),
    },
  },
}));

function alarm(
  index: number,
  lifecycleState: OperatorAlarmLifecycleState = 'active_unacknowledged',
): OperatorAlarm {
  const active = lifecycleState.startsWith('active');
  const acknowledged = lifecycleState.endsWith('acknowledged')
    && !lifecycleState.endsWith('unacknowledged');
  return {
    schemaVersion: 'manyforge.operator_alarm.v2',
    catalogSchemaVersion: 'manyforge.operator_alarm_catalog.v1',
    catalogVersion: '1.0.0',
    definitionVersion: 1,
    definitionId: `manyforge.test.alarm_${index}`,
    conditionId: `condition-${index}`,
    activationId: `activation-${index}`,
    scope: { resource: `resource-${index}` },
    alarmClass: 'motion',
    priority: index === 4 ? 'critical' : 'high',
    title: `Test alarm ${index}`,
    message: `Alarm message ${index}`,
    consequence: 'Motion remains unavailable.',
    operatorResponse: 'Inspect and repair the cause.',
    requiresMotionAttention: true,
    lifecycleState,
    active,
    acknowledged,
    activationCount: 1,
    occurrenceCount: 1,
    recordRevision: index,
    activatedAtNs: index,
    activatedAt: `2026-07-21T00:00:0${index}Z`,
    firstSeenAtNs: index,
    firstSeenAt: `2026-07-21T00:00:0${index}Z`,
    lastSeenAtNs: index,
    lastSeenAt: `2026-07-21T00:00:0${index}Z`,
    returnedAtNs: active ? null : index + 10,
    returnedAt: active ? null : `2026-07-21T00:00:1${index}Z`,
    acknowledgedAtNs: acknowledged ? index + 20 : null,
    acknowledgedAt: acknowledged ? `2026-07-21T00:00:2${index}Z` : null,
    acknowledgementPrincipal: acknowledged ? 'operator_ui' : null,
    acknowledgementReason: null,
    sourceId: 'test-source',
    sourceEpoch: 'epoch-1',
    sourceSequence: index,
    sourceQuality: 'good',
    displayArgs: {},
    details: {},
    evidenceRefs: [],
    correlations: {},
  };
}

function snapshot(generation: number, alarms: OperatorAlarm[]): OperatorAlarmSnapshot {
  const attention = alarms.filter((item) => (
    item.lifecycleState === 'active_unacknowledged'
    || item.lifecycleState === 'returned_unacknowledged'
  ));
  return {
    schemaVersion: 'manyforge.operator_alarm_snapshot.v2',
    catalogVersion: '1.0.0',
    generation,
    stateRevision: generation,
    reconciled: true,
    health: { state: 'available', details: {} },
    domains: {
      motion: { required: true, reconciled: true, sourceCount: 1, unknownSourceCount: 0 },
    },
    liveCount: alarms.length,
    unacknowledgedCount: attention.length,
    motionAttentionCount: attention.filter((item) => item.requiresMotionAttention).length,
    activeCount: alarms.filter((item) => item.active).length,
    activeAcknowledgedCount: alarms.filter(
      (item) => item.lifecycleState === 'active_acknowledged',
    ).length,
    unacknowledgedMembership: attention.map((item) => ({
      conditionId: item.conditionId,
      activationId: item.activationId,
      recordRevision: item.recordRevision,
    })),
    countsByState: {},
    countsByPriority: {},
    countsByClass: {},
    alarms,
    truncated: false,
    timestampNs: generation * 100,
  };
}

function response(next: OperatorAlarmSnapshot): OperatorAlarmAcknowledgeResponse {
  return {
    status: 'acknowledged',
    changed: true,
    acknowledgedConditionIds: [],
    auditEventIndices: [],
    snapshot: next,
  };
}

beforeEach(() => {
  vi.clearAllMocks();
  useOperatorAlarmStore.setState({
    alarms: [],
    attentionAlarms: [],
    standingAlarms: [],
    liveCount: 0,
    unacknowledgedCount: 0,
    motionAttentionCount: 0,
    activeCount: 0,
    activeAcknowledgedCount: 0,
    unacknowledgedMembership: [],
    generation: 0,
    stateRevision: 0,
    timestampNs: 0,
    truncated: false,
    reconciled: false,
    health: { state: 'unavailable', details: {} },
    domains: {},
    loaded: false,
    busy: false,
    error: null,
  });
});

describe('operator alarm store', () => {
  it('reannunciates only a genuinely new activation, not repeated samples', () => {
    const previous = ['condition-1:activation-1'];
    expect(hasNewAlarmActivation(previous, ['condition-1:activation-1'])).toBe(false);
    expect(hasNewAlarmActivation(previous, ['condition-1:activation-2'])).toBe(true);
    expect(hasNewAlarmActivation(previous, [
      'condition-1:activation-1',
      'condition-2:activation-1',
    ])).toBe(true);
  });

  it('detects membership, recurrence, revision, and generation changes in a bulk fence', () => {
    const members = snapshot(3, [alarm(3), alarm(2)]).unacknowledgedMembership;
    const fence = acknowledgementFence(3, members);
    expect(acknowledgementFenceMatches(fence, 3, [...members].reverse())).toBe(true);
    expect(acknowledgementFenceMatches(fence, 4, members)).toBe(false);
    expect(acknowledgementFenceMatches(fence, 3, [
      { ...members[0], recordRevision: members[0].recordRevision + 1 },
      members[1],
    ])).toBe(false);
    expect(acknowledgementFenceMatches(fence, 3, [
      { ...members[0], activationId: 'activation-recurrence' },
      members[1],
    ])).toBe(false);
  });

  it('fails closed only for motion-producing controls', () => {
    expect(operatorAttentionBlocksMotion(true, false, null, false, 'unavailable', 0)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, 'offline', true, 'available', 0)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, null, false, 'available', 0)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, null, true, 'unavailable', 0)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, null, true, 'available', 1)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, null, true, 'available', 0)).toBe(false);
    expect(operatorAttentionBlocksMotion(false, false, 'offline', false, 'unavailable', 3)).toBe(false);
  });

  it('derives attention and standing records while retaining server totals', async () => {
    const next = snapshot(4, [
      alarm(4),
      alarm(3, 'returned_unacknowledged'),
      alarm(2, 'active_acknowledged'),
      alarm(1, 'active_acknowledged'),
    ]);
    vi.mocked(api.execution.alarms).mockResolvedValue(next);

    await useOperatorAlarmStore.getState().refresh();
    const state = useOperatorAlarmStore.getState();
    expect(state.unacknowledgedCount).toBe(2);
    expect(state.attentionAlarms.map((item) => item.conditionId)).toEqual([
      'condition-4', 'condition-3',
    ]);
    expect(state.standingAlarms.map((item) => item.conditionId)).toEqual([
      'condition-2', 'condition-1',
    ]);
    expect(api.execution.alarms).toHaveBeenCalledWith(1000);
  });

  it('binds individual acknowledgement to exact activation and revision', async () => {
    const currentAlarm = alarm(4);
    useOperatorAlarmStore.setState({
      ...snapshot(4, [currentAlarm]),
      attentionAlarms: [currentAlarm],
      standingAlarms: [],
      loaded: true,
    });
    const acknowledged = { ...currentAlarm, lifecycleState: 'active_acknowledged' as const, acknowledged: true };
    vi.mocked(api.execution.acknowledgeAlarm).mockResolvedValue(
      response(snapshot(5, [acknowledged])),
    );

    await useOperatorAlarmStore.getState().acknowledge(
      currentAlarm.conditionId,
      currentAlarm.activationId,
      currentAlarm.recordRevision,
    );
    expect(api.execution.acknowledgeAlarm).toHaveBeenCalledWith(
      'condition-4', 'activation-4', 4, true,
    );
    expect(useOperatorAlarmStore.getState().standingAlarms).toHaveLength(1);
  });

  it('binds acknowledge-all to the confirmation-time membership and generation', async () => {
    const current = snapshot(7, [alarm(3), alarm(2), alarm(1)]);
    useOperatorAlarmStore.setState({
      ...current,
      attentionAlarms: current.alarms,
      standingAlarms: [],
      loaded: true,
    });
    const fence = acknowledgementFence(current.generation, current.unacknowledgedMembership);
    vi.mocked(api.execution.acknowledgeAllAlarms).mockResolvedValue(
      response(snapshot(8, [])),
    );

    await useOperatorAlarmStore.getState().acknowledgeAll(fence);
    expect(api.execution.acknowledgeAllAlarms).toHaveBeenCalledWith(
      current.unacknowledgedMembership,
      7,
      true,
    );
    expect(useOperatorAlarmStore.getState().unacknowledgedCount).toBe(0);
  });

  it('accepts a later observation from a restarted process with a reset generation', async () => {
    const old = snapshot(9, [alarm(9)]);
    useOperatorAlarmStore.setState({
      ...old,
      attentionAlarms: old.alarms,
      standingAlarms: [],
      timestampNs: 900,
      loaded: true,
    });
    vi.mocked(api.execution.alarms).mockResolvedValue({
      ...snapshot(0, []),
      timestampNs: 1000,
    });

    await useOperatorAlarmStore.getState().refresh();
    expect(useOperatorAlarmStore.getState().generation).toBe(0);
    expect(useOperatorAlarmStore.getState().unacknowledgedCount).toBe(0);
  });

  it('does not let a delayed acknowledgement overwrite a newer poll', async () => {
    const current = snapshot(4, [alarm(4), alarm(3)]);
    useOperatorAlarmStore.setState({
      ...current,
      attentionAlarms: current.alarms,
      standingAlarms: [],
      loaded: true,
    });
    let resolveAcknowledgement!: (value: OperatorAlarmAcknowledgeResponse) => void;
    vi.mocked(api.execution.acknowledgeAlarm).mockImplementation(
      () => new Promise((resolve) => { resolveAcknowledgement = resolve; }),
    );

    const acknowledgement = useOperatorAlarmStore.getState().acknowledge(
      'condition-4', 'activation-4', 4,
    );
    const newer = snapshot(6, [alarm(6)]);
    useOperatorAlarmStore.setState({
      ...newer,
      attentionAlarms: newer.alarms,
      standingAlarms: [],
      loaded: true,
    });
    resolveAcknowledgement(response(snapshot(5, [alarm(3)])));
    await acknowledgement;

    expect(useOperatorAlarmStore.getState().generation).toBe(6);
    expect(useOperatorAlarmStore.getState().attentionAlarms[0].conditionId).toBe('condition-6');
  });

  it('refreshes state after a stale bulk acknowledgement', async () => {
    const current = snapshot(7, [alarm(2), alarm(1)]);
    useOperatorAlarmStore.setState({
      ...current,
      attentionAlarms: current.alarms,
      standingAlarms: [],
      loaded: true,
    });
    vi.mocked(api.execution.acknowledgeAllAlarms).mockRejectedValue(
      new Error('Alarm membership changed'),
    );
    vi.mocked(api.execution.alarms).mockResolvedValue(
      snapshot(8, [alarm(3), alarm(2), alarm(1)]),
    );

    await expect(useOperatorAlarmStore.getState().acknowledgeAll(
      acknowledgementFence(current.generation, current.unacknowledgedMembership),
    )).rejects.toThrow('Alarm membership changed');
    const state = useOperatorAlarmStore.getState();
    expect(state.generation).toBe(8);
    expect(state.unacknowledgedCount).toBe(3);
    expect(state.error).toBe('Alarm membership changed');
  });
});
