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
    expect(operatorAlarmRefusalMessage('operator_alarm_interlock_active')).toBe(
      'Motion is blocked by an active alarm condition. Resolve its cause; acknowledging the alarm does not release it.',
    );
    expect(operatorAlarmRefusalMessage('operator_alarm_state_unavailable'))
      .toMatch(/alarm state is unavailable/);
    expect(operatorAlarmRefusalMessage('motion_admission_closed')).toBeNull();
  });

  it('shows the interlock membership by condition in the failure details', () => {
    expect(refusalMembershipConditionIds('not json')).toEqual([]);
    expect(structuredErrorRows({
      details: {
        reason: 'operator_alarm_interlock_active',
        interlock_alarms: membership('alarm_scope'),
        owner_epoch: 'alarm.owner.1',
        generation: '7',
      },
      origin: null,
    })).toEqual([
      { label: 'Reason', value: 'operator_alarm_interlock_active' },
      { label: 'Alarm interlocks', value: 'alarm_scope' },
      { label: 'Generation', value: '7' },
      { label: 'Owner epoch', value: 'alarm.owner.1' },
    ]);
  });
});
