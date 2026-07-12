import { describe, expect, it } from 'vitest';

import type {
  CandidateResponse,
  SceneGripperState,
  SceneRobotState,
} from '../types/api';
import { gripperGhostPreviewFromCandidate } from './gripperGhost';

const ROBOT: SceneRobotState = {
  robot_id: 'ur5',
  base_frame: 'base',
  tcp_frame: 'tool0',
  joint_names: ['shoulder_pan_joint', 'wrist_3_joint'],
  joint_positions: {
    shoulder_pan_joint: 0.1,
    wrist_3_joint: -0.2,
  },
};

const GRIPPER: SceneGripperState = {
  gripper_id: 'primary',
  command_id: 1,
  generation: 1,
  revision: 1,
  joint_name: 'finger_joint',
  state: 'idle',
  current_position: 0.0,
  target_position: 0.0,
  limit_lower: 0.0,
  limit_upper: 0.8,
  motion_time_s: 0.5,
  srdf_presets: {},
  visualization: {
    gripper_root_link: 'robotiq_base_link',
    tcp_to_root_position_m: [0, 0, 0],
    tcp_to_root_orientation_xyzw: [0, 0, 0, 1],
    subtree_links: [
      {
        name: 'robotiq_base_link',
        position_m: [0, 0, 0],
        orientation_xyzw: [0, 0, 0, 1],
      },
      {
        name: 'left_finger_link',
        position_m: [0.01, 0, 0],
        orientation_xyzw: [0, 0, 0, 1],
      },
    ],
  },
  error: null,
};

function candidate(
  targetPosition: number,
  status: CandidateResponse['status'] = 'ready',
): CandidateResponse {
  return {
    candidateId: 'cand-1',
    generation: 1,
    sourceSurface: 'gripper_slider',
    inputMode: 'gripper',
    status,
    commitEligible: status === 'ready',
    gripperTarget: { gripperId: 'primary', targetPosition },
    diagnostics: { reasonCode: 'ready', summary: 'ready' },
    maxAgeNs: 0,
    expiresAtNs: 0,
    createdAtNs: 0,
    updatedAtNs: 0,
  };
}

describe('gripperGhostPreviewFromCandidate', () => {
  it('builds a subtree-only FK request by appending the staged gripper joint', () => {
    const preview = gripperGhostPreviewFromCandidate(candidate(0.42), ROBOT, [GRIPPER]);

    expect(preview).not.toBeNull();
    expect(preview?.linkNames).toEqual(['robotiq_base_link', 'left_finger_link']);
    expect(preview?.request).toMatchObject({
      kind: 'joint',
      link_names: ['robotiq_base_link', 'left_finger_link'],
      joint_names: ['shoulder_pan_joint', 'wrist_3_joint', 'finger_joint'],
      joint_positions_rad: [0.1, -0.2, 0.42],
    });
  });

  it('overrides an existing gripper joint in the current robot vector', () => {
    const robot = {
      ...ROBOT,
      joint_names: [...ROBOT.joint_names, 'finger_joint'],
      joint_positions: { ...ROBOT.joint_positions, finger_joint: 0.1 },
    };

    const preview = gripperGhostPreviewFromCandidate(candidate(0.5), robot, [GRIPPER]);

    expect(preview?.request.joint_names).toEqual([
      'shoulder_pan_joint',
      'wrist_3_joint',
      'finger_joint',
    ]);
    expect(preview?.request.joint_positions_rad).toEqual([0.1, -0.2, 0.5]);
  });

  it('fails closed for blocked or out-of-limit gripper targets', () => {
    expect(
      gripperGhostPreviewFromCandidate(candidate(0.42, 'blocked'), ROBOT, [GRIPPER]),
    ).toBeNull();
    expect(
      gripperGhostPreviewFromCandidate(candidate(1.2), ROBOT, [GRIPPER]),
    ).toBeNull();
  });

  it('fails closed when subtree visualization metadata is unavailable', () => {
    const gripper = { ...GRIPPER, visualization: null };

    expect(
      gripperGhostPreviewFromCandidate(candidate(0.42), ROBOT, [gripper]),
    ).toBeNull();
  });
});
