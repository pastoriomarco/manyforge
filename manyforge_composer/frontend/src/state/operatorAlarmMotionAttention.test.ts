import { describe, expect, it } from 'vitest';

import { ACTIVE_CONFIGURATION, alarmSnapshot, gatingMember } from '../testFixtures/operatorAlarms';
import type { CycleRecoveryOffer } from '../types/api';
import type { OperatorAlarmSnapshot } from '../types/headless';
import {
  MOTION_PRODUCING_RECOVERY_ACTIONS,
  motionAttentionBlock,
  recoveryOfferResourceIds,
} from './operatorAlarmMotionAttention';
import { operatorAlarmRefusalMessage } from './operatorAlarmRefusal';

const ARM = ['robot.joint_1', 'robot.joint_2'];

describe('operator-alarm motion attention (spec 450 §5.2)', () => {
  it('admits motion when no gating record exists or none intersects', () => {
    expect(motionAttentionBlock(alarmSnapshot([]), ARM, ACTIVE_CONFIGURATION)).toBeNull();
    const other = alarmSnapshot([
      gatingMember('other.robot', 'active_unacknowledged', ['robot.2.joint_1']),
    ]);
    // Disjoint operations continue independently.
    expect(motionAttentionBlock(other, ARM, ACTIVE_CONFIGURATION)).toBeNull();
  });

  it('refuses an intersecting acknowledgement-gated record with the acknowledge remedy', () => {
    for (const state of ['active_unacknowledged', 'returned_unacknowledged'] as const) {
      const block = motionAttentionBlock(
        alarmSnapshot([gatingMember('arm.collision', state, ['robot.joint_2'])]),
        ARM,
        ACTIVE_CONFIGURATION,
      );
      expect(block?.code).toBe('unacknowledged_operator_alarms');
      expect(block?.message).toBe(operatorAlarmRefusalMessage('unacknowledged_operator_alarms'));
      expect(block?.members.map((member) => member.conditionId)).toEqual(['arm.collision']);
    }
  });

  it('refuses every operation while a cell-global record gates', () => {
    const block = motionAttentionBlock(
      alarmSnapshot([gatingMember('provider', 'active_unacknowledged', [])]),
      [],
      ACTIVE_CONFIGURATION,
    );
    expect(block?.code).toBe('unacknowledged_operator_alarms');
  });

  it('names an interlock first, with both remedies when unacknowledged alarms also gate', () => {
    const interlock = gatingMember('scope', 'active_acknowledged', []);
    const only = motionAttentionBlock(alarmSnapshot([interlock]), ARM, ACTIVE_CONFIGURATION);
    expect(only?.code).toBe('operator_alarm_interlock_active');
    expect(only?.message).toBe(operatorAlarmRefusalMessage('operator_alarm_interlock_active'));
    const both = motionAttentionBlock(
      alarmSnapshot([interlock, gatingMember('arm', 'active_unacknowledged', ['robot.joint_1'])]),
      ARM,
      ACTIVE_CONFIGURATION,
    );
    expect(both?.code).toBe('operator_alarm_interlock_active');
    expect(both?.message).toMatch(/Resolve its cause.*Then acknowledge the unacknowledged alarms\./);
  });

  it('fails closed while the owner projection is unreconciled or absent', () => {
    expect(motionAttentionBlock(alarmSnapshot([], false), ARM, ACTIVE_CONFIGURATION)?.code)
      .toBe('operator_alarm_state_unavailable');
    expect(motionAttentionBlock(null, ARM, ACTIVE_CONFIGURATION)?.code)
      .toBe('operator_alarm_state_unavailable');
  });

  it('enables Run only once the Kernel holds a reconciled copy of the active configuration', () => {
    const unavailable = 'operator_alarm_state_unavailable';
    const held = (overrides: Partial<NonNullable<OperatorAlarmSnapshot['kernelAttention']>>) =>
      alarmSnapshot([], true, {
        ownerEpoch: 'alarm.epoch.1',
        generation: 3,
        reconciled: true,
        activeConfiguration: ACTIVE_CONFIGURATION,
        ...overrides,
      });
    // The owner reads reconciled, but the Kernel holds no current copy yet.
    expect(motionAttentionBlock(alarmSnapshot([], true, null), ARM, ACTIVE_CONFIGURATION)?.code)
      .toBe(unavailable);
    // The Kernel still holds the unreconciled copy published before.
    expect(motionAttentionBlock(held({ reconciled: false }), ARM, ACTIVE_CONFIGURATION)?.code)
      .toBe(unavailable);
    // After an Apply: the Kernel holds a copy judged against the superseded
    // configuration (or none), which it refuses as a configuration mismatch.
    expect(motionAttentionBlock(
      held({ activeConfiguration: { retainedBundleId: 'bundle.old', graphRevision: 'graph.test' } }),
      ARM,
      ACTIVE_CONFIGURATION,
    )?.code).toBe(unavailable);
    expect(motionAttentionBlock(
      held({ activeConfiguration: { retainedBundleId: 'bundle.test', graphRevision: 'graph.old' } }),
      ARM,
      ACTIVE_CONFIGURATION,
    )?.code).toBe(unavailable);
    expect(motionAttentionBlock(held({ activeConfiguration: null }), ARM, ACTIVE_CONFIGURATION)?.code)
      .toBe(unavailable);
    // No active configuration is published.
    expect(motionAttentionBlock(held({}), ARM, null)?.code).toBe(unavailable);
    // A copy of a replaced owner epoch.
    expect(motionAttentionBlock(held({ ownerEpoch: 'alarm.epoch.0' }), ARM, ACTIVE_CONFIGURATION)?.code)
      .toBe(unavailable);
    // Reconciled for the active configuration: Run is enabled.
    expect(motionAttentionBlock(held({}), ARM, ACTIVE_CONFIGURATION)).toBeNull();
  });

  it('tests recovery against the held operation’s exact resources only', () => {
    const offer = {
      participants: [
        { operation_participant_id: 'p1', resource_ids: ['robot.joint_2', 'robot.joint_1'] },
        { operation_participant_id: 'p2', resource_ids: ['robot.joint_1'] },
      ],
    } as unknown as CycleRecoveryOffer;
    expect(recoveryOfferResourceIds(offer)).toEqual(['robot.joint_1', 'robot.joint_2']);
    expect(MOTION_PRODUCING_RECOVERY_ACTIONS.has('acknowledge_and_continue')).toBe(false);
    expect(MOTION_PRODUCING_RECOVERY_ACTIONS.has('reset_cycle')).toBe(false);
    for (const action of [
      'retry_motion_from_current_state', 'retry_after_scene_repair',
      'replan_from_stopped_state', 'retry_runtime_step',
    ] as const) {
      expect(MOTION_PRODUCING_RECOVERY_ACTIONS.has(action)).toBe(true);
    }
  });
});
