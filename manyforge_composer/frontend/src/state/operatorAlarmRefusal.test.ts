import { describe, expect, it } from 'vitest';

import {
  operatorAlarmRefusalMessage,
  refusalMembershipConditionIds,
} from './operatorAlarmRefusal';
import { structuredErrorRows } from './structuredError';

const membership = (...conditions: string[]): string => JSON.stringify(
  conditions.map((condition) => ({
    activation_id: `${condition}.activation`,
    condition_id: condition,
    record_revision: 1,
  })),
);

describe('operator-alarm attention refusals', () => {
  it('names the remedy that releases each Kernel refusal', () => {
    expect(operatorAlarmRefusalMessage('unacknowledged_operator_alarms', {
      interlock_alarms: '[]',
      unacknowledged_alarms: membership('collision.arm'),
    })).toBe('Motion is blocked by unacknowledged alarms. Acknowledge them, then try again.');
    expect(operatorAlarmRefusalMessage('operator_alarm_interlock_active', {
      interlock_alarms: membership('alarm_scope'),
      unacknowledged_alarms: '[]',
    })).toBe(
      'Motion is blocked by an active alarm condition. Resolve its cause; acknowledging the alarm does not release it.',
    );
    expect(operatorAlarmRefusalMessage('operator_alarm_state_unavailable', null))
      .toMatch(/alarm state is unavailable/);
    expect(operatorAlarmRefusalMessage('motion_admission_closed', null)).toBeNull();
  });

  it('asks for both remedies when an interlock and unacknowledged alarms both gate', () => {
    expect(operatorAlarmRefusalMessage('operator_alarm_interlock_active', {
      interlock_alarms: membership('alarm_scope'),
      unacknowledged_alarms: membership('collision.arm'),
    })).toBe(
      'Motion is blocked by an active alarm condition. Resolve its cause; acknowledging the alarm '
        + 'does not release it. Then acknowledge the unacknowledged alarms.',
    );
  });

  it('shows both memberships by condition in the failure details', () => {
    expect(refusalMembershipConditionIds('not json')).toEqual([]);
    expect(structuredErrorRows({
      details: {
        reason: 'operator_alarm_interlock_active',
        interlock_alarms: membership('alarm_scope'),
        unacknowledged_alarms: membership('collision.a', 'collision.b'),
        owner_epoch: 'alarm.owner.1',
        generation: '7',
      },
      origin: null,
    })).toEqual([
      { label: 'Reason', value: 'operator_alarm_interlock_active' },
      { label: 'Alarm interlocks', value: 'alarm_scope' },
      { label: 'Unacknowledged alarms', value: 'collision.a, collision.b' },
      { label: 'Generation', value: '7' },
      { label: 'Owner epoch', value: 'alarm.owner.1' },
    ]);
  });
});
