import { describe, expect, it } from 'vitest';

import type { ComposerHeadlessState } from '../types/headless';
import { admitHeadlessState } from './useHeadlessState';

function state(
  runtimeEpoch: string,
  runtimeGeneration: number,
  alarmEpoch = 'alarms.epoch.1',
  alarmGeneration = 3,
): ComposerHeadlessState {
  return {
    schemaVersion: 'manyforge.composer_headless_state.v1',
    runtime: {
      schemaVersion: 'manyforge.headless_runtime_snapshot.v1',
      complete: true,
      projectionGeneration: runtimeGeneration,
      projectionOwnerEpoch: runtimeEpoch,
      projectionOwnerId: 'headless.owner',
      reasons: [],
      unavailableSections: [],
      sections: {
        manager: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'manager.epoch.1',
          sourceOwnerId: 'manager.owner',
          receiverAgeNs: 1,
        },
        logicalScene: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'scene.epoch.1',
          sourceOwnerId: 'scene.owner',
          receiverAgeNs: 1,
        },
        sceneRuntime: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          payload: {},
          sourceGeneration: 1,
          sourceOwnerEpoch: 'runtime.epoch.1',
          sourceOwnerId: 'runtime.owner',
          receiverAgeNs: 1,
        },
      },
    },
    alarms: {
      schemaVersion: 'manyforge.operator_alarm_snapshot.v2',
      ownerId: 'alarm.owner',
      ownerEpoch: alarmEpoch,
      generation: alarmGeneration,
      stateRevision: alarmGeneration,
      reconciled: true,
      healthState: 'healthy',
      liveCount: 0,
      unacknowledgedCount: 0,
      activeCount: 0,
      activeAcknowledgedCount: 0,
      motionAttentionCount: 0,
      unacknowledgedMembership: [],
      alarms: [],
      truncated: false,
    },
  };
}

describe('Headless owner-frame admission', () => {
  it('accepts monotonic generations and replacement epochs', () => {
    expect(admitHeadlessState(null, state('runtime.epoch.1', 2)).accepted).toBe(true);
    expect(admitHeadlessState(
      state('runtime.epoch.1', 2),
      state('runtime.epoch.1', 3),
    ).accepted).toBe(true);
    expect(admitHeadlessState(
      state('runtime.epoch.1', 20, 'alarms.epoch.1', 30),
      state('runtime.epoch.2', 0, 'alarms.epoch.2', 0),
    ).accepted).toBe(true);
  });

  it('rejects regressions and same-generation conflicts', () => {
    const current = state('runtime.epoch.1', 2);
    expect(admitHeadlessState(current, state('runtime.epoch.1', 1)).reason)
      .toContain('regressed');
    const conflict = state('runtime.epoch.1', 2);
    conflict.runtime.complete = false;
    expect(admitHeadlessState(current, conflict).reason).toContain('conflicted');
    expect(admitHeadlessState(
      state('runtime.epoch.1', 2, 'alarms.epoch.1', 3),
      state('runtime.epoch.1', 3, 'alarms.epoch.1', 2),
    ).reason).toContain('alarm projection generation regressed');
  });
});
