import { describe, expect, it } from 'vitest';

import { parseApiErrorDto } from '../api/parsers/programRuntime';
import { commandRefusalText, receiptRefusal } from './commandRefusal';

describe('command refusal text', () => {
  it('names the Kernel admission reason of a Trial-E-like refusal', () => {
    // Captured live: POST /api/control/commands, HTTP 409.
    const error = parseApiErrorDto({
      detail: {
        code: 'stale_generation',
        details: { reason: 'expected_generation_stale' },
        message: 'cell-admission request rejected',
      },
    })!;
    expect(commandRefusalText(error)).toBe(
      'cell-admission request rejected (Reason: expected_generation_stale)',
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
      'invalid_request: cell-admission request rejected (Reason: claim_capacity_exhausted)',
    );
    expect(commandRefusalText(receiptRefusal('command_not_allowed', null), { withCode: true }))
      .toBe('command_not_allowed: command_not_allowed');
  });

  it('keeps the existing alarm and motion-wait guidance first', () => {
    expect(commandRefusalText(receiptRefusal('global_interrupt_source_not_ready', {
      error: { details: { rule_id: 'door' } },
    }), { withCode: true })).toBe(
      'Motion waits: global interrupt rule door is waiting for its source. Try again once the source reports.',
    );
    expect(commandRefusalText({
      code: 'unacknowledged_operator_alarms', message: 'refused', details: null, origin: null,
    })).toBe('Motion is blocked by unacknowledged alarms. Acknowledge them, then try again.');
  });
});
