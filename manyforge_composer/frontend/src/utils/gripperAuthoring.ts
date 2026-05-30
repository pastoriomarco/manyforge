import type { CandidateCreateRequest, CandidateSourceSurface } from '../types/api';

/**
 * Build the gripper candidate request a gripper slider release or preset click
 * authors. The default Robot Control gripper widget authors this validated
 * candidate instead of calling `/api/scene/gripper/command` directly; the robot
 * moves only via the shared Approve action.
 */
export function gripperCandidateRequest(
  gripperId: string,
  targetPosition: number,
  surface: Extract<CandidateSourceSurface, 'gripper_slider' | 'gripper_preset'>,
): CandidateCreateRequest {
  return {
    sourceSurface: surface,
    inputMode: 'gripper',
    gripperTarget: { gripperId, targetPosition },
  };
}
