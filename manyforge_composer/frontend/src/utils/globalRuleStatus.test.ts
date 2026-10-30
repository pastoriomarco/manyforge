import { describe, expect, it } from 'vitest';

import {
  globalRuleStateText,
  globalRuleUnavailableText,
  parseGlobalRuleStatus,
} from './globalRuleStatus';

describe('global rule status', () => {
  it('parses unknown, empty and every state', () => {
    expect(parseGlobalRuleStatus(null)).toBeNull();
    expect(parseGlobalRuleStatus([])).toEqual([]);
    expect(parseGlobalRuleStatus([
      { rule_id: 'a', state: 'clear', keys: [] },
      { rule_id: 'b', state: 'tripped', keys: [] },
      { rule_id: 'c', state: 'latched', keys: [] },
      { rule_id: 'd', state: 'unknown', keys: [],
        unavailable_reason: { code: 'motion_interrupt_arm_failed', message: 'watch unavailable', details: { source: 'door' } } },
      { rule_id: 'e', state: 'pending', keys: [] },
      { rule_id: 'f', state: 'waiting_for_program', keys: ['part_ready'] },
    ])?.map((rule) => rule.state)).toEqual(['clear', 'tripped', 'latched', 'unknown', 'pending', 'waiting_for_program']);
    expect(parseGlobalRuleStatus([
      { rule_id: 'd', state: 'unknown', keys: [],
        unavailable_reason: { code: 'motion_interrupt_arm_failed', message: 'watch unavailable', details: { source: 'door' } } },
    ])?.[0].unavailableReason).toEqual({
      code: 'motion_interrupt_arm_failed', message: 'watch unavailable', details: { source: 'door' },
    });
  });

  it.each([
    [undefined],
    [{}],
    [[{ rule_id: 'a', state: 'unknown', keys: [] }]],
    [[{ rule_id: 'a', state: 'unknown', keys: [], unavailable_reason: { code: 'x' } }]],
    [[{ rule_id: 'a', state: 'unknown', keys: [], unavailable_reason: {
      code: 'x', message: '', details: {}, extra: 1,
    } }]],
    [[{ rule_id: 'a', state: 'unknown', keys: [], unavailable_reason: {
      code: 'x', message: '', details: { source: false },
    } }]],
    [[{ rule_id: 'a', state: 'clear', keys: [], unavailable_reason: {
      code: 'x', message: '', details: {},
    } }]],
    [[{ rule_id: 'a', state: 'unknown', keys: ['part_ready'] }]],
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
    const unknown = {
      ruleId: 'a', state: 'unknown' as const, keys: [],
      unavailableReason: { code: 'motion_interrupt_arm_failed', message: 'watch unavailable', details: { source: 'door' } },
    };
    expect(globalRuleStateText(unknown))
      .toBe("can't watch (motion blocked)");
    expect(globalRuleUnavailableText(unknown))
      .toBe("Interrupt rule a can't watch its signals: Signal interrupt not armed");
    expect(globalRuleUnavailableText({ ruleId: 'a', state: 'pending', keys: [] })).toBeNull();
    expect(globalRuleStateText({ ruleId: 'a', state: 'pending', keys: [] }))
      .toBe('pending (no current state yet)');
    expect(globalRuleStateText({ ruleId: 'a', state: 'waiting_for_program', keys: ['x', 'y'] }))
      .toBe('waiting for a Program that declares x, y');
  });

  it.each([
    ['io_watch_bound_exceeded', 'Signal watch capacity reached', { reason: 'io_watch_bound_exceeded', limit: '4' }],
    ['io_rule_source_unavailable', 'Interrupt rule source unavailable', { reason: 'io_rule_source_unavailable' }],
    ['io_rule_source_disabled', 'Interrupt rule source is disabled', { reason: 'io_rule_source_disabled' }],
    ['io_rule_baseline_unavailable', 'Program value baseline unavailable', { reason: 'io_rule_baseline_unavailable' }],
  ])('uses the catalog text for underlying cause %s', (underlyingCode, reasonText, details) => {
    expect(globalRuleUnavailableText({
      ruleId: 'a', state: 'unknown', keys: [],
      unavailableReason: {
        code: underlyingCode === 'io_watch_bound_exceeded'
          ? 'execution_rejected' : 'motion_interrupt_arm_failed',
        message: 'Input unavailable',
        details,
      },
    })).toBe(`Interrupt rule a can't watch its signals: ${reasonText}`);
  });

  it('does not synthesize text for an unmapped detailed reason', () => {
    expect(globalRuleUnavailableText({
      ruleId: 'a', state: 'unknown', keys: [],
      unavailableReason: {
        code: 'motion_interrupt_arm_failed', message: 'Input unavailable',
        details: { reason: 'unregistered_watch_failure' },
      },
    })).toBe("Interrupt rule a can't watch its signals: Signal interrupt not armed");
  });
});
