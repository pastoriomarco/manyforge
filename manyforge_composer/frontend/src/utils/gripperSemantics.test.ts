import { describe, expect, it } from 'vitest';
import {
  formatGripperSemanticToken,
  gripperCompletionNotice,
  gripperPositionSourceLabel,
} from './gripperSemantics';

describe('gripper semantic presentation', () => {
  it('formats known and unknown semantic tokens readably', () => {
    expect(formatGripperSemanticToken('dwell_elapsed')).toBe('Dwell elapsed');
    expect(formatGripperSemanticToken('custom_driver_mode')).toBe('custom driver mode');
    expect(formatGripperSemanticToken('')).toBe('Unconfigured');
  });

  it('does not present pneumatic dwell completion as grasp proof', () => {
    expect(
      gripperCompletionNotice({
        actuationKind: 'pneumatic',
        completionPolicy: 'dwell_elapsed',
        graspConfirmationPolicy: 'none_open_loop',
      }),
    ).toContain('grasp remains unverified');
  });

  it('distinguishes observed, commanded-estimate, and unavailable state', () => {
    expect(
      gripperPositionSourceLabel({
        stateAuthority: 'sim_observed',
        observedPosition: 0.01,
      }),
    ).toBe('observed sim');
    expect(gripperPositionSourceLabel({ commandedPosition: 0 })).toBe(
      'commanded estimate',
    );
    expect(gripperPositionSourceLabel({})).toBe('unavailable');
  });
});
