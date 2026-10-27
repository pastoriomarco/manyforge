import { buildJointMotionIntent, initialRobotControlState, robotControlReducer } from '../state/robotControl';
import type { CapabilityCommandPreviewReceipt, ExecutionOwnerStatus, MotionPreviewReceipt } from '../types/robotControl';

export function statusFixture(ownerEpoch = 'execution-owner-1'): ExecutionOwnerStatus {
  const status: ExecutionOwnerStatus = {
    ownerEpoch,
    cellId: 'cell.1',
    activePublicationGeneration: 1,
    graphRevision: 'sha256:graph',
    retainedBundleId: 'sha256:bundle',
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
      targetDescriptor: {
        schemaVersion: 'manyforge.manual_control_motion_descriptor.v5',
        ordinaryJerk: null,
        controllableComponentId: 'robot-a',
        commandTargetId: 'arm',
        componentRoleId: 'controlled_component',
        targetRoleId: 'command_target',
        frames: [
          { frameRoleId: 'base', compiledFrameName: 'base_link', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
          { frameRoleId: 'tcp', compiledFrameName: 'tool0', modelInstanceId: 'robot-model', modelRevision: 'sha256:model' },
        ],
        joints: [
          { sourceJointName: 'joint1', compiledJointName: 'robot__joint1', unit: 'rad', minPositionRad: -1, maxPositionRad: 1, maxVelocityRadS: 2, maxAccelerationRadS2: 3 },
          { sourceJointName: 'joint2', compiledJointName: 'robot__joint2', unit: 'rad', minPositionRad: -2, maxPositionRad: 2, maxVelocityRadS: 2, maxAccelerationRadS2: null },
        ],
        operationalJointLimits: [
          {
            sourceJointName: 'joint1', modelMaxVelocityRadS: 2,
            modelMaxAccelerationRadS2: 3, deploymentMaxVelocityRadS: 1.5,
            deploymentMaxAccelerationRadS2: 2.5, deploymentMaxJerkRadS3: 10,
            effectiveMaxVelocityRadS: 1.5, effectiveMaxAccelerationRadS2: 2.5,
            effectiveMaxJerkRadS3: 10,
          },
          {
            sourceJointName: 'joint2', modelMaxVelocityRadS: 2,
            modelMaxAccelerationRadS2: null, deploymentMaxVelocityRadS: null,
            deploymentMaxAccelerationRadS2: null, deploymentMaxJerkRadS3: null,
            effectiveMaxVelocityRadS: 2, effectiveMaxAccelerationRadS2: null,
            effectiveMaxJerkRadS3: null,
          },
        ],
        operationalLimit: null,
        contracts: {
          operationCapabilityId: 'execution.trajectory.execute',
          operationCapabilityDefinitionRevision: 'sha256:execution',
          commandSchema: null,
          controlPath: null,
          supportingProviders: [
            'planning.motion.cartesian_path',
            'planning.motion.joint_goal',
            'planning.motion.pose_goal',
          ].map((capabilityId) => ({
            capabilityId,
            capabilityDefinitionRevision: 'sha256:planning',
            capabilityProviderBindingId: capabilityId,
            capabilityProviderBindingRevision: 'sha256:provider',
            providerManifestId: 'planner',
            providerManifestRevision: 'sha256:manifest',
          })),
        },
      },
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
      targetDescriptor: {
        schemaVersion: 'manyforge.manual_control_gripper_descriptor.v3',
        controllableComponentId: 'robot-a', commandTargetId: 'gripper',
        componentRoleId: 'controlled_component', targetRoleId: 'command_target',
        description: 'Test gripper', commandKind: 'gripper_position',
        actuationKind: 'position_commanded', transportKind: 'resolved_binding',
        outputMode: 'commanded',
        controlInterfaceContractId: null, stateAuthorityId: null,
        stateAuthorityClass: null, completionPolicy: 'goal_fenced_target_reached',
        graspConfirmationPolicy: 'binding_feedback_required',
        sourceGroupName: 'gripper', openStateRef: 'open', closedStateRef: 'closed',
        commandDurationS: 0.5, touchLinks: ['finger_link'], mountFrame: 'tool0',
        attachFrame: 'tool0', tcpFrame: 'tool0', auxiliaryFrames: [],
        sourceCoordinateName: 'finger_joint', sourceKind: 'model_role',
        coordinateKind: 'linear_m', unit: 'm', minPosition: 0, maxPosition: 0.04,
        mimicFollowers: [], namedPresets: [],
        contracts: {
          operationCapabilityId: 'tool.gripper.command',
          operationCapabilityDefinitionRevision: 'sha256:gripper',
          commandSchema: { commandSchemaId: 'gripper.command_position', commandSchemaRevision: 'sha256:gripper-schema' },
          controlPath: null, supportingProviders: [],
        },
      },
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
      targetRoleId: 'command_target',
      controllableComponentId: 'robot-a',
      commandTargetId: 'gripper',
      sequence: 4,
      sourceEpoch: 'gripper-source-1',
      sourceCoordinateName: 'finger_joint',
      position: 0.02,
      coordinateKind: 'linear_m',
      complete: true,
    },
    manualOperations: [],
    runtimeScaleControl: null,
    cellStopAction: {
      available: true,
      unavailableReason: null,
      expectedStopGeneration: 0,
      latestTerminal: null,
      fault: null,
    },
    components: [],
    directControlTargets: [],
    directControlGripperTargets: [],
    observedMotions: [],
    observedGrippers: [],
  };
  // One component owning the arm and gripper targets (H1a-IF.2 inventory).
  const motion = status.directControl!;
  const gripper = status.directControlGripper!;
  status.components = [{
    controllableComponentId: motion.controllableComponentId,
    commandEligible: true,
    unavailableReason: null,
    unsupportedTargets: [],
  }];
  status.directControlTargets = [motion];
  status.directControlGripperTargets = [gripper];
  status.observedMotions = [status.observedMotion!];
  status.observedGrippers = [{
    ...status.observedGripper!,
    targetRoleId: gripper.targetRoleId,
    controllableComponentId: gripper.controllableComponentId,
    commandTargetId: gripper.commandTargetId,
  }];
  return status;
}

export function receiptFixture(
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
    programId: status.directControl!.programId,
    programRevision: status.directControl!.programRevision,
    programBindingId: status.directControl!.programBindingId,
    programRunId: status.directControl!.programRunId,
    graphRevision: status.graphRevision,
    retainedBundleId: status.retainedBundleId,
    sceneRevision: { scene_id: 'scene-1', revision_id: 3, content_hash: 'sha256:scene' },
    componentRoleId: status.directControl!.componentRoleId,
    targetRoleId: status.directControl!.targetRoleId,
    requiredCapabilityId: status.directControl!.operationCapabilityId,
    sourceJointNames: status.directControl!.joints.map(
      (joint) => joint.sourceJointName,
    ),
    startPositionsRad: [...status.observedMotion!.positionsRad],
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

export function readyState() {
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

export function executingState() {
  const approving = robotControlReducer(
    readyState(),
    { type: 'approve_started' },
  );
  const status = structuredClone(approving.status!);
  status.manualOperations = [{
    programRunId: status.directControl!.programRunId,
    operationId: 'operation-1',
    operationRevision: 1,
    resident: true,
    state: 'queued',
    progress: 0,
    activeStopId: null,
    activeStopGeneration: null,
    terminalError: null,
  }];
  return robotControlReducer(approving, {
    type: 'status_refreshed',
    status,
  });
}

export function gripperReceiptFixture(
  status: ExecutionOwnerStatus,
  requestId: string,
  position: number,
): CapabilityCommandPreviewReceipt {
  const gripper = status.directControlGripper;
  if (gripper === null) throw new Error('gripper receipt fixture needs a gripper');
  return {
    schemaVersion: 'manyforge.direct_control_capability_command_preview.v1',
    receiptDigest: 'sha256:gripper-receipt',
    intentDigest: 'sha256:gripper-intent',
    requestId,
    requestOwnerEpoch: status.ownerEpoch,
    programId: gripper.programId,
    programRevision: gripper.programRevision,
    programBindingId: gripper.programBindingId,
    programRunId: gripper.programRunId,
    graphRevision: status.graphRevision,
    retainedBundleId: status.retainedBundleId,
    componentRoleId: gripper.componentRoleId,
    targetRoleId: gripper.targetRoleId,
    requiredCapabilityId: gripper.operationCapabilityId,
    commandSchemaId: gripper.commandSchemaId,
    commandSchemaRevision: gripper.commandSchemaRevision,
    sourceCoordinateName: gripper.sourceCoordinateName,
    sourceKind: gripper.sourceKind,
    coordinateKind: gripper.coordinateKind,
    targetPosition: position,
    minPosition: gripper.minPosition,
    maxPosition: gripper.maxPosition,
    commitEligible: true,
  };
}
