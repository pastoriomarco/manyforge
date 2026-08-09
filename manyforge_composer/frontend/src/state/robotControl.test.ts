import { describe, expect, it } from 'vitest';
import {
  buildJointMotionIntent,
  initialRobotControlState,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  ROBOT_CONTROL_OPERATION_POLL_MS,
  robotControlReducer,
} from './robotControl';
import type {
  ExecutionOwnerStatus,
  MotionPreviewReceipt,
} from '../types/robotControl';

function statusFixture(ownerEpoch = 'execution-owner-1'): ExecutionOwnerStatus {
  return {
    ownerEpoch,
    graphRevision: 'sha256:graph',
    retainedBundleId: 'sha256:bundle',
    sceneRevision: {
      scene_id: 'scene-1',
      revision_id: 3,
      content_hash: 'sha256:scene',
    },
    directControl: {
      controllableComponentId: 'robot-a',
      commandTargetId: 'arm',
      componentRoleId: 'controlled_component',
      targetRoleId: 'command_target',
      baseFrameRoleId: 'base',
      tcpFrameRoleId: 'tcp',
      operationCapabilityId: 'execution.trajectory.execute',
      planningCapabilityIds: ['planning.motion.joint_goal'],
      frameRoleIds: ['base', 'tcp'],
      programId: 'manyforge.direct_control.motion',
      programRevision: 'sha256:direct-program',
      programBindingId: 'sha256:direct-binding',
      programRunId: 'direct-control-owner-1',
      joints: [
        {
          sourceJointName: 'joint1',
          minPositionRad: -1,
          maxPositionRad: 1,
          maxVelocityRadS: 2,
          maxAccelerationRadS2: 3,
        },
        {
          sourceJointName: 'joint2',
          minPositionRad: -2,
          maxPositionRad: 2,
          maxVelocityRadS: 2,
          maxAccelerationRadS2: null,
        },
      ],
    },
    observedMotion: {
      targetRoleId: 'scenario-arm',
      controllableComponentId: 'robot-a',
      commandTargetId: 'arm',
      sequence: 7,
      sourceEpoch: 'joint-source-1',
      sourceJointNames: ['joint1', 'joint2'],
      positionsRad: [0, 0.1],
      complete: true,
    },
  };
}

function receiptFixture(
  status: ExecutionOwnerStatus,
  requestId: string,
  positionsRad: number[],
): MotionPreviewReceipt {
  return {
    schemaVersion: 'manyforge.direct_control_motion_preview.v1',
    receiptDigest: 'sha256:receipt',
    intentDigest: 'sha256:intent',
    requestId,
    requestOwnerEpoch: status.ownerEpoch,
    programId: status.directControl.programId,
    programRevision: status.directControl.programRevision,
    programBindingId: status.directControl.programBindingId,
    programRunId: status.directControl.programRunId,
    graphRevision: status.graphRevision,
    retainedBundleId: status.retainedBundleId,
    sceneRevision: { ...status.sceneRevision },
    componentRoleId: status.directControl.componentRoleId,
    targetRoleId: status.directControl.targetRoleId,
    requiredCapabilityId: status.directControl.operationCapabilityId,
    sourceJointNames: status.directControl.joints.map(
      (joint) => joint.sourceJointName,
    ),
    startPositionsRad: [...status.observedMotion.positionsRad],
    finalPositionsRad: [...positionsRad],
    finalTcpPose: {
      position_m: { x_m: 0.1, y_m: 0.2, z_m: 0.3 },
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    },
    collisionWorldRevision: {
      revision_id: 5,
      content_hash: 'collision-5',
    },
    commitEligible: true,
  };
}

function readyState() {
  const status = statusFixture();
  const positions = [0.25, 0.5];
  let state = robotControlReducer(initialRobotControlState, {
    type: 'status_resolved',
    status,
  });
  state = robotControlReducer(state, { type: 'edit', positionsRad: positions });
  const intent = buildJointMotionIntent(status, positions, 'request-1');
  state = robotControlReducer(state, {
    type: 'preview_started',
    generation: state.generation,
    intent,
  });
  state = robotControlReducer(state, {
    type: 'preview_resolved',
    generation: state.generation,
    receipt: receiptFixture(status, 'request-1', positions),
  });
  return state;
}

describe('addressed Robot Control state', () => {
  it('preserves the historical bounded authoring cadence', () => {
    expect(ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS).toBe(150);
    expect(ROBOT_CONTROL_OPERATION_POLL_MS).toBe(500);
  });

  it('builds an ordinary operation from exact source-local owner facts', () => {
    const status = statusFixture();
    const intent = buildJointMotionIntent(status, [0.2, -0.4], 'request-9');
    const participant = intent.participants[0];

    expect(intent.program_id).toBe('manyforge.direct_control.motion');
    expect(intent.program_binding_id).toBe('sha256:direct-binding');
    expect(intent.request_context.request_owner_epoch).toBe('execution-owner-1');
    expect(participant.component_role_id).toBe('controlled_component');
    expect(participant.target_role_id).toBe('command_target');
    expect(participant.payload.base_frame_role_id).toBe('base');
    expect(participant.payload.tcp_frame_role_id).toBe('tcp');
    expect(participant.payload.motion).toEqual({
      kind: 'joint_goal',
      joint_names: ['joint1', 'joint2'],
      positions_rad: [0.2, -0.4],
    });
  });

  it('makes a prior ready preview stale and non-approvable immediately', () => {
    const ready = readyState();
    const edited = robotControlReducer(ready, {
      type: 'edit',
      positionsRad: [0.7, 0.5],
    });

    expect(ready.phase).toBe('ready');
    expect(edited.phase).toBe('planning');
    expect(edited.preview).toBeNull();
    expect(edited.intent).toBeNull();
    expect(edited.ghost).toBe(ready.ghost);
    expect(edited.ghostStale).toBe(true);
  });

  it('ignores a preview response superseded by a newer edit', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit',
      positionsRad: [0.2, 0.1],
    });
    const oldGeneration = state.generation;
    const oldIntent = buildJointMotionIntent(status, [0.2, 0.1], 'old');
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: oldGeneration,
      intent: oldIntent,
    });
    state = robotControlReducer(state, {
      type: 'edit',
      positionsRad: [0.8, 0.1],
    });
    const stale = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: oldGeneration,
      receipt: receiptFixture(status, 'old', [0.2, 0.1]),
    });

    expect(stale).toBe(state);
    expect(stale.phase).toBe('planning');
    expect(stale.preview).toBeNull();
  });

  it('shows the owner-validated planned final state without requiring it to equal the goal', () => {
    const status = statusFixture();
    const positions = [0.25, 0.5];
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, { type: 'edit', positionsRad: positions });
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: state.generation,
      intent: buildJointMotionIntent(status, positions, 'request-final'),
    });
    state = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: receiptFixture(
        status,
        'request-final',
        [0.249, 0.501],
      ),
    });

    expect(state.phase).toBe('ready');
    expect(state.ghost?.finalPositionsRad).toEqual([0.249, 0.501]);
  });

  it('fails closed on an out-of-limit draft', () => {
    const status = statusFixture();
    const loaded = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    const blocked = robotControlReducer(loaded, {
      type: 'edit',
      positionsRad: [2, 0.1],
    });

    expect(blocked.phase).toBe('blocked');
    expect(blocked.preview).toBeNull();
    expect(blocked.error).toContain('outside');
  });

  it('submits only a ready preview and tracks its exact operation', () => {
    const ready = readyState();
    const approving = robotControlReducer(ready, { type: 'approve_started' });
    const executing = robotControlReducer(approving, {
      type: 'approve_resolved',
      acceptance: {
        requestId: 'request-1',
        operationId: 'operation-1',
        operationRevision: 1,
      },
    });

    expect(approving.phase).toBe('approving');
    expect(executing.phase).toBe('executing');
    expect(executing.operation).toMatchObject({
      operationId: 'operation-1',
      operationRevision: 1,
      state: 'queued',
    });
    expect(executing.preview).toBeNull();
  });

  it('does not let Edit or Revert pretend to cancel execution', () => {
    const ready = readyState();
    const approving = robotControlReducer(ready, { type: 'approve_started' });
    const executing = robotControlReducer(approving, {
      type: 'approve_resolved',
      acceptance: {
        requestId: 'request-1',
        operationId: 'operation-1',
        operationRevision: 1,
      },
    });

    expect(robotControlReducer(executing, {
      type: 'edit',
      positionsRad: [0.8, 0.5],
    })).toBe(executing);
    expect(robotControlReducer(executing, { type: 'revert' })).toBe(executing);
    expect(robotControlReducer(executing, { type: 'stop_started' }).phase)
      .toBe('stopping');
  });

  it('settles from owner-issued operation state and then permits Revert', () => {
    const ready = readyState();
    const approving = robotControlReducer(ready, { type: 'approve_started' });
    let state = robotControlReducer(approving, {
      type: 'approve_resolved',
      acceptance: {
        requestId: 'request-1',
        operationId: 'operation-1',
        operationRevision: 1,
      },
    });
    state = robotControlReducer(state, {
      type: 'operation_observed',
      operation: {
        operationId: 'operation-1',
        operationRevision: 8,
        state: 'completed',
        progress: 1,
        activeStopId: null,
        activeStopGeneration: null,
        terminalError: null,
      },
    });
    expect(state.phase).toBe('completed');
    expect(state.ghostStale).toBe(true);

    const reverted = robotControlReducer(state, { type: 'revert' });
    expect(reverted.phase).toBe('idle');
    expect(reverted.operation).toBeNull();
    expect(reverted.ghost).toBeNull();
  });

  it('invalidates all local preview and operation state on owner restart', () => {
    const ready = readyState();
    const restarted = robotControlReducer(ready, {
      type: 'status_refreshed',
      status: statusFixture('execution-owner-2'),
    });

    expect(restarted.phase).toBe('idle');
    expect(restarted.preview).toBeNull();
    expect(restarted.ghost).toBeNull();
    expect(restarted.error).toContain('restarted');
  });
});
