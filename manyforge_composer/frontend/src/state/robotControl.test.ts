import { describe, expect, it } from 'vitest';
import {
  buildCartesianMotionIntent,
  buildGripperCommandIntent,
  buildJointMotionIntent,
  buildPoseMotionIntent,
  initialRobotControlState,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  ROBOT_CONTROL_OPERATION_POLL_MS,
  robotControlReducer,
} from './robotControl';
import type {
  CapabilityCommandPreviewReceipt,
  ExecutionOwnerStatus,
  MotionPreviewReceipt,
} from '../types/robotControl';

function statusFixture(ownerEpoch = 'execution-owner-1'): ExecutionOwnerStatus {
  return {
    ownerEpoch,
    cellId: 'cell.1',
    activePublicationGeneration: 1,
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
      planningCapabilityIds: [
        'planning.motion.cartesian_path',
        'planning.motion.joint_goal',
        'planning.motion.pose_goal',
      ],
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
    directControlGripper: {
      controllableComponentId: 'robot-a',
      commandTargetId: 'gripper',
      componentRoleId: 'controlled_component',
      targetRoleId: 'command_target',
      operationCapabilityId: 'tool.gripper.command',
      programId: 'manyforge.direct_control.gripper',
      programRevision: 'sha256:direct-gripper-program',
      programBindingId: 'sha256:direct-gripper-binding',
      programRunId: 'direct-control-gripper-owner-1',
      commandSchemaId: 'gripper.command_position',
      commandSchemaRevision: 'sha256:gripper-schema',
      sourceCoordinateName: 'finger_joint',
      sourceKind: 'model_role',
      coordinateKind: 'linear_m',
      minPosition: 0,
      maxPosition: 0.04,
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
      currentTcpPose: {
        position_m: { x_m: 0.1, y_m: 0.2, z_m: 0.3 },
        orientation: { x: 0, y: 0, z: 0, w: 1 },
      },
    },
    observedGripper: {
      sequence: 4,
      sourceEpoch: 'gripper-source-1',
      sourceCoordinateName: 'finger_joint',
      position: 0.02,
      coordinateKind: 'linear_m',
      complete: true,
    },
    cellStopAction: {
      available: true,
      unavailableReason: null,
      expectedStopGeneration: 0,
      latestTerminal: null,
      fault: null,
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

function gripperReceiptFixture(
  status: ExecutionOwnerStatus,
  requestId: string,
  position: number,
): CapabilityCommandPreviewReceipt {
  return {
    schemaVersion: 'manyforge.direct_control_capability_command_preview.v1',
    receiptDigest: 'sha256:gripper-receipt',
    intentDigest: 'sha256:gripper-intent',
    requestId,
    requestOwnerEpoch: status.ownerEpoch,
    programId: status.directControlGripper.programId,
    programRevision: status.directControlGripper.programRevision,
    programBindingId: status.directControlGripper.programBindingId,
    programRunId: status.directControlGripper.programRunId,
    graphRevision: status.graphRevision,
    retainedBundleId: status.retainedBundleId,
    sceneRevision: { ...status.sceneRevision },
    componentRoleId: status.directControlGripper.componentRoleId,
    targetRoleId: status.directControlGripper.targetRoleId,
    requiredCapabilityId: status.directControlGripper.operationCapabilityId,
    commandSchemaId: status.directControlGripper.commandSchemaId,
    commandSchemaRevision: status.directControlGripper.commandSchemaRevision,
    sourceCoordinateName: status.directControlGripper.sourceCoordinateName,
    sourceKind: status.directControlGripper.sourceKind,
    coordinateKind: status.directControlGripper.coordinateKind,
    targetPosition: position,
    minPosition: status.directControlGripper.minPosition,
    maxPosition: status.directControlGripper.maxPosition,
    commitEligible: true,
  };
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
    if (participant.payload.kind !== 'motion') {
      throw new Error('expected motion payload');
    }

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
        resident: true,
        state: 'completed',
        progress: 1,
        activeStopId: null,
        activeStopGeneration: null,
        terminalError: null,
      },
    });
    expect(state.phase).toBe('completed');
    expect(state.ghostStale).toBe(true);

    const observed = statusFixture();
    observed.observedMotion.positionsRad = [0.24, 0.49];
    observed.observedGripper = {
      ...observed.observedGripper!,
      position: 0.031,
    };
    state = robotControlReducer(state, {
      type: 'status_refreshed',
      status: observed,
    });
    expect(state.draftPositionsRad).toEqual([0.24, 0.49]);
    expect(state.draftGripperPosition).toBe(0.031);

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

  it('preserves absolute pose and explicit current-to-target Cartesian meaning', () => {
    const status = statusFixture();
    const target = {
      position_m: { x_m: 0.4, y_m: -0.1, z_m: 0.2 },
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    };
    const pose = buildPoseMotionIntent(status, target, 'pose-request');
    const cartesian = buildCartesianMotionIntent(
      status,
      target,
      'cartesian-request',
    );
    const posePayload = pose.participants[0].payload;
    const cartesianPayload = cartesian.participants[0].payload;
    if (
      posePayload.kind !== 'motion'
      || cartesianPayload.kind !== 'motion'
    ) throw new Error('expected motion payloads');

    expect(posePayload.motion).toEqual({ kind: 'pose_goal', goal: target });
    expect(cartesianPayload.motion).toMatchObject({
      kind: 'cartesian_path',
      waypoints: [status.observedMotion.currentTcpPose, target],
      fallback_policy: 'fail_closed',
    });
  });

  it('authors gripper commands with the exact schema and source-local unit', () => {
    const status = statusFixture();
    const intent = buildGripperCommandIntent(status, 0.03, 'gripper-request');
    const payload = intent.participants[0].payload;
    if (payload.kind !== 'capability_command') {
      throw new Error('expected capability command');
    }

    expect(intent.program_id).toBe('manyforge.direct_control.gripper');
    expect(payload.command_schema_id).toBe('gripper.command_position');
    expect(payload.command_value.position).toEqual({
      kind: 'linear_m',
      value_m: 0.03,
    });
  });

  it('discards an unsubmitted preview without planning on a mode change', () => {
    const ready = readyState();
    const changed = robotControlReducer(ready, {
      type: 'select_mode',
      mode: 'pose_goal',
    });

    expect(changed.phase).toBe('idle');
    expect(changed.intent).toBeNull();
    expect(changed.preview).toBeNull();
    expect(changed.ghost).toBeNull();
    expect(changed.ghostStale).toBe(false);
    expect(changed.draftPositionsRad).toEqual(
      changed.status?.observedMotion.positionsRad,
    );
  });

  it('accepts only the latest exact gripper preview and fabricates no ghost', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit_gripper',
      position: 0.03,
    });
    const intent = buildGripperCommandIntent(status, 0.03, 'gripper-new');
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: state.generation,
      intent,
    });
    const staleSceneReceipt = gripperReceiptFixture(
      status,
      'gripper-new',
      0.03,
    );
    staleSceneReceipt.sceneRevision.revision_id += 1;
    const rejected = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: staleSceneReceipt,
    });
    expect(rejected.phase).toBe('blocked');
    expect(rejected.preview).toBeNull();
    state = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: gripperReceiptFixture(status, 'gripper-new', 0.03),
    });

    expect(state.phase).toBe('ready');
    expect(state.preview?.requestId).toBe('gripper-new');
    expect(state.ghost).toBeNull();
  });

  it('keeps approval fail-closed after the current preview is rejected', () => {
    const ready = readyState();
    const planning = robotControlReducer(ready, {
      type: 'edit',
      positionsRad: [0.6, 0.5],
    });
    const blocked = robotControlReducer(planning, {
      type: 'preview_rejected',
      generation: planning.generation,
      error: 'state changed before validation',
    });

    expect(blocked.phase).toBe('blocked');
    expect(blocked.intent).toBeNull();
    expect(blocked.preview).toBeNull();
    expect(robotControlReducer(blocked, { type: 'approve_started' }))
      .toBe(blocked);
  });

  it('rejects a late receipt that targets an older request identity', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit',
      positionsRad: [0.3, 0.1],
    });
    state = robotControlReducer(state, {
      type: 'preview_started',
      generation: state.generation,
      intent: buildJointMotionIntent(status, [0.3, 0.1], 'new-request'),
    });
    const blocked = robotControlReducer(state, {
      type: 'preview_resolved',
      generation: state.generation,
      receipt: receiptFixture(status, 'old-request', [0.3, 0.1]),
    });

    expect(blocked.phase).toBe('blocked');
    expect(blocked.preview).toBeNull();
  });

  it('Revert clears a settled local command without issuing cancellation state', () => {
    const status = statusFixture();
    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    state = robotControlReducer(state, {
      type: 'edit_gripper',
      position: 0.03,
    });
    const reverted = robotControlReducer(state, { type: 'revert' });

    expect(reverted.phase).toBe('idle');
    expect(reverted.operation).toBeNull();
    expect(reverted.preview).toBeNull();
    expect(reverted.draftGripperPosition).toBe(
      status.observedGripper?.position,
    );
  });

  it('authors the first component-local command without inventing state or limits', () => {
    const status = statusFixture();
    status.directControlGripper = {
      ...status.directControlGripper,
      sourceCoordinateName: 'gripper_position',
      sourceKind: 'component_local',
      minPosition: null,
      maxPosition: null,
    };
    status.observedGripper = null;

    let state = robotControlReducer(initialRobotControlState, {
      type: 'status_resolved',
      status,
    });
    expect(state.draftGripperPosition).toBeNull();
    state = robotControlReducer(state, {
      type: 'edit_gripper',
      position: 0.01,
    });
    expect(state.phase).toBe('planning');

    const intent = buildGripperCommandIntent(status, 0.01, 'local-command-1');
    const payload = intent.participants[0].payload;
    expect(payload.kind).toBe('capability_command');
    if (payload.kind !== 'capability_command') return;
    expect(payload.caller_options[0].value_s).toBe(0);
  });
});
