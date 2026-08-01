import { describe, expect, it } from 'vitest';
import {
  acknowledgementFence,
  acknowledgementFenceMatches,
  hasNewAlarmActivation,
} from './alarmFences';

const members = [
  { conditionId: 'condition-a', activationId: 'activation-1', recordRevision: 3 },
  { conditionId: 'condition-b', activationId: 'activation-2', recordRevision: 7 },
];

describe('alarm presentation fences', () => {
  it('reannunciates only genuinely new activations', () => {
    expect(hasNewAlarmActivation(['activation-1'], ['activation-1'])).toBe(false);
    expect(hasNewAlarmActivation(['activation-1'], ['activation-1', 'activation-2'])).toBe(true);
  });

  it('binds bulk confirmation to generation, membership, and revisions', () => {
    const fence = acknowledgementFence(5, members);
    expect(acknowledgementFenceMatches(fence, 5, [...members].reverse())).toBe(true);
    expect(acknowledgementFenceMatches(fence, 6, members)).toBe(false);
    expect(acknowledgementFenceMatches(fence, 5, [
      members[0],
      { ...members[1], recordRevision: 8 },
    ])).toBe(false);
  });
});
