import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import {
  GLOBAL_INTERRUPT_GATED,
  GLOBAL_INTERRUPT_SOURCE_NOT_READY,
  isMotionWaitRefusal,
  motionWaitRefusalMessage,
} from './motionWaitRefusal';

describe('global interrupt source not ready (contract 75)', () => {
  it('names the waiting rule readably', () => {
    const message = motionWaitRefusalMessage(GLOBAL_INTERRUPT_SOURCE_NOT_READY, { rule_id: 'door_rule' });
    expect(message).toBe(
      'Motion waits: global interrupt rule door_rule is waiting for its source. Try again once the source reports.',
    );
    expect(isMotionWaitRefusal(message)).toBe(true);
    expect(motionWaitRefusalMessage(GLOBAL_INTERRUPT_SOURCE_NOT_READY, {}))
      .toContain('a global interrupt rule is waiting');
  });

  it('presents an active gate rule as a wait, by code or by details.reason', () => {
    const expected = 'Motion waits: global interrupt rule light_curtain is active. Motion is allowed again when its condition clears.';
    expect(motionWaitRefusalMessage(GLOBAL_INTERRUPT_GATED, { rule_id: 'light_curtain' })).toBe(expected);
    const owner = motionWaitRefusalMessage('invalid_state_transition',
      { reason: 'global_interrupt_gated', rule_id: 'light_curtain' });
    expect(owner).toBe(expected);
    expect(isMotionWaitRefusal(owner)).toBe(true);
    // A Full Stop latch needs the operator: not a wait.
    expect(motionWaitRefusalMessage('invalid_state_transition',
      { reason: 'global_interrupt_latched', rule_id: 'light_curtain' })).toBeNull();
  });

  it('leaves every other refusal to the existing presentation', () => {
    expect(motionWaitRefusalMessage('operator_alarm_interlock_active', { rule_id: 'x' })).toBeNull();
    expect(motionWaitRefusalMessage(null)).toBeNull();
    expect(isMotionWaitRefusal('Run failed')).toBe(false);
    expect(isMotionWaitRefusal(null)).toBe(false);
  });

  it('is presented as an amber notice on both refusal paths, never an operational error', () => {
    // Every command refusal path renders through the one refusal text.
    const refusal = readFileSync(new URL('./commandRefusal.ts', import.meta.url), 'utf8');
    expect(refusal).toContain('?? motionWaitRefusalMessage(error.code, error.details);');
    const headless = readFileSync(new URL('../hooks/useHeadlessState.ts', import.meta.url), 'utf8');
    expect(headless).toContain(': commandRefusalText(error));');
    expect(headless.match(/commandRefusalText\(\s*receiptRefusal\(/g)).toHaveLength(2);
    const app = readFileSync(new URL('../App.tsx', import.meta.url), 'utf8');
    expect(app).toContain('message: isMotionWaitRefusal(programRuntime.state.error) ? null : programRuntime.state.error,');
    expect(app).toContain('&& !isMotionWaitRefusal(robotControl.state.error)');
    expect(app).toMatch(/id: `motion\.wait\.\$\{id\}`,\s*severity: 'warning',/);
  });
});
