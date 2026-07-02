import { describe, expect, it } from 'vitest';

import {
  formatGripperSemanticToken,
  gripperCompletionNotice,
  gripperPositionSourceLabel,
  gripperTransportKind,
} from './gripperSemantics';

describe('gripper semantic presentation helpers', () => {
  it('formats known semantic tokens and preserves unknown tokens readably', () => {
    expect(formatGripperSemanticToken('dwell_elapsed')).toBe('Dwell elapsed');
    expect(formatGripperSemanticToken('custom_driver_mode')).toBe('custom driver mode');
    expect(formatGripperSemanticToken('')).toBe('Unconfigured');
  });

  it('resolves transport kind from the nested deployment transport', () => {
    expect(gripperTransportKind({ kind: 'isaac_joint_state_effort' })).toBe(
      'isaac_joint_state_effort',
    );
    expect(gripperTransportKind({}, 'ros_gripper_command')).toBe(
      'ros_gripper_command',
    );
  });

  it('warns that pneumatic dwell completion is not grasp proof', () => {
    expect(
      gripperCompletionNotice({
        actuationKind: 'pneumatic',
        completionPolicy: 'dwell_elapsed',
        graspConfirmationPolicy: 'none_open_loop',
      }),
    ).toContain('grasp remains unverified');
  });

  it('labels commanded estimate separately from observed state', () => {
    expect(
      gripperPositionSourceLabel({
        stateAuthority: 'sim_observed',
        observedPosition: 0.01,
      }),
    ).toBe('observed sim');
    expect(
      gripperPositionSourceLabel({
        stateAuthority: 'commanded_estimate',
        commandedPosition: 0.0,
      }),
    ).toBe('commanded estimate');
  });
});
