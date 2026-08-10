export interface SceneRevisionValue {
  scene_id: string;
  revision_id: number;
  content_hash: string;
}

export interface Pose3Value {
  position_m: { x_m: number; y_m: number; z_m: number };
  orientation: { x: number; y: number; z: number; w: number };
}

export interface DirectControlJoint {
  sourceJointName: string;
  minPositionRad: number;
  maxPositionRad: number;
  maxVelocityRadS: number;
  maxAccelerationRadS2: number | null;
}

export interface DirectControlMotionStatus {
  controllableComponentId: string;
  commandTargetId: string;
  componentRoleId: string;
  targetRoleId: string;
  baseFrameRoleId: string;
  tcpFrameRoleId: string;
  operationCapabilityId: string;
  planningCapabilityIds: string[];
  frameRoleIds: string[];
  programId: string;
  programRevision: string;
  programBindingId: string;
  programRunId: string;
  joints: DirectControlJoint[];
}

export type CoordinateKind = 'angular_rad' | 'linear_m';
export type ResourceSourceKind = 'model_role' | 'component_local';

export interface DirectControlGripperStatus {
  controllableComponentId: string;
  commandTargetId: string;
  componentRoleId: string;
  targetRoleId: string;
  operationCapabilityId: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
  programRunId: string;
  commandSchemaId: string;
  commandSchemaRevision: string;
  sourceCoordinateName: string;
  sourceKind: ResourceSourceKind;
  coordinateKind: CoordinateKind;
  minPosition: number | null;
  maxPosition: number | null;
}

export interface ObservedMotionTarget {
  targetRoleId: string;
  controllableComponentId: string;
  commandTargetId: string;
  sequence: number;
  sourceEpoch: string;
  sourceJointNames: string[];
  positionsRad: number[];
  complete: boolean;
  currentTcpPose: Pose3Value;
}

export interface ObservedGripperTarget {
  sequence: number;
  sourceEpoch: string;
  sourceCoordinateName: string;
  position: number;
  coordinateKind: CoordinateKind;
  complete: boolean;
}

export interface ExecutionOwnerStatus {
  ownerEpoch: string;
  graphRevision: string;
  retainedBundleId: string;
  sceneRevision: SceneRevisionValue;
  directControl: DirectControlMotionStatus;
  directControlGripper: DirectControlGripperStatus;
  observedMotion: ObservedMotionTarget;
  observedGripper: ObservedGripperTarget | null;
}

export type DirectControlMotionSpec =
  | {
      kind: 'joint_goal';
      joint_names: string[];
      positions_rad: number[];
    }
  | {
      kind: 'pose_goal';
      goal: Pose3Value;
    }
  | {
      kind: 'cartesian_path';
      waypoints: Pose3Value[];
      max_translation_step_m: number;
      max_rotation_step_rad: number;
      speed_mode: 'bounded';
      max_linear_speed_mps: number;
      max_angular_speed_rad_s: number;
      max_linear_acceleration_mps2: number;
      max_angular_acceleration_rad_s2: number;
      fallback_policy: 'fail_closed';
    };

export interface MotionParticipantPayload {
  kind: 'motion';
  move_id: string;
  base_frame_role_id: string;
  tcp_frame_role_id: string;
  tolerances: {
    joint_position_tol_rad: number;
    translation_tol_m: number;
    rotation_tol_rad: number;
  };
  speed_profile: {
    velocity_scale: number;
    acceleration_scale: number;
    max_tcp_linear_speed_mps: number | null;
  };
  timeouts: {
    planning_timeout_s: number;
    execution_timeout_s: number;
  };
  cache_mode: 'default';
  planning_profile: 'first_feasible' | 'balanced' | 'quality_bounded';
  target_mode: 'absolute';
  target_reference: null;
  offset_reference: null;
  motion: DirectControlMotionSpec;
  contact_policy_id: null;
  per_joint_dynamics_limits: Record<string, never>;
  motion_interrupt_scopes: [];
  runtime_object_bindings: [];
}

export interface CapabilityCommandParticipantPayload {
  kind: 'capability_command';
  command_schema_id: string;
  command_schema_revision: string;
  command_value: {
    kind: 'gripper_position';
    position:
      | { kind: 'angular_rad'; value_rad: number }
      | { kind: 'linear_m'; value_m: number };
    max_effort: null;
  };
  completion_timeout_s: number;
  caller_options: [{ option_id: 'command_duration_s'; value_s: number }];
}

export interface DirectControlOperationIntent {
  request_context: {
    request_id: string;
    request_owner_epoch: string;
    source_label: 'composer_ui';
    correlation_id: string | null;
  };
  program_id: string;
  program_revision: string;
  program_binding_id: string;
  program_run_id: string;
  expected_graph_revision: string;
  expected_retained_bundle_id: string;
  expected_scene_revision: SceneRevisionValue;
  participants: [{
    component_role_id: string;
    target_role_id: string;
    required_capability_id: string;
    action_span: null;
    payload: MotionParticipantPayload | CapabilityCommandParticipantPayload;
  }];
  coordination_policy: 'independent';
}

export type JointMotionOperationIntent = DirectControlOperationIntent;

export interface MotionPreviewReceipt {
  schemaVersion: 'manyforge.direct_control_motion_preview.v1';
  receiptDigest: string;
  intentDigest: string;
  requestId: string;
  requestOwnerEpoch: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
  programRunId: string;
  graphRevision: string;
  retainedBundleId: string;
  sceneRevision: SceneRevisionValue;
  componentRoleId: string;
  targetRoleId: string;
  requiredCapabilityId: string;
  sourceJointNames: string[];
  startPositionsRad: number[];
  finalPositionsRad: number[];
  finalTcpPose: Pose3Value;
  collisionWorldRevision: {
    revision_id: number;
    content_hash: string;
  };
  commitEligible: true;
}

export interface CapabilityCommandPreviewReceipt {
  schemaVersion: 'manyforge.direct_control_capability_command_preview.v1';
  receiptDigest: string;
  intentDigest: string;
  requestId: string;
  requestOwnerEpoch: string;
  programId: string;
  programRevision: string;
  programBindingId: string;
  programRunId: string;
  graphRevision: string;
  retainedBundleId: string;
  sceneRevision: SceneRevisionValue;
  componentRoleId: string;
  targetRoleId: string;
  requiredCapabilityId: string;
  commandSchemaId: string;
  commandSchemaRevision: string;
  sourceCoordinateName: string;
  sourceKind: ResourceSourceKind;
  coordinateKind: CoordinateKind;
  targetPosition: number;
  minPosition: number | null;
  maxPosition: number | null;
  commitEligible: true;
}

export type DirectControlPreviewReceipt =
  | MotionPreviewReceipt
  | CapabilityCommandPreviewReceipt;

export interface OperationAcceptance {
  requestId: string;
  operationId: string;
  operationRevision: number;
}

export type OperationStateName =
  | 'queued'
  | 'resolving'
  | 'planning'
  | 'validating'
  | 'prepared'
  | 'dispatching'
  | 'executing'
  | 'paused'
  | 'stopping'
  | 'completed'
  | 'stopped'
  | 'failed'
  | 'interrupted'
  | 'intervention_required'
  | 'cancelled_pre_dispatch';

export interface OperationObservation {
  operationId: string;
  operationRevision: number;
  state: OperationStateName;
  progress: number;
  activeStopId: string | null;
  activeStopGeneration: number | null;
  terminalError: string | null;
}
