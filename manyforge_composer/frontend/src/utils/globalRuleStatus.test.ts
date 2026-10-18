import { describe, expect, it } from 'vitest';

import { globalRuleStateText, parseGlobalRuleStatus } from './globalRuleStatus';

describe('global rule status', () => {
  it('parses unknown, empty and every state', () => {
    expect(parseGlobalRuleStatus(null)).toBeNull();
    expect(parseGlobalRuleStatus([])).toEqual([]);
    expect(parseGlobalRuleStatus([
      { rule_id: 'a', state: 'clear', keys: [] },
      { rule_id: 'b', state: 'tripped', keys: [] },
      { rule_id: 'c', state: 'latched', keys: [] },
      { rule_id: 'd', state: 'pending', keys: [] },
      { rule_id: 'e', state: 'waiting_for_program', keys: ['part_ready'] },
    ])?.map((rule) => rule.state)).toEqual(['clear', 'tripped', 'latched', 'pending', 'waiting_for_program']);
  });

  it.each([
    [undefined],
    [{}],
    [[{ rule_id: 'a', state: 'unknown', keys: [] }]],
    [[{ rule_id: 'a', state: 'clear' }]],
    [[{ rule_id: 'a', state: 'clear', keys: [], extra: 1 }]],
    [[{ rule_id: '', state: 'clear', keys: [] }]],
    [[{ rule_id: 'a', state: 'clear', keys: [] }, { rule_id: 'a', state: 'clear', keys: [] }]],
    [[{ rule_id: 'a', state: 'waiting_for_program', keys: [] }]],
    [[{ rule_id: 'a', state: 'clear', keys: ['k'] }]],
  ])('refuses a malformed list %#', (value) => {
    expect(() => parseGlobalRuleStatus(value)).toThrow();
  });

  it('names the pending source and the keys a Program must declare', () => {
    expect(globalRuleStateText({ ruleId: 'a', state: 'pending', keys: [] }))
      .toBe('pending (no current state yet)');
    expect(globalRuleStateText({ ruleId: 'a', state: 'waiting_for_program', keys: ['x', 'y'] }))
      .toBe('waiting for a Program that declares x, y');
  });
});
