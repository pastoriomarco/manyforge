/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { OperatorAlarm, OperatorAlarmLifecycleState } from '../types/api';
import { HeaderBar } from './HeaderBar';

const operatorAlarmState = vi.hoisted(() => ({
  alarms: [] as OperatorAlarm[],
  attentionAlarms: [] as OperatorAlarm[],
  standingAlarms: [] as OperatorAlarm[],
  liveCount: 0,
  unacknowledgedCount: 0,
  motionAttentionCount: 0,
  activeCount: 0,
  activeAcknowledgedCount: 0,
  unacknowledgedMembership: [] as Array<{
    conditionId: string;
    activationId: string;
    recordRevision: number;
  }>,
  generation: 0,
  stateRevision: 0,
  timestampNs: 0,
  truncated: false,
  reconciled: true,
  health: { state: 'available' as const, details: {} },
  domains: {},
  loaded: false,
  busy: false,
  error: null as string | null,
  acknowledge: vi.fn(),
  acknowledgeAll: vi.fn(),
  refresh: vi.fn(),
}));

const cycleState = vi.hoisted(() => ({
  state: 'idle',
  error: 'Duplicated cycle wrapper',
  errorCode: 'cycle_error',
  errorDetails: {} as Record<string, unknown>,
}));

vi.mock('../store/operatorAlarmStore', () => ({
  acknowledgementFence: (generation: number, members: unknown[]) => ({ generation, members }),
  acknowledgementFenceMatches: () => true,
  hasNewAlarmActivation: (previous: string[], current: string[]) => (
    current.some((value) => !previous.includes(value))
  ),
  useOperatorAlarmStore: () => operatorAlarmState,
  useOperatorAlarmPolling: () => undefined,
}));

vi.mock('../store/cycleStore', () => ({
  useCycleStore: (selector?: (state: typeof cycleState) => unknown) => (
    selector ? selector(cycleState) : cycleState
  ),
  useEditLocked: () => false,
}));

function alarm(
  index: number,
  lifecycleState: OperatorAlarmLifecycleState = 'active_unacknowledged',
): OperatorAlarm {
  const active = lifecycleState.startsWith('active');
  const acknowledged = lifecycleState === 'active_acknowledged'
    || lifecycleState === 'returned_acknowledged';
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
    title: `Server alarm ${index}`,
    message: `Alarm details ${index}`,
    consequence: 'Motion remains unavailable.',
    operatorResponse: 'Inspect the cause.',
    requiresMotionAttention: true,
    lifecycleState,
    active,
    acknowledged,
    activationCount: 1,
    occurrenceCount: 1,
    recordRevision: index,
    activatedAtNs: index,
    activatedAt: '2026-07-21T00:00:00Z',
    firstSeenAtNs: index,
    firstSeenAt: '2026-07-21T00:00:00Z',
    lastSeenAtNs: index,
    lastSeenAt: '2026-07-21T00:00:00Z',
    returnedAtNs: active ? null : index + 10,
    returnedAt: active ? null : '2026-07-21T00:00:10Z',
    acknowledgedAtNs: acknowledged ? index + 20 : null,
    acknowledgedAt: acknowledged ? '2026-07-21T00:00:20Z' : null,
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

describe('HeaderBar operator alarm stack', () => {
  it('does not render alarm acknowledgement controls with no attention alarms', () => {
    Object.assign(operatorAlarmState, {
      alarms: [],
      attentionAlarms: [],
      standingAlarms: [],
      liveCount: 0,
      unacknowledgedCount: 0,
      motionAttentionCount: 0,
      activeCount: 0,
      activeAcknowledgedCount: 0,
      unacknowledgedMembership: [],
      generation: 1,
      stateRevision: 1,
      timestampNs: 1,
      truncated: false,
      loaded: true,
    });

    const html = renderToStaticMarkup(createElement(HeaderBar));
    expect(html).not.toContain('Acknowledge alarm');
    expect(html).not.toContain('Acknowledge all');
  });

  it('shows only individual acknowledgement for one attention alarm', () => {
    const attention = alarm(1);
    Object.assign(operatorAlarmState, {
      alarms: [attention],
      attentionAlarms: [attention],
      standingAlarms: [],
      liveCount: 1,
      unacknowledgedCount: 1,
      motionAttentionCount: 1,
      activeCount: 1,
      activeAcknowledgedCount: 0,
      unacknowledgedMembership: [{
        conditionId: attention.conditionId,
        activationId: attention.activationId,
        recordRevision: attention.recordRevision,
      }],
      generation: 2,
      stateRevision: 2,
      timestampNs: 2,
      truncated: false,
      loaded: true,
    });

    const html = renderToStaticMarkup(createElement(HeaderBar));
    expect(html).toContain('Acknowledge alarm');
    expect(html).not.toContain('Acknowledge all');
    expect(html).not.toContain('Showing newest 1');
  });

  it('shows bulk acknowledgement for two attention alarms', () => {
    const attention = [alarm(2), alarm(1)];
    Object.assign(operatorAlarmState, {
      alarms: attention,
      attentionAlarms: attention,
      standingAlarms: [],
      liveCount: 2,
      unacknowledgedCount: 2,
      motionAttentionCount: 2,
      activeCount: 2,
      activeAcknowledgedCount: 0,
      unacknowledgedMembership: attention.map((item) => ({
        conditionId: item.conditionId,
        activationId: item.activationId,
        recordRevision: item.recordRevision,
      })),
      generation: 3,
      stateRevision: 3,
      timestampNs: 3,
      truncated: false,
      loaded: true,
    });

    const html = renderToStaticMarkup(createElement(HeaderBar));
    expect(html).toContain('2 alarms requiring acknowledgement');
    expect(html).toContain('Acknowledge all');
  });

  it('shows the newest three attention panels and a separate standing alarm', () => {
    const attention = [
      alarm(4),
      alarm(3, 'returned_unacknowledged'),
      alarm(2),
    ];
    const standing = alarm(5, 'active_acknowledged');
    Object.assign(operatorAlarmState, {
      alarms: [...attention, standing],
      attentionAlarms: attention,
      standingAlarms: [standing],
      liveCount: 5,
      unacknowledgedCount: 4,
      motionAttentionCount: 4,
      activeCount: 4,
      activeAcknowledgedCount: 1,
      unacknowledgedMembership: [4, 3, 2, 1].map((index) => ({
        conditionId: `condition-${index}`,
        activationId: `activation-${index}`,
        recordRevision: index,
      })),
      generation: 4,
      stateRevision: 8,
      timestampNs: 4,
      truncated: true,
      loaded: true,
    });

    const html = renderToStaticMarkup(createElement(HeaderBar));
    expect(html).toContain('4 alarms requiring acknowledgement');
    expect(html).toContain('Showing newest 3');
    expect(html).toContain('Acknowledge all');
    expect(html).toContain('ACTIVE — ACKNOWLEDGEMENT REQUIRED');
    expect(html).toContain('RESOLVED — ACKNOWLEDGEMENT REQUIRED');
    expect(html).toContain('Server alarm 4');
    expect(html).toContain('Server alarm 3');
    expect(html).toContain('Server alarm 2');
    expect(html).not.toContain('Server alarm 1');
    expect(html).toContain('ACTIVE — ACKNOWLEDGED · HIGH · Server alarm 5');
    expect(html).not.toContain('Duplicated cycle wrapper');
  });
});
