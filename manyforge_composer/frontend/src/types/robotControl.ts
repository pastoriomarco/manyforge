import type { IoSignalBinding, SystemSignalProjection } from '../api/parsers/ioSignals';

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

export interface DirectControlOperationalJointLimit {
  sourceJointName: string;
  modelMaxVelocityRadS: number;
  modelMaxAccelerationRadS2: number | null;
  deploymentMaxVelocityRadS: number | null;
  deploymentMaxAccelerationRadS2: number | null;
  deploymentMaxJerkRadS3: number | null;
  effectiveMaxVelocityRadS: number;
  effectiveMaxAccelerationRadS2: number | null;
  effectiveMaxJerkRadS3: number | null;
}

export interface ManualControlProviderContract {
  capabilityId: string;
  capabilityDefinitionRevision: string;
  capabilityProviderBindingId: string;
  capabilityProviderBindingRevision: string;
  providerManifestId: string;
  providerManifestRevision: string;
}

export interface ManualControlContracts {
  operationCapabilityId: string;
  operationCapabilityDefinitionRevision: string;
  commandSchema: {
    commandSchemaId: string;
    commandSchemaRevision: string;
  } | null;
  controlPath: {
    controlBindingId: string;
    controlBindingRevision: string;
    controlInterfaceContractId: string;
    controlInterfaceRevision: string;
    controllerTopologyId: string;
    topologyRevision: string;
    stopEvidencePolicyId: string;
    stopEvidencePolicyRevision: string;
  } | null;
  supportingProviders: ManualControlProviderContract[];
}

export interface TcpEntryIdentity {
  componentId: string;
  targetId: string;
  frameComponentId: string;
  frameRoleId: string;
}

/** A stable in-memory key. The display name is never parsed or sent. */
export function tcpEntryIdentityKey(entry: TcpEntryIdentity): string {
  return JSON.stringify([entry.componentId, entry.targetId, entry.frameComponentId, entry.frameRoleId]);
}

/** One TCP Robot Control can move a robot at, identified by its exact owner. */
export interface DirectControlTcp {
  entry: TcpEntryIdentity;
  controllableComponentId: string;
  commandTargetId: string;
  frameRoleId: string;
  /** Display-only; IDs may contain dots and this value is never parsed. */
  qualifiedName: string;
  compiledFrameName: string;
  modelInstanceId: string;
  modelRevision: string;
  isDefault: boolean;
  mountedHere: boolean;
  mountedElsewhere: boolean;
  contactBodyRoleIds: string[];
}

export interface ManualControlMotionDescriptor {
  schemaVersion: 'manyforge.manual_control_motion_descriptor.v8';
  controllableComponentId: string;
  /** Exact component selected by Applied at this robot's flange; null if empty. */
  endEffectorComponentId: string | null;
  commandTargetId: string;
  frames: Array<{
    frameComponentId: string;
    frameRoleId: string;
    compiledFrameName: string;
    modelInstanceId: string;
    modelRevision: string;
  }>;
  /** All Deployment TCP rows for the robot and tools, with the active default
   * (the end-effector TCP) first; only carried rows have observed poses. */
  tcps: DirectControlTcp[];
  joints: Array<DirectControlJoint & { compiledJointName: string; unit: 'rad' }>;
  operationalJointLimits: DirectControlOperationalJointLimit[];
  ordinaryJerk: {
    enabled: boolean;
    presetReferenceTimeS: number;
    jointLimits: Array<{
      sourceJointName: string;
      source: 'off' | 'override' | 'preset';
      maximumRadS3: number | null;
      presetRadS3: number | null;
    }>;
  } | null;
  operationalLimit: {
    maximumTcpLinearSpeedMps: number;
  } | null;
  contracts: ManualControlContracts;
}

export interface ManualControlGripperDescriptor {
  schemaVersion: 'manyforge.manual_control_gripper_descriptor.v5';
  controllableComponentId: string;
  commandTargetId: string;
  description: string | null;
  commandKind: 'gripper_position';
  actuationKind: 'position_commanded' | 'pneumatic' | null;
  outputMode: 'commanded' | 'none' | null;
  transportKind: 'resolved_binding';
  controlInterfaceContractId: string | null;
  stateAuthorityId: string | null;
  stateAuthorityClass: 'measured' | 'commanded' | 'predicted' | null;
  completionPolicy: 'goal_fenced_target_reached' | 'output_delivery' | 'no_output';
  graspConfirmationPolicy: 'binding_feedback_required' | 'not_required';
  /** The Applied row's opt-in grasp confirmation; absent when not required. */
  graspConfirmation?: 'required';
  sourceGroupName: string | null;
  sourceCoordinateName: string;
  sourceKind: ResourceSourceKind;
  coordinateKind: CoordinateKind;
  unit: 'rad' | 'm';
  minPosition: number | null;
  maxPosition: number | null;
  openStateRef: string | null;
  closedStateRef: string | null;
  commandDurationS: number | null;
  /** Mapped physical outputs from the Applied gripper Binding; absent for legacy/unmapped tools. */
  pneumaticOutputs?: Array<{
    controllableComponentId: string;
    signalName: string;
    openValue: boolean;
    closeValue: boolean;
  }>;
  touchLinks: string[];
  mountFrame: string | null;
  attachFrame: string | null;
  tcpFrame: string | null;
  auxiliaryFrames: string[];
  mimicFollowers: Array<{
    sourceCoordinateName: string;
    compiledCoordinateName: string;
    multiplier: number;
    offset: number;
  }>;
  namedPresets: Array<{
    name: string;
    masterPosition: number;
    coordinates: Array<{ sourceCoordinateName: string; position: number }>;
  }>;
  contracts: ManualControlContracts;
}

/** One stage of a target's qualified Stop chain, in chain order. */
export interface TargetStopResponseStage {
  stageId: string;
  stageKind: string;
  commandKind: string;
}

/** `target.command.stop_response`: the Applied qualified Stop chain of one
 * motion target (the same artifact Program Stop validation uses), exposed
 * before Enable; it does not assert the commands are ready. */
export interface TargetStopResponse {
  controlBindingId: string;
  controlBindingRevision: string;
  controlRouteId: string;
  nominalStopMode: string | null;
  stages: TargetStopResponseStage[];
}

export interface DirectControlMotionStatus {
  activeTcp: { entry: TcpEntryIdentity | null; display: string | null; ownerEpoch: string;
    registrationGeneration: number; sequence: number } | null;
  controllableComponentId: string;
  commandTargetId: string;
  baseFrame: { componentId: string; frameId: string };
  baseFrameRoleId: string;
  /** The end-effector TCP's frame role, null when the robot has none;
   * in a selected-status projection, the selected TCP's frame role. */
  tcpFrameRoleId: string | null;
  /** Selected-status projection: exact identity of the selected TCP row. */
  selectedTcpEntryKey?: string;
  operationCapabilityId: string;
  planningCapabilityIds: string[];
  frameRoleIds: string[];
  programId: string;
  programRevision: string;
  programBindingId: string;
  programRunId: string;
  joints: DirectControlJoint[];
  targetDescriptor: ManualControlMotionDescriptor;
  /** Present only when the owner publishes the target's qualified Stop chain. */
  stopResponse?: TargetStopResponse;
}

export type CoordinateKind = 'angular_rad' | 'linear_m';
export type ResourceSourceKind = 'model_role' | 'component_local';

export interface DirectControlGripperStatus {
  controllableComponentId: string;
  commandTargetId: string;
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
  targetDescriptor: ManualControlGripperDescriptor;
}

export interface ObservedMotionTarget {
  controllableComponentId: string;
  commandTargetId: string;
  sequence: number;
  sourceEpoch: string;
  sourceJointNames: string[];
  positionsRad: number[];
  complete: boolean;
  /** The end-effector TCP's pose, null when the robot has none; in a
   * selected-status projection, the selected TCP's. */
  currentTcpPose: Pose3Value | null;
  /** TCP-C: carried TCP poses in the base frame, by exact entry identity key. */
  tcpPoses: Record<string, Pose3Value>;
}

export interface ObservedGripperTarget {
  controllableComponentId: string;
  commandTargetId: string;
  sequence: number;
  sourceEpoch: string;
  sourceCoordinateName: string;
  position: number;
  coordinateKind: CoordinateKind;
  complete: boolean;
}

export interface StopCertificateReference {
  stopCertificateId: string;
  stopCertificateRevision: number;
  stopId: string;
  stopGeneration: number;
}

export interface CellStopStageSummary {
  operationId: string;
  participantId: string;
  stageId: string;
  stageIndex: number;
  actionClass: 'controlled' | 'software_hard' | 'safety_system_request';
  outcome: 'succeeded' | 'partial' | 'failed' | 'superseded_no_effect';
  executedMode: 'soft_on_path' | 'spring_back_hold' | 'spring_back_fast' | 'hard_deceleration' | null;
  substitutionReason: string | null;
}

export interface CellStopActionStatus {
  available: boolean;
  unavailableReason: string | null;
  expectedStopGeneration: number;
  latestTerminal: {
    certificate: StopCertificateReference;
    result: 'succeeded_nominal' | 'succeeded_escalated' | 'partial' | 'failed_uncontained';
    escalated: boolean;
    hardStopFloorUsed: boolean;
    executedStages: CellStopStageSummary[];
  } | null;
}

/** One component listed by the execution owner. A present but
 * ineligible component stays listed; the backend refuses its commands. */
export interface ExecutionComponentStatus {
  controllableComponentId: string;
  commandEligible: boolean;
  /** An existing owner refusal code exactly when not command-eligible. */
  unavailableReason: string | null;
  /** Advertised but unsupported capabilities: visible, never commandable. */
  unsupportedTargets: Array<{ commandTargetId: string; operationCapabilityId: string }>;
}

export interface ExecutionOwnerStatus {
  ownerEpoch: string;
  cellId: string;
  activePublicationGeneration: number;
  graphRevision: string;
  retainedBundleId: string;
  components: ExecutionComponentStatus[];
  /** The selected motion/gripper target (a UI projection, not a wire
   * primary). Null when the selected component has no such capability. */
  directControl: DirectControlMotionStatus | null;
  directControlGripper: DirectControlGripperStatus | null;
  /** Every addressed target, each owned by one listed component. */
  directControlTargets: DirectControlMotionStatus[];
  directControlGripperTargets: DirectControlGripperStatus[];
  observedMotion: ObservedMotionTarget | null;
  observedGripper: ObservedGripperTarget | null;
  observedMotions: ObservedMotionTarget[];
  observedGrippers: ObservedGripperTarget[];
  manualOperations: (OperationObservation & { programRunId: string })[];
  cellStopAction: CellStopActionStatus;
  runtimeScaleControl: RuntimeScaleControlStatus | null;
  /** `targets.io_signal_bindings` when the owner publishes it (static per activation). */
  ioSignalBindings?: IoSignalBinding[];
  /** Normalized `targets.system_signals`; empty when the owner omits it. */
  systemSignals: SystemSignalProjection[];
}

export interface ExecutionIdentity {
  ownerEpoch: string;
  cellId: string;
  activePublicationGeneration: number;
  controlManagementMode: 'pending' | 'direct' | 'component_lifecycle';
  graphRevision: string;
  retainedBundleId: string;
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
      speed_mode?: 'constant_speed' | 'time_optimal';
      max_angular_speed_rad_s: number;
      max_linear_acceleration_mps2: number;
      max_angular_acceleration_rad_s2: number;
      fallback_policy: 'fail_closed';
      /** One per waypoint; 0 keeps the corner exact. */
      blend_radii_m: number[];
    };

export interface MotionParticipantPayload {
  kind: 'motion';
  move_id: string;
  base_frame: { component_id: string; frame_id: string };
  tcp_endpoint: { kind: 'current' };
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
  target_mode: 'absolute';
  target_reference: null;
  offset_reference: null;
  motion: DirectControlMotionSpec;
  contact_policy_id: null;
  per_joint_dynamics_limits: Record<string, {
    max_velocity_rad_s?: number;
    max_acceleration_rad_s2?: number;
    max_jerk_rad_s3?: number;
  }>;
  motion_interrupt_scopes: [];
  runtime_object_bindings: [];
  /** Collision clearance (m) for this move; absent keeps the Deployment's. */
  clearance_m?: number;
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

/** Opaque Kernel coordinates; copied into the intent, never minted by the UI. */
export interface ManualProgramHandoverContext {
  program_runner_owner_epoch: string;
  program_run_id: string;
  claim_generation: number;
  selection_revision: number;
}

export interface ManualControlAvailability {
  requestId: string;
  requestOwnerEpoch: string;
  intentDigest: string;
  available: boolean;
  unavailableReason: string | null;
  cleanupRequired: boolean;
  manualHandover: ManualProgramHandoverContext | null;
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
  /** The latest Scene is used; callers name no Scene revision. */
  expected_scene_revision: null;
  participants: [{
    target: { component_id: string; command_target_id: string };
    required_capability_id: string;
    action_span: null;
    payload: MotionParticipantPayload | CapabilityCommandParticipantPayload;
  }];
  coordination_policy: 'independent';
  manual_handover?: ManualProgramHandoverContext;
}

export type JointMotionOperationIntent = DirectControlOperationIntent;

export type DirectControlSetTcpIntent = Omit<DirectControlOperationIntent, 'participants'> & {
  participants: [{
    target: { component_id: string; command_target_id: string };
    required_capability_id: string;
    action_span: null;
    payload: { kind: 'set_tcp'; tcp: { kind: 'entry'; target: {
      component_id: string; command_target_id: string;
    }; frame: { component_id: string; frame_id: string } } };
  }];
};

export interface MotionPreviewPath {
  schemaVersion?: 'manyforge.motion_preview_diagnostic.v2';
  kind?: 'validated_path' | 'collision_disabled_path' | 'collision_disabled_ik_pose' | 'collision_disabled_ik_path';
  reason?: string;
  sourceJointNames: string[];
  pairs: { bodyA: string; bodyB: string }[];
  samples: {
    timeFromStartS: number;
    positionsRad: number[];
    contacts: { pairIndex: number; disposition: 'allowed' | 'refused' | 'unknown' }[];
    status: 'checked' | 'unknown';
    clearanceViolations?: { pairIndex: number; distanceM: number; requiredClearanceM: number }[];
  }[];
}

export interface MotionPreviewReceipt {
  schemaVersion: 'manyforge.direct_control_motion_preview.v4';
  pathPreview: MotionPreviewPath;
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
  target: { componentId: string; commandTargetId: string };
  resolvedTcp: {
    entry: {
      target: { componentId: string; commandTargetId: string };
      frame: { componentId: string; frameId: string };
    };
    source: 'active_tcp' | 'joint_flange_fallback' | 'explicit_entry';
    ownerEpoch: string;
    registrationGeneration: number;
    sequence: number;
  };
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
  schemaVersion: 'manyforge.direct_control_capability_command_preview.v2';
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
  target: { componentId: string; commandTargetId: string };
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

export type OperationStateName =
  | 'queued'
  | 'resolving'
  | 'planning'
  | 'validating'
  | 'prepared'
  | 'dispatching'
  | 'executing'
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
  resident: boolean;
  state: OperationStateName;
  progress: number;
  activeStopId: string | null;
  activeStopGeneration: number | null;
  terminalError: string | null;
  /** Exact target identity when the owner exposes operation participants. */
  controllableComponentId?: string;
  commandTargetId?: string;
}

export type RuntimeScaleApplicationState =
  | 'pending'
  | 'applied'
  | 'rejected'
  | 'failed';

export interface RuntimeScaleControlSingularSelector {
  kind: 'singular';
  programBindingId: string;
  componentId: string;
  commandTargetId: string;
}

export interface RuntimeScaleControlGroupSelector {
  kind: 'group';
  programBindingId: string;
  groupTargets: RuntimeScaleControlTarget[];
}

export interface RuntimeScaleControlTarget {
  componentId: string;
  commandTargetId: string;
}

export type RuntimeScaleControlSelector =
  | RuntimeScaleControlSingularSelector
  | RuntimeScaleControlGroupSelector;

/** Persistent owner setting; exact operation fields are observation only. */
export interface RuntimeScaleControlStatus {
  selector: RuntimeScaleControlSelector;
  controlGeneration: number;
  requestedScale: number;
  supported: boolean;
  frozen: boolean;
  controlDomainId: string | null;
  controlDomainRevision: string | null;
  controlBindingId: string | null;
  controlBindingRevision: string | null;
  scalingDomainId: string | null;
  activeControlConfigurationId: string | null;
  activeControlConfigurationGeneration: number | null;
  activeOperationId: string | null;
  activeOperationRevision: number | null;
  activeOperationScaleGeneration: number | null;
  effectiveScale: number | null;
  applicationState: RuntimeScaleApplicationState | null;
  reason: string | null;
  groupMembers?: RuntimeScaleControlMember[];
}

export interface RuntimeScaleControlMember extends RuntimeScaleControlTarget {
  controlDomainId: string;
  controlDomainRevision: string;
  controlBindingId: string;
  controlBindingRevision: string;
  scalingDomainId: string;
  activeControlConfigurationId: string;
  activeControlConfigurationGeneration: number;
}

export interface RuntimeScaleControlAccepted {
  requestId: string;
  selector: RuntimeScaleControlSelector;
  requestedScale: number;
  controlGeneration: number;
  activeOperationId: string | null;
  activeOperationRevision: number | null;
  activeOperationScaleGeneration: number | null;
}

/**
 * One robot's latest owner-admitted TCP speed in a Program run: copied from
 * the Kernel's prepared-motion diagnostics in operation history. The browser
 * never derives it.
 */
export interface AdmittedTcpSpeed {
  controllableComponentId: string;
  commandTargetId: string;
  operationId: string;
  occurredAtWall: string;
  requestedTcpLinearSpeedMps: number | null;
  deploymentMaximumTcpLinearSpeedMps: number;
  admittedTcpLinearSpeedMps: number;
}

export interface ProgramRunAdmittedSpeeds {
  programRunId: string;
  targets: AdmittedTcpSpeed[];
}
