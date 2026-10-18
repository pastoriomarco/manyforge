import { describe, expect, it, vi } from 'vitest';

import {
  globalRuleLatchFromOffer,
  globalRuleLatchNotices,
  type GlobalRuleLatch,
} from './globalRuleLatches';

const latch = (patch: Partial<GlobalRuleLatch> = {}): GlobalRuleLatch => ({
  ruleId: 'door_open',
  latchCause: 'condition',
  currentCondition: false,
  postLatchDisposition: 'await_operator_ack',
  ...patch,
});

describe('global rule latch notices (contracts 78/86)', () => {
  it('shows a source-loss latch distinctly with its current condition', () => {
    const [lost] = globalRuleLatchNotices([latch({ latchCause: 'source_loss', currentCondition: null })]);
    expect(lost.title).toBe('Global rule door_open latched: source lost');
    expect(lost.message).toContain('Cause: source lost');
    expect(lost.message).toContain('Current condition: unknown');
    const [held] = globalRuleLatchNotices([latch({ currentCondition: true })]);
    expect(held.title).toBe('Global rule door_open latched');
    expect(held.message).toContain('Cause: condition held');
    expect(held.message).toContain('Current condition: true');
    expect(held.severity).toBe('error');
    expect(held.domain).toBe('recovery');
  });

  it('offers Clear latch only once the source is fresh and the condition false, with a command', () => {
    const clear = vi.fn();
    const action = (value: GlobalRuleLatch, onClear?: (latch: GlobalRuleLatch) => void) => (
      globalRuleLatchNotices([value], onClear)[0].actions![0]
    );
    expect(action(latch({ currentCondition: null }), clear)).toMatchObject({ disabled: true, title: expect.stringContaining('not all fresh') });
    expect(action(latch({ currentCondition: true }), clear)).toMatchObject({ disabled: true, title: expect.stringContaining('still holds') });
    expect(action(latch())).toMatchObject({ disabled: true, title: expect.stringContaining('not available') });
    const ready = action(latch({ latchCause: 'source_loss' }), clear);
    expect(ready.disabled).toBe(false);
    ready.run();
    expect(clear).toHaveBeenCalledWith(latch({ latchCause: 'source_loss' }));
  });

  it('clears an automatic condition latch by itself, never a source-loss latch', () => {
    const [automatic] = globalRuleLatchNotices(
      [latch({ postLatchDisposition: 'auto_clear_on_signal_release', currentCondition: true })],
      vi.fn(),
    );
    expect(automatic.actions).toBeUndefined();
    expect(automatic.message).toContain('clears automatically when the condition is false');
    const [lost] = globalRuleLatchNotices(
      [latch({ latchCause: 'source_loss', postLatchDisposition: 'auto_clear_on_signal_release' })],
      vi.fn(),
    );
    expect(lost.actions![0].disabled).toBe(false);
  });

  it('shows the clear result and blocks a second send while pending or unknown', () => {
    const clear = vi.fn();
    const [pending] = globalRuleLatchNotices(
      [latch({ clear: { pending: true, message: null, outcomeUnknown: false } })], clear,
    );
    expect(pending.busy).toBe(true);
    expect(pending.actions![0]).toMatchObject({ disabled: true, label: 'Clearing latch' });
    const [unknown] = globalRuleLatchNotices([latch({
      clear: { pending: false, message: 'Clear request outcome unknown (offline); it was not resent.', outcomeUnknown: true },
    })], clear);
    expect(unknown.actions![0]).toMatchObject({ disabled: true, title: expect.stringContaining('unknown') });
    expect(unknown.message).toContain('it was not resent');
    unknown.actions![0].run();
    expect(clear).not.toHaveBeenCalled();
    const [rejected] = globalRuleLatchNotices([latch({
      clear: { pending: false, message: 'Clear rejected: global_rule_condition_true.', outcomeUnknown: false },
    })], clear);
    expect(rejected.message).toContain('Clear rejected: global_rule_condition_true.');
    expect(rejected.actions![0].disabled).toBe(false);
  });

  it('maps one owner offer by its selector rule', () => {
    expect(globalRuleLatchFromOffer({
      selector: {
        context: {
          owner_epoch: 'kernel.1', catalog_generation: '4', active_publication_generation: 2,
          bundle: 'bundle.1', graph: 'graph.1',
        },
        rule_id: 'door_open',
        expected_rule_registration_id: '9',
      },
      currentCondition: false,
      postLatchDisposition: 'await_operator_ack',
      latchCause: 'source_loss',
    })).toEqual(latch({ latchCause: 'source_loss' }));
  });
});
