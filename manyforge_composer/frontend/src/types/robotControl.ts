export interface SceneRevisionValue {
  scene_id: string;
  revision_id: number;
  content_hash: string;
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

export interface ObservedMotionTarget {
  targetRoleId: string;
  controllableComponentId: string;
  commandTargetId: string;
  sequence: number;
  sourceEpoch: string;
  sourceJointNames: string[];
  positionsRad: number[];
  complete: boolean;
}

export interface ExecutionOwnerStatus {
  ownerEpoch: string;
  graphRevision: string;
  retainedBundleId: string;
  sceneRevision: SceneRevisionValue;
  directControl: DirectControlMotionStatus;
  observedMotion: ObservedMotionTarget;
}

export interface JointMotionOperationIntent {
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
    payload: {
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
      motion: {
        kind: 'joint_goal';
        joint_names: string[];
        positions_rad: number[];
      };
      contact_policy_id: null;
      per_joint_dynamics_limits: Record<string, never>;
      motion_interrupt_scopes: [];
      runtime_object_bindings: [];
    };
  }];
  coordination_policy: 'independent';
}

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
  finalTcpPose: {
    position_m: { x_m: number; y_m: number; z_m: number };
    orientation: { x: number; y: number; z: number; w: number };
  };
  collisionWorldRevision: {
    revision_id: number;
    content_hash: string;
  };
  commitEligible: true;
}

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
