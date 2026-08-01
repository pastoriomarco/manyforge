const TOKEN_LABELS: Record<string, string> = {
  position_commanded: 'Position commanded',
  pneumatic: 'Pneumatic',
  native_kinematic: 'Native kinematic',
  ros_gripper_command: 'ROS GripperCommand',
  isaac_joint_state_effort: 'Isaac JointState effort',
  plc_gripper_request: 'PLC gripper request',
  native_backend: 'Native backend',
  sim_observed: 'Simulator observed',
  real_observed: 'Real observed',
  commanded_estimate: 'Commanded estimate',
  position_reached: 'Position reached',
  driver_result: 'Driver result',
  dwell_elapsed: 'Dwell elapsed',
  request_ack_then_dwell: 'Request ack then dwell',
  open_loop_immediate: 'Open-loop immediate',
  none_open_loop: 'None, open loop',
  operator_confirm: 'Operator confirm',
  discrete_signal: 'Discrete signal',
  perception: 'Perception',
  force_or_current: 'Force/current',
  unverified: 'Unverified',
};

function cleanToken(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

export function formatGripperSemanticToken(value: unknown): string {
  const token = cleanToken(value);
  if (!token) return 'Unconfigured';
  return TOKEN_LABELS[token] ?? token.replaceAll('_', ' ');
}

/** Explain completion evidence without treating open-loop completion as grasp proof. */
export function gripperCompletionNotice(options: {
  actuationKind?: unknown;
  completionPolicy?: unknown;
  graspConfirmationPolicy?: unknown;
}): string | null {
  const actuationKind = cleanToken(options.actuationKind);
  const completionPolicy = cleanToken(options.completionPolicy);
  const graspConfirmationPolicy = cleanToken(options.graspConfirmationPolicy);
  if (
    actuationKind === 'pneumatic'
    && (completionPolicy === 'dwell_elapsed'
      || completionPolicy === 'request_ack_then_dwell')
  ) {
    return 'Command and dwell completion only; grasp remains unverified unless a confirmation policy provides evidence.';
  }
  if (graspConfirmationPolicy === 'none_open_loop') {
    return 'Open-loop attach: no independent grasp proof is configured.';
  }
  return null;
}

export function gripperPositionSourceLabel(options: {
  stateAuthority?: unknown;
  observedPosition?: number | null;
  commandedPosition?: number | null;
}): string {
  const authority = cleanToken(options.stateAuthority);
  if (options.observedPosition !== null && options.observedPosition !== undefined) {
    if (authority === 'sim_observed') return 'observed sim';
    if (authority === 'real_observed') return 'observed real';
    if (authority === 'native_backend') return 'backend';
    return 'observed';
  }
  if (options.commandedPosition !== null && options.commandedPosition !== undefined) {
    return 'commanded estimate';
  }
  return 'unavailable';
}
