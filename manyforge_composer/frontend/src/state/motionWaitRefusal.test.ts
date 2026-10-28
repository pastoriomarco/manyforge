import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import {
  GLOBAL_INTERRUPT_GATED,
  GLOBAL_INTERRUPT_SOURCE_NOT_READY,
  isMotionWaitRefusal,
  motionWaitRefusalMessage,
  setMotionWaitSourceDescriber,
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

  it('names the missing signal and its own source id, labelled by the Applied Deployment', () => {
    const details = { rule_id: 'door_rule', signal_component_id: 'cell_io', signal_name: 'door_closed', source_id: 'door_src' };
    setMotionWaitSourceDescriber(null);
    expect(motionWaitRefusalMessage(GLOBAL_INTERRUPT_SOURCE_NOT_READY, details)).toBe(
      'Motion waits: global interrupt rule door_rule is waiting for signal door_closed of cell_io (source door_src), '
        + 'which has no fresh reading yet. Try again once it reports.',
    );
    setMotionWaitSourceDescriber((component, signal, source) => (
      component === 'cell_io' && signal === 'door_closed' && source === 'door_src' ? 'ROS topic /cell/door' : null));
    try {
      const message = motionWaitRefusalMessage(GLOBAL_INTERRUPT_SOURCE_NOT_READY, details);
      expect(message).toBe(
        'Motion waits: global interrupt rule door_rule is waiting for signal door_closed of cell_io '
          + '(source door_src: ROS topic /cell/door), which has no fresh reading yet. Try again once it reports.',
      );
      expect(isMotionWaitRefusal(message)).toBe(true);
      // Not declared so in the Applied Deployment: only the owner's source id.
      expect(motionWaitRefusalMessage(GLOBAL_INTERRUPT_SOURCE_NOT_READY, { ...details, signal_name: 'other' }))
        .toContain('signal other of cell_io (source door_src),');
      // The source changed elsewhere: the refusal's own id wins, never a stale label.
      expect(motionWaitRefusalMessage(GLOBAL_INTERRUPT_SOURCE_NOT_READY, { ...details, source_id: 'new_src' }))
        .toContain('signal door_closed of cell_io (source new_src),');
      // No source id in the refusal: nothing to label.
      expect(motionWaitRefusalMessage(GLOBAL_INTERRUPT_SOURCE_NOT_READY, { ...details, source_id: undefined }))
        .toContain('waiting for signal door_closed of cell_io, which has no fresh reading yet.');
    } finally {
      setMotionWaitSourceDescriber(null);
    }
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

  it('is presented as a warning to acknowledge on both refusal paths, titled as a wait', () => {
    // Every command refusal path renders through the one refusal text.
    const refusal = readFileSync(new URL('./commandRefusal.ts', import.meta.url), 'utf8');
    expect(refusal).toContain('?? motionWaitRefusalMessage(error.code, error.details);');
    const headless = readFileSync(new URL('../hooks/useHeadlessState.ts', import.meta.url), 'utf8');
    expect(headless).toContain(': commandRefusalText(error));');
    expect(headless.match(/commandRefusalText\(\s*receiptRefusal\(/g)).toHaveLength(2);
    const app = readFileSync(new URL('../App.tsx', import.meta.url), 'utf8');
    // A past refusal: a registry warning (Acknowledge), never red or Dismiss.
    expect(app).toMatch(/title: isMotionWaitRefusal\(programRuntime\.state\.error\)\s*\? 'Motion waiting'/);
    expect(app).toMatch(/title: isMotionWaitRefusal\(robotOperationalError\)\s*\? 'Motion waiting'/);
    expect(app).not.toContain('motion.wait.');
  });
});
