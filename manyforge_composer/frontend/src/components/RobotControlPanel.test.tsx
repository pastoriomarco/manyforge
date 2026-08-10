import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { RobotControlPanel } from './RobotControlPanel';

const controller = vi.hoisted(() => ({
  state: {
    status: {
      ownerEpoch: 'owner-1',
      graphRevision: 'sha256:graph',
      retainedBundleId: 'sha256:bundle',
      sceneRevision: {
        scene_id: 'scene-1',
        revision_id: 1,
        content_hash: 'sha256:scene',
      },
      directControl: {
        controllableComponentId: 'robot-1',
        commandTargetId: 'arm',
        componentRoleId: 'controlled_component',
        targetRoleId: 'command_target',
        baseFrameRoleId: 'base',
        tcpFrameRoleId: 'tcp',
        operationCapabilityId: 'execution.trajectory.execute',
        planningCapabilityIds: ['planning.motion.joint_goal'],
        frameRoleIds: ['base', 'tcp'],
        programId: 'manyforge.direct_control.motion',
        programRevision: 'sha256:motion-program',
        programBindingId: 'sha256:motion-binding',
        programRunId: 'motion-run',
        joints: [{
          sourceJointName: 'joint_1',
          minPositionRad: -1,
          maxPositionRad: 1,
          maxVelocityRadS: 2,
          maxAccelerationRadS2: 3,
        }],
      },
      directControlGripper: {
        controllableComponentId: 'robot-1',
        commandTargetId: 'gripper',
        componentRoleId: 'controlled_component',
        targetRoleId: 'command_target',
        operationCapabilityId: 'tool.gripper.command',
        programId: 'manyforge.direct_control.gripper',
        programRevision: 'sha256:gripper-program',
        programBindingId: 'sha256:gripper-binding',
        programRunId: 'gripper-run',
        commandSchemaId: 'gripper.command_position',
        commandSchemaRevision: 'sha256:gripper-schema',
        sourceCoordinateName: 'finger_joint',
        sourceKind: 'model_role',
        coordinateKind: 'linear_m',
        minPosition: 0,
        maxPosition: 0.04,
      },
      observedMotion: {
        targetRoleId: 'arm',
        controllableComponentId: 'robot-1',
        commandTargetId: 'arm',
        sequence: 1,
        sourceEpoch: 'motion-source',
        sourceJointNames: ['joint_1'],
        positionsRad: [0],
        complete: true,
        currentTcpPose: {
          position_m: { x_m: 0, y_m: 0, z_m: 0.2 },
          orientation: { x: 0, y: 0, z: 0, w: 1 },
        },
      },
      observedGripper: {
        sequence: 1,
        sourceEpoch: 'gripper-source',
        sourceCoordinateName: 'finger_joint',
        position: 0.02,
        coordinateKind: 'linear_m',
        complete: true,
      },
    },
    phase: 'ready',
    mode: 'joint',
    draftPositionsRad: [0.2],
    draftPose: {
      position_m: { x_m: 0, y_m: 0, z_m: 0.2 },
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    },
    draftGripperPosition: 0.02,
    generation: 1,
    intent: {},
    preview: {},
    ghost: null,
    ghostStale: false,
    operation: null,
    error: null,
  },
  selectMode: vi.fn(),
  editJoint: vi.fn(),
  editPose: vi.fn(),
  editGripper: vi.fn(),
  approve: vi.fn(),
  revert: vi.fn(),
  stop: vi.fn(),
  retry: vi.fn(),
}));

vi.mock('../hooks/useRobotControl', () => ({
  useRobotControl: () => controller,
}));

describe('RobotControlPanel layout', () => {
  it('keeps the candidate actions outside the bounded editor scroll region', () => {
    const markup = renderToStaticMarkup(<RobotControlPanel enabled />);
    const editor = markup.indexOf('data-testid="robot-control-editor-scroll"');
    const footer = markup.indexOf('data-testid="robot-control-candidate-footer"');

    expect(markup).toContain('data-testid="robot-control-panel"');
    expect(editor).toBeGreaterThan(0);
    expect(footer).toBeGreaterThan(editor);
    expect(markup).toContain('>Approve</button>');
    expect(markup).toContain('>Stop operation</button>');
  });
});
