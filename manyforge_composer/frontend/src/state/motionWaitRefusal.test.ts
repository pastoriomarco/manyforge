import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import {
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

  it('leaves every other refusal to the existing presentation', () => {
    expect(motionWaitRefusalMessage('operator_alarm_interlock_active', { rule_id: 'x' })).toBeNull();
    expect(motionWaitRefusalMessage(null)).toBeNull();
    expect(isMotionWaitRefusal('Run failed')).toBe(false);
    expect(isMotionWaitRefusal(null)).toBe(false);
  });

  it('is presented as an amber notice on both refusal paths, never an operational error', () => {
    const headless = readFileSync(new URL('../hooks/useHeadlessState.ts', import.meta.url), 'utf8');
    expect(headless).toContain('|| motionWaitRefusalMessage(error?.code, error?.details)');
    expect(headless).toContain('?? motionWaitRefusalMessage(value.code as string, ownerDetails);');
    const app = readFileSync(new URL('../App.tsx', import.meta.url), 'utf8');
    expect(app).toContain('message: isMotionWaitRefusal(programRuntime.state.error) ? null : programRuntime.state.error,');
    expect(app).toContain('&& !isMotionWaitRefusal(robotControl.state.error)');
    expect(app).toMatch(/id: `motion\.wait\.\$\{id\}`,\s*severity: 'warning',/);
  });
});
