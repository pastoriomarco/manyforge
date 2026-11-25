import { describe, expect, it } from 'vitest';

import { parseApiErrorDto } from '../api/parsers/programRuntime';
import { ERROR_CATALOG } from '../api/errorCatalog.generated';
import {
  commandRefusalText, errorCatalogRow, errorPresentation, isRuleHoldsRefusal, receiptRefusal, ruleHoldsMessage,
} from './commandRefusal';

describe('command refusal text', () => {
  it('names a late command\'s age and limit on the REST and WebSocket paths', () => {
    const [compact, full] = ERROR_CATALOG.control_freshness_expired;
    const expected = `${compact}: ${full} too late (Age: 2.4 s, limit 1.0 s)`;
    const rest = parseApiErrorDto({
      detail: {
        code: 'control_freshness_expired', message: 'too late',
        details: { elapsed_ns: 2_400_000_000, valid_for_ns: 1_000_000_000 },
      },
    })!;
    expect(commandRefusalText(rest)).toBe(expected);
    const receipt = receiptRefusal('control_freshness_expired', {
      code: 'control_freshness_expired', client_sequence: '4', message: 'too late',
      details: { elapsed_ns: '2400000000', valid_for_ns: '1000000000' },
    });
    expect(commandRefusalText(receipt, { withCode: true })).toBe(`control_freshness_expired: ${expected}`);
  });

  it('names the Kernel admission reason of a refusal', () => {
    // POST /api/control/commands, HTTP 409.
    const error = parseApiErrorDto({
      detail: {
        code: 'request_ledger_full',
        details: { reason: 'claim_capacity_exhausted' },
        message: 'cell-admission request rejected',
      },
    })!;
    expect(commandRefusalText(error)).toBe(
      'Too many retained requests: The request was refused because the record of recent requests is full.'
        + ' Wait for running work to finish and try again; report it if this persists.'
        + ' cell-admission request rejected (Reason: claim_capacity_exhausted)',
    );
  });

  it('keeps the receipt code lead and reads the owner error of a rejected receipt', () => {
    expect(commandRefusalText(receiptRefusal('invalid_request', {
      ok: false,
      error: {
        code: 'invalid_request',
        message: 'cell-admission request rejected',
        details: { reason: 'claim_capacity_exhausted' },
      },
    }), { withCode: true })).toBe(
      'invalid_request: Request is invalid: The request was refused before any effect because a'
        + ' field is missing or invalid. The details name each refused field.'
        + ' cell-admission request rejected (Reason: claim_capacity_exhausted)',
    );
    expect(commandRefusalText(receiptRefusal('command_not_allowed', null), { withCode: true }))
      .toBe('command_not_allowed: Command not allowed: This command is not allowed in the current'
        + ' state. command_not_allowed');
  });

  it('retains the actual producer of a rejected receipt when supplied', () => {
    const nested = receiptRefusal('invalid_request', {
      producer: 'headless_api',
      error: { message: 'refused', details: { reason: 'busy' }, producer: 'kernel' },
    });
    expect(nested.producer).toBe('kernel');
    expect(nested.details).toEqual({ reason: 'busy' });
    expect(receiptRefusal('invalid_request', { producer: 'kernel' }).producer).toBe('kernel');
    expect(receiptRefusal('invalid_request', { error: { message: 'refused' } }).producer)
      .toBeUndefined();
  });

  it('keeps the existing alarm and holding-rule guidance first', () => {
    expect(commandRefusalText(receiptRefusal('rule_holds', {
      error: { details: { rule_id: 'door' } },
    }), { withCode: true })).toBe(
      "Global interrupt rule door holds this scope: its condition still holds after its Stop, or it can't watch its signals. It clears by itself.",
    );
    expect(isRuleHoldsRefusal({ code: 'rule_holds', message: '', details: null, origin: null })).toBe(true);
    expect(isRuleHoldsRefusal({ code: 'global_interrupt_gated', message: '', details: null, origin: null })).toBe(false);
    expect(ruleHoldsMessage(null)).toContain('A global interrupt rule holds this scope');
    expect(commandRefusalText({
      code: 'operator_alarm_interlock_active', message: 'refused', details: null, origin: null,
    })).toBe(
      'Motion is blocked by an active alarm condition. Resolve its cause; acknowledging the alarm does not release it.',
    );
  });

  it('presents the catalog text and falls back to the producer message', () => {
    // A code without a row keeps the producer's message, unchanged.
    expect(errorCatalogRow('not_a_catalog_code')).toBeNull();
    expect(errorPresentation({
      code: 'not_a_catalog_code', message: 'owner said no', details: { reason: 'busy' }, origin: null,
    })).toEqual({ text: 'owner said no (Reason: busy)', detail: null });
    // A row without full text shows its compact text and the producer message.
    // Every shipped row has full text, so the test adds a temporary one.
    const catalog = ERROR_CATALOG as Record<string, readonly [string, string]>;
    catalog.test_placeholder_code = ['Placeholder', ''];
    try {
      expect(errorPresentation({
        code: 'test_placeholder_code', message: 'producer text', details: null, origin: null,
      })).toEqual({ text: 'Placeholder', detail: 'producer text' });
    } finally {
      delete catalog.test_placeholder_code;
    }
    // Inherited object members are not catalog rows.
    expect(errorCatalogRow('toString')).toBeNull();
    // The Manager's forwarded producer is carried, not shown as text.
    const forwarded = parseApiErrorDto({
      detail: { code: 'apply_admission_failed', message: 'Kernel refused', producer: 'kernel' },
    })!;
    expect(forwarded.producer).toBe('kernel');
    expect(errorPresentation(forwarded).text).toBe('Apply not admitted');
  });

  it('presents lost position feedback as its own motion stop, not a collision', () => {
    // Live incident 2026-10-29: stale measured joint states stopped motion.
    const { text, detail } = errorPresentation({
      code: 'runtime_monitor_observation_lost',
      message: 'Runtime monitor stopped the operation: measured robot state was lost',
      details: { reason: 'runtime_monitor_observation_lost' },
      origin: null,
    });
    expect(text).toBe('Robot position feedback lost');
    expect(detail).toMatch(
      /^Motion stopped: the robot's position feedback was lost during motion; this is not a detected collision\./,
    );
  });
});
