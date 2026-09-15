import type {
  CapabilityCommandPreviewReceipt,
  DirectControlMotionSpec,
  DirectControlOperationIntent,
  DirectControlPreviewReceipt,
  ExecutionOwnerStatus,
  MotionParticipantPayload,
  MotionPreviewReceipt,
  OperationObservation,
  OperationStateName,
  Pose3Value,
} from '../types/robotControl';
import { cartesianMoveOffsetsFromTarget, computeCartesianMoveDraft, DEFAULT_MOVE_TO_OPTIONS } from '../utils/moveTo';
import type { CartesianMoveDraftOptions, MoveToDraft, MoveToDraftSource } from '../utils/moveTo';
import { poseTupleFromValue, poseValueFromTuple, samePose } from '../utils/poseTransforms';

export const ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS = 150;

const TERMINAL_OPERATION_STATES = new Set<OperationStateName>([
  'completed',
  'stopped',
  'failed',
  'interrupted',
  'intervention_required',
  'cancelled_pre_dispatch',
]);

export type RobotControlMode =
  | 'joint'
  | 'pose_goal'
  | 'move_to'
  | 'cartesian_path'
  | 'gripper';

export type RobotControlPhase =
  | 'loading'
  | 'unavailable'
  | 'idle'
  | 'planning'
  | 'ready'
  | 'blocked'
  | 'approving'
  | 'executing'
  | 'stopping'
  | 'completed'
  | 'stopped'
  | 'failed'
  | 'interrupted'
  | 'intervention_required'
  | 'cancelled_pre_dispatch';

const IDENTITY_POSE: Pose3Value = {
  position_m: { x_m: 0, y_m: 0, z_m: 0 },
  orientation: { x: 0, y: 0, z: 0, w: 1 },
};

export interface ManualMotionSettings {
  velocityScale: number;
  accelerationScale: number;
  tcpLinearSpeedMps: number;
  tcpAngularSpeedRadS: number;
  tcpLinearAccelerationMps2: number;
  tcpAngularAccelerationRadS2: number;
  jointLimits: Record<string, {
    max_velocity_rad_s?: number | null;
    max_acceleration_rad_s2?: number | null;
  }>;
}

const DEFAULT_MANUAL_MOTION_SETTINGS: ManualMotionSettings = {
  velocityScale: 1,
  accelerationScale: 1,
  tcpLinearSpeedMps: 0.1,
  tcpAngularSpeedRadS: 0.5,
  tcpLinearAccelerationMps2: 0.5,
  tcpAngularAccelerationRadS2: 2.5,
  jointLimits: {},
};

export interface RobotControlState {
  status: ExecutionOwnerStatus | null;
  phase: RobotControlPhase;
  mode: RobotControlMode;
  moveToKind: 'pose' | 'joint';
  moveToDraft: MoveToDraft | null;
  poseDragging: boolean;
  draftPositionsRad: number[];
  draftPose: Pose3Value;
  draftGripperPosition: number | null;
  motionSettings: ManualMotionSettings;
  generation: number;
  intent: DirectControlOperationIntent | null;
  preview: DirectControlPreviewReceipt | null;
  ghost: MotionPreviewReceipt | null;
  ghostStale: boolean;
  operation: OperationObservation | null;
  previewIssue: string | null;
  error: string | null;
}

export type RobotControlAction =
  | { type: 'load' }
  | { type: 'status_resolved'; status: ExecutionOwnerStatus }
  | { type: 'status_refreshed'; status: ExecutionOwnerStatus }
  | { type: 'manual_scope_changed' }
  | { type: 'load_failed'; error: string }
  | { type: 'program_activity_changed' }
  | { type: 'select_mode'; mode: RobotControlMode }
  | { type: 'edit'; positionsRad: number[] }
  | { type: 'edit_joint'; jointIndex: number; positionRad: number }
  | { type: 'edit_move_to_joint'; positionsRad: number[] }
  | { type: 'select_move_source'; source: MoveToDraftSource }
  | { type: 'edit_move_options'; update: Partial<CartesianMoveDraftOptions> }
  | { type: 'begin_pose_drag' }
  | { type: 'finish_pose_drag'; generation: number; pose: Pose3Value | null }
  | { type: 'edit_pose'; pose: Pose3Value }
  | { type: 'edit_gripper'; position: number }
  | { type: 'edit_motion_settings'; update: Partial<ManualMotionSettings> }
  | {
      type: 'preview_started';
      generation: number;
      intent: DirectControlOperationIntent;
    }
  | {
      type: 'preview_resolved';
      generation: number;
      receipt: DirectControlPreviewReceipt;
    }
  | { type: 'preview_rejected'; generation: number; error: string; targetRejection?: string | null }
  | { type: 'approve_started' }
  | { type: 'approve_rejected'; generation: number; error: string }
  | { type: 'retry_preview' }
  | { type: 'operation_observed'; operation: OperationObservation }
  | { type: 'operation_control_failed'; ownerEpoch: string; operationId: string; error: string }
  | { type: 'dismiss_error' }
  | { type: 'revert' }
  | { type: 'clear' };

export const initialRobotControlState: RobotControlState = {
  status: null,
  phase: 'loading',
  mode: 'joint',
  moveToKind: 'pose',
  moveToDraft: null,
  poseDragging: false,
  draftPositionsRad: [],
  draftPose: IDENTITY_POSE,
  draftGripperPosition: null,
  motionSettings: DEFAULT_MANUAL_MOTION_SETTINGS,
  generation: 0,
  intent: null,
  preview: null,
  ghost: null,
  ghostStale: false,
  operation: null,
  previewIssue: null,
  error: null,
};

function copyPose(pose: Pose3Value): Pose3Value {
  return {
    position_m: { ...pose.position_m },
    orientation: { ...pose.orientation },
  };
}

export function operationStateIsTerminal(state: OperationStateName): boolean {
  return TERMINAL_OPERATION_STATES.has(state);
}

/** Presentation only: clean terminal results retire automatically at the owner. */
export function manualOperationRequiresRecovery(operation: OperationObservation | null): boolean {
  return operation?.resident === true && operationStateIsTerminal(operation.state)
    && (operation.terminalError !== null
      || ['failed', 'interrupted', 'intervention_required'].includes(operation.state));
}

export function robotControlHasActiveOperation(
  state: RobotControlState,
): boolean {
  return state.phase === 'approving'
    || state.phase === 'executing'
    || state.phase === 'stopping';
}

/** Candidate presentation follows the backend's manual-control admission.
 * A retained or active manual operation remains the sole owner of its result. */
export function robotControlCandidatePresentationAllowed(
  state: RobotControlState,
  manualControlAllowed: boolean,
): boolean {
  return manualControlAllowed
    && state.operation === null
    && !robotControlHasActiveOperation(state);
}

function arraysEqual<T>(left: T[], right: T[]): boolean {
  return left.length === right.length
    && left.every((value, index) => value === right[index]);
}

function posesEqual(left: Pose3Value, right: Pose3Value): boolean {
  return left.position_m.x_m === right.position_m.x_m
    && left.position_m.y_m === right.position_m.y_m
    && left.position_m.z_m === right.position_m.z_m
    && left.orientation.x === right.orientation.x
    && left.orientation.y === right.orientation.y
    && left.orientation.z === right.orientation.z
    && left.orientation.w === right.orientation.w;
}

function positionsAreValid(
  status: ExecutionOwnerStatus,
  positions: number[],
): boolean {
  const joints = status.directControl.targetDescriptor.joints;
  return positions.length === joints.length
    && positions.every((position, index) => {
      const joint = joints[index];
      return Number.isFinite(position)
        && joint !== undefined
        && position >= joint.minPositionRad
        && position <= joint.maxPositionRad;
    });
}

function motionSettingsAreValid(status: ExecutionOwnerStatus, settings: ManualMotionSettings): boolean {
  const { jointLimits, ...values } = settings;
  const descriptor = status.directControl.targetDescriptor;
  return Object.values(values).every((value) => Number.isFinite(value) && value > 0)
    && settings.velocityScale <= 1 && settings.accelerationScale <= 1
    && settings.tcpLinearSpeedMps <= (descriptor.operationalLimit?.effectiveMaximumTcpLinearSpeedMps ?? Infinity)
    && Object.entries(jointLimits).every(([name, limit]) => {
      const joint = descriptor.joints.find((entry) => entry.sourceJointName === name);
      if (joint === undefined) return false;
      const ceiling = descriptor.operationalJointLimits.find((entry) => entry.sourceJointName === name);
      return Object.values(limit).every((value) => value == null || (Number.isFinite(value) && value > 0))
        && (limit.max_velocity_rad_s == null
          || limit.max_velocity_rad_s <= (ceiling?.effectiveMaxVelocityRadS ?? joint.maxVelocityRadS))
        && (limit.max_acceleration_rad_s2 == null
          || limit.max_acceleration_rad_s2 <= (ceiling?.effectiveMaxAccelerationRadS2 ?? joint.maxAccelerationRadS2 ?? Infinity));
    });
}

function initialMotionSettings(status: ExecutionOwnerStatus): ManualMotionSettings {
  return {
    ...DEFAULT_MANUAL_MOTION_SETTINGS,
    tcpLinearSpeedMps: Math.min(DEFAULT_MANUAL_MOTION_SETTINGS.tcpLinearSpeedMps,
      status.directControl.targetDescriptor.operationalLimit?.effectiveMaximumTcpLinearSpeedMps ?? Infinity),
  };
}

function poseIsValid(pose: Pose3Value): boolean {
  const values = [
    pose.position_m.x_m,
    pose.position_m.y_m,
    pose.position_m.z_m,
    pose.orientation.x,
    pose.orientation.y,
    pose.orientation.z,
    pose.orientation.w,
  ];
  const norm = Math.hypot(
    pose.orientation.x,
    pose.orientation.y,
    pose.orientation.z,
    pose.orientation.w,
  );
  return values.every(Number.isFinite) && norm > 1e-12;
}

function gripperPositionIsValid(
  status: ExecutionOwnerStatus,
  position: number,
): boolean {
  if (!Number.isFinite(position)) return false;
  const { minPosition, maxPosition } = status.directControlGripper.targetDescriptor;
  return minPosition === null && maxPosition === null
    ? true
    : minPosition !== null
      && maxPosition !== null
      && position >= minPosition
      && position <= maxPosition;
}

function commonReceiptMatches(
  status: ExecutionOwnerStatus,
  intent: DirectControlOperationIntent,
  receipt: DirectControlPreviewReceipt,
): boolean {
  const participant = intent.participants[0];
  return receipt.commitEligible
    && receipt.requestId === intent.request_context.request_id
    && receipt.requestOwnerEpoch === status.ownerEpoch
    && receipt.programId === intent.program_id
    && receipt.programRevision === intent.program_revision
    && receipt.programBindingId === intent.program_binding_id
    && receipt.programRunId === intent.program_run_id
    && receipt.graphRevision === intent.expected_graph_revision
    && receipt.retainedBundleId === intent.expected_retained_bundle_id
    && receipt.sceneRevision.scene_id
      === intent.expected_scene_revision.scene_id
    && receipt.sceneRevision.revision_id
      === intent.expected_scene_revision.revision_id
    && receipt.sceneRevision.content_hash
      === intent.expected_scene_revision.content_hash
    && receipt.componentRoleId === participant.component_role_id
    && receipt.targetRoleId === participant.target_role_id
    && receipt.requiredCapabilityId === participant.required_capability_id;
}

function receiptMatchesCurrentDraft(
  state: RobotControlState,
  receipt: DirectControlPreviewReceipt,
): boolean {
  const { intent, status } = state;
  if (intent === null || status === null) return false;
  const payload = intent.participants[0].payload;
  if (!commonReceiptMatches(status, intent, receipt)) return false;
  if (receipt.schemaVersion === 'manyforge.direct_control_motion_preview.v1') {
    if (payload.kind !== 'motion') return false;
    const expectedNames = status.directControl.targetDescriptor.joints.map(
      (joint) => joint.sourceJointName,
    );
    return arraysEqual(receipt.sourceJointNames, expectedNames)
      && receipt.finalPositionsRad.length === expectedNames.length;
  }
  if (payload.kind !== 'capability_command') return false;
  const position = payload.command_value.position;
  const target = position.kind === 'angular_rad'
    ? position.value_rad
    : position.value_m;
  return receipt.commandSchemaId === payload.command_schema_id
    && receipt.commandSchemaRevision === payload.command_schema_revision
    && receipt.sourceCoordinateName
      === status.directControlGripper.targetDescriptor.sourceCoordinateName
    && receipt.sourceKind === status.directControlGripper.sourceKind
    && receipt.coordinateKind === position.kind
    && receipt.targetPosition === target
    && receipt.minPosition === status.directControlGripper.targetDescriptor.minPosition
    && receipt.maxPosition === status.directControlGripper.targetDescriptor.maxPosition;
}

function phaseForOperation(operation: OperationObservation): RobotControlPhase {
  if (!operationStateIsTerminal(operation.state)) {
    return operation.state === 'stopping' ? 'stopping' : 'executing';
  }
  return operation.state as RobotControlPhase;
}

function draftFromStatus(status: ExecutionOwnerStatus) {
  return {
    moveToDraft: null,
    poseDragging: false,
    draftPositionsRad: [...status.observedMotion.positionsRad],
    draftPose: copyPose(status.observedMotion.currentTcpPose),
    draftGripperPosition: status.observedGripper?.position ?? null,
  };
}

export function robotControlHasLocalDraft(state: RobotControlState): boolean {
  return state.operation === null
    && (state.poseDragging || state.phase === 'planning' || state.phase === 'ready'
      || state.phase === 'approving' || state.phase === 'blocked');
}

function resolveStatus(
  state: RobotControlState,
  status: ExecutionOwnerStatus,
  initialize: boolean,
): RobotControlState {
  const selectedRun = state.mode === 'gripper'
    ? status.directControlGripper.programRunId : status.directControl.programRunId;
  const projected = status.manualOperations.filter((operation) => operation.programRunId === selectedRun);
  const projectedOperation = projected.length === 1 ? projected[0] : null;
  const projectionError = projected.length > 1
    ? 'More than one direct-control operation is resident; Robot Control will not guess which operation to own.'
    : null;
  const resolvedPhase = projectionError !== null
    ? 'blocked'
    : projectedOperation === null
      ? 'idle'
      : phaseForOperation(projectedOperation);
  const resolvedOperation = projectionError === null ? projectedOperation : null;
  if (state.status !== null && (
    state.status.ownerEpoch !== status.ownerEpoch
    || state.status.retainedBundleId !== status.retainedBundleId
    || state.status.graphRevision !== status.graphRevision
    || state.status.directControl.programBindingId !== status.directControl.programBindingId
    || state.status.directControlGripper.programBindingId !== status.directControlGripper.programBindingId
  )) {
    return {
      ...initialRobotControlState,
      mode: state.mode,
      motionSettings: initialMotionSettings(status),
      status,
      phase: resolvedPhase,
      ...draftFromStatus(status),
      generation: state.generation + 1,
      operation: resolvedOperation,
      error: projectionError
        ?? (state.status.ownerEpoch !== status.ownerEpoch
          ? 'The execution owner restarted. The prior preview was discarded.'
          : 'The target binding changed. The prior preview was discarded.'),
    };
  }
  if (initialize || state.status === null) {
    return {
      ...state,
      status,
      phase: resolvedPhase,
      ...draftFromStatus(status),
      motionSettings: state.status === null ? initialMotionSettings(status) : state.motionSettings,
      intent: null,
      preview: null,
      ghost: null,
      ghostStale: false,
      operation: resolvedOperation,
      previewIssue: null,
      error: projectionError ?? resolvedOperation?.terminalError ?? null,
    };
  }
  if (projectionError !== null) {
    return {
      ...state,
      status,
      phase: 'blocked',
      operation: null,
      previewIssue: null,
      error: projectionError,
    };
  }
  // The latest settled result stays visible through the same owner snapshot,
  // but repeated publication must not overwrite a new draft or an approval
  // still waiting for a *new* operation to become visible.
  const alreadyPresentedTerminal = projectedOperation !== null && !projectedOperation.resident
    && state.status.manualOperations.some((previous) => !previous.resident
      && previous.operationId === projectedOperation.operationId);
  if ((projectedOperation === null || alreadyPresentedTerminal)
    && (state.phase === 'ready' || state.phase === 'planning'
      || (state.phase === 'blocked' && state.intent === null))
    && (state.status.sceneRevision.scene_id !== status.sceneRevision.scene_id
      || state.status.sceneRevision.revision_id !== status.sceneRevision.revision_id
      || state.status.sceneRevision.content_hash !== status.sceneRevision.content_hash)) {
    // Same owner/binding and authored target, new scene evidence. Reuse the
    // normal trailing preview debounce; never retry approval or an operation.
    return invalidateDraft(state, { status });
  }
  if (alreadyPresentedTerminal) {
    return { ...state, status };
  }
  if (projectedOperation !== null) {
    return {
      ...state,
      status,
      phase: phaseForOperation(projectedOperation),
      poseDragging: false,
      moveToDraft: null,
      operation: projectedOperation,
      previewIssue: null,
      intent: null,
      preview: null,
      ghost: null,
      ghostStale: false,
      error: projectedOperation.terminalError ?? state.error,
      ...(operationStateIsTerminal(projectedOperation.state)
        ? draftFromStatus(status)
        : {}),
    };
  }
  if (state.operation !== null) {
    return {
      ...state,
      status,
      phase: 'idle',
      ...draftFromStatus(status),
      operation: null,
      intent: null,
      preview: null,
      ghost: null,
      ghostStale: false,
      error: state.error,
    };
  }
  return {
    ...state,
    status,
  };
}

function invalidateDraft(
  state: RobotControlState,
  update: Partial<RobotControlState>,
): RobotControlState {
  return {
    ...state,
    ...update,
    poseDragging: false,
    phase: 'planning',
    generation: state.generation + 1,
    intent: null,
    preview: null,
    ghostStale: state.ghost !== null,
    operation: null,
    previewIssue: null,
    error: null,
  };
}

export function robotControlReducer(
  state: RobotControlState,
  action: RobotControlAction,
): RobotControlState {
  if (state.operation?.resident && ['edit', 'edit_joint', 'edit_move_to_joint', 'edit_pose', 'edit_gripper', 'edit_motion_settings', 'select_move_source', 'edit_move_options', 'begin_pose_drag', 'finish_pose_drag']
    .includes(action.type)) return state;
  switch (action.type) {
    case 'load':
      return state.status === null
        ? { ...state, phase: 'loading', error: null }
        : { ...state, error: null };
    case 'status_resolved':
      return resolveStatus(state, action.status, true);
    case 'status_refreshed':
      return resolveStatus(state, action.status, false);
    case 'manual_scope_changed':
      return robotControlHasLocalDraft(state) ? invalidateDraft(state, {}) : state;
    case 'load_failed':
      return state.status === null
        ? { ...state, phase: 'unavailable', error: action.error }
        : { ...state, error: action.error };
    case 'program_activity_changed':
      if (state.status === null || robotControlHasActiveOperation(state) || state.operation?.resident) {
        return state;
      }
      // A Program lifecycle owns robot motion while it is non-idle. Discard
      // local no-motion work at either boundary and rebase on the owner's
      // latest observation; never resume a stale pre-Program preview.
      return {
        ...state,
        phase: 'idle',
        ...draftFromStatus(state.status),
        generation: state.generation + 1,
        intent: null,
        preview: null,
        ghost: null,
        ghostStale: false,
        operation: null,
        previewIssue: null,
        error: null,
      };
    case 'select_mode':
      if (
        state.status === null
        || robotControlHasActiveOperation(state)
        || state.mode === action.mode
      ) return state;
      // A new editor must not relabel the last settled operation as its own
      // preview. resolveStatus still retains any resident operation/fault.
      return resolveStatus({
        ...state,
        mode: action.mode,
        moveToKind: 'pose',
        phase: 'idle',
        ...draftFromStatus(state.status),
        generation: state.generation + 1,
        intent: null,
        preview: null,
        ghost: null,
        ghostStale: false,
        operation: null,
        previewIssue: null,
        error: null,
      }, state.status, false);
    case 'edit':
    case 'edit_joint': {
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      const baseline = robotControlHasLocalDraft(state)
        ? state.draftPositionsRad : state.status.observedMotion.positionsRad;
      const positionsRad = action.type === 'edit' ? action.positionsRad : [...baseline];
      if (action.type === 'edit_joint') {
        if (!Number.isInteger(action.jointIndex) || action.jointIndex < 0
          || action.jointIndex >= positionsRad.length) return state;
        positionsRad[action.jointIndex] = action.positionRad;
      }
      if (!positionsAreValid(state.status, positionsRad)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The authored joint target is outside its retained limits.',
        };
      }
      if (
        state.mode === 'joint'
        && arraysEqual(baseline, positionsRad)
      ) return state;
      return invalidateDraft(state, {
        mode: 'joint',
        draftPositionsRad: [...positionsRad],
      });
    }
    case 'edit_motion_settings': {
      if (state.status === null || state.mode === 'gripper' || robotControlHasActiveOperation(state)) return state;
      const motionSettings = {
        ...state.motionSettings,
        ...action.update,
        jointLimits: { ...state.motionSettings.jointLimits },
      };
      for (const [joint, limit] of Object.entries(action.update.jointLimits ?? {})) {
        motionSettings.jointLimits[joint] = { ...motionSettings.jointLimits[joint], ...limit };
      }
      if (!motionSettingsAreValid(state.status, motionSettings)) return state;
      // Editing limits alone does not invent a target or start a no-op preview.
      return robotControlHasLocalDraft(state)
        ? invalidateDraft(state, { motionSettings })
        : { ...state, motionSettings };
    }
    case 'select_move_source': {
      if (state.status === null || robotControlHasActiveOperation(state)) return state;
      const moveToDraft: MoveToDraft = {
        source: structuredClone(action.source),
        currentTcpPose: poseTupleFromValue(state.status.observedMotion.currentTcpPose),
        options: structuredClone(state.moveToDraft?.options ?? DEFAULT_MOVE_TO_OPTIONS),
      };
      if (action.source.kind === 'joint') {
        return robotControlReducer({ ...state, moveToDraft }, { type: 'edit_move_to_joint', positionsRad: action.source.positionsRad });
      }
      const pose = computeCartesianMoveDraft(action.source.pose, moveToDraft.currentTcpPose, moveToDraft.options);
      if (pose === null) return state;
      return invalidateDraft(state, { mode: 'move_to', moveToKind: 'pose', moveToDraft, draftPose: poseValueFromTuple(pose) });
    }
    case 'edit_move_options': {
      const draft = state.moveToDraft;
      if (state.status === null || state.mode !== 'move_to' || draft?.source.kind !== 'pose'
        || robotControlHasActiveOperation(state)) return state;
      const options = { ...draft.options, ...action.update };
      const pose = computeCartesianMoveDraft(draft.source.pose, draft.currentTcpPose, options);
      if (pose === null) return state;
      return invalidateDraft(state, { moveToDraft: { ...draft, options }, draftPose: poseValueFromTuple(pose) });
    }
    case 'begin_pose_drag':
      if (state.status === null || state.poseDragging || robotControlHasActiveOperation(state)
        || !['pose_goal', 'cartesian_path', 'move_to'].includes(state.mode)
        || (state.mode === 'move_to' && state.moveToDraft?.source.kind !== 'pose')) return state;
      return {
        ...state,
        draftPose: robotControlHasLocalDraft(state) ? state.draftPose : copyPose(state.status.observedMotion.currentTcpPose),
        poseDragging: true,
        generation: state.generation + 1,
      };
    case 'finish_pose_drag': {
      if (!state.poseDragging || action.generation !== state.generation) return state;
      const settled = { ...state, poseDragging: false };
      if (action.pose === null || samePose(poseTupleFromValue(state.draftPose), poseTupleFromValue(action.pose))) return settled;
      return robotControlReducer(settled, { type: 'edit_pose', pose: action.pose });
    }
    case 'edit_move_to_joint': {
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      if (!positionsAreValid(state.status, action.positionsRad)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The stored joint target is outside its retained limits.',
        };
      }
      if (
        state.mode === 'move_to'
        && state.moveToKind === 'joint'
        && arraysEqual(state.draftPositionsRad, action.positionsRad)
      ) return state;
      return invalidateDraft(state, {
        mode: 'move_to',
        moveToKind: 'joint',
        draftPositionsRad: [...action.positionsRad],
      });
    }
    case 'edit_pose': {
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      if (!poseIsValid(action.pose)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The authored TCP pose is invalid.',
        };
      }
      if (posesEqual(robotControlHasLocalDraft(state)
        ? state.draftPose : state.status.observedMotion.currentTcpPose, action.pose)
        && (state.mode !== 'move_to' || state.moveToKind === 'pose')) return state;
      const draft = state.mode === 'move_to' ? state.moveToDraft : null;
      const options = draft?.source.kind === 'pose'
        ? cartesianMoveOffsetsFromTarget(draft.source.pose, draft.currentTcpPose, draft.options, poseTupleFromValue(action.pose))
        : null;
      return invalidateDraft(state, {
        ...(state.mode === 'move_to' ? { moveToKind: 'pose' as const } : {}),
        ...(draft !== null && options !== null ? { moveToDraft: { ...draft, options } } : {}),
        draftPose: copyPose(action.pose),
      });
    }
    case 'edit_gripper':
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      if (!gripperPositionIsValid(state.status, action.position)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The authored gripper target is outside its retained limits.',
        };
      }
      if (
        state.mode === 'gripper'
        && (robotControlHasLocalDraft(state)
          ? state.draftGripperPosition : state.status.observedGripper?.position) === action.position
      ) return state;
      return invalidateDraft(state, {
        mode: 'gripper',
        draftGripperPosition: action.position,
      });
    case 'preview_started':
      if (
        action.generation !== state.generation
        || state.poseDragging
        || state.phase !== 'planning'
      ) return state;
      return { ...state, intent: action.intent };
    case 'preview_resolved':
      if (
        action.generation !== state.generation
        || state.poseDragging
        || state.phase !== 'planning'
      ) return state;
      if (!receiptMatchesCurrentDraft(state, action.receipt)) {
        return {
          ...state,
          phase: 'blocked',
          intent: null,
          preview: null,
          ghostStale: state.ghost !== null,
          error: 'The preview evidence does not match the current draft.',
        };
      }
      return {
        ...state,
        phase: 'ready',
        preview: action.receipt,
        ghost: action.receipt.schemaVersion
          === 'manyforge.direct_control_motion_preview.v1'
          ? action.receipt
          : state.ghost,
        ghostStale: action.receipt.schemaVersion
          === 'manyforge.direct_control_motion_preview.v1'
          ? false
          : state.ghost !== null,
        previewIssue: null,
        error: null,
      };
    case 'preview_rejected':
      if (action.generation !== state.generation || state.poseDragging) return state;
      return {
        ...state,
        phase: 'blocked',
        intent: null,
        preview: null,
        ghostStale: state.ghost !== null,
        previewIssue: action.targetRejection ?? null,
        error: action.targetRejection ? null : action.error,
      };
    case 'approve_started':
      if (
        state.phase !== 'ready'
        || state.poseDragging
        || state.preview === null
        || state.intent === null
      ) return state;
      return { ...state, phase: 'approving', error: null };
    case 'approve_rejected':
      if (state.phase !== 'approving' || action.generation !== state.generation) return state;
      return {
        ...state,
        phase: 'blocked',
        preview: null,
        ghostStale: state.ghost !== null,
        error: action.error,
      };
    case 'retry_preview':
      if (state.status === null || state.operation?.resident || state.phase !== 'blocked') return state;
      return invalidateDraft(state, {});
    case 'operation_observed':
      if (
        state.operation === null
        || state.operation.operationId !== action.operation.operationId
        || action.operation.operationRevision
          < state.operation.operationRevision
      ) return state;
      return {
        ...state,
        phase: phaseForOperation(action.operation),
        operation: action.operation,
        ghost: null,
        ghostStale: false,
        error: action.operation.terminalError ?? state.error,
      };
    case 'operation_control_failed':
      return state.status?.ownerEpoch === action.ownerEpoch
        && state.operation?.operationId === action.operationId
        ? { ...state, error: action.error }
        : state;
    case 'dismiss_error':
      return state.error === null ? state : { ...state, error: null };
    case 'revert':
      if (robotControlHasActiveOperation(state)) return state;
      return {
        ...state,
        phase: state.operation?.resident ? phaseForOperation(state.operation)
          : state.status === null ? 'unavailable' : 'idle',
        ...(state.status === null
          ? {
              draftPositionsRad: [],
              draftPose: IDENTITY_POSE,
              draftGripperPosition: null,
            }
          : draftFromStatus(state.status)),
        generation: state.generation + 1,
        intent: null,
        preview: null,
        ghost: null,
        ghostStale: false,
        operation: state.operation?.resident ? state.operation : null,
        previewIssue: null,
        error: state.operation?.resident ? state.operation.terminalError : null,
      };
    case 'clear':
      return {
        ...initialRobotControlState,
        phase: 'unavailable',
        generation: state.generation + 1,
      };
  }
}

function requireMotionStatus(
  status: ExecutionOwnerStatus,
  capability: string,
): void {
  if (!status.observedMotion.complete) {
    throw new Error('Authoritative joint state is incomplete.');
  }
  const descriptor = status.directControl.targetDescriptor;
  const frameRoles = descriptor.frames.map((frame) => frame.frameRoleId);
  if (!frameRoles.includes(status.directControl.baseFrameRoleId)
    || !frameRoles.includes(status.directControl.tcpFrameRoleId)) {
    throw new Error('The direct-control base/TCP frame contract is incomplete.');
  }
  if (!descriptor.contracts.supportingProviders.some(
    (provider) => provider.capabilityId === capability,
  )) {
    throw new Error(`${capability} is unavailable for this target.`);
  }
}

function buildMotionIntent(
  status: ExecutionOwnerStatus,
  motion: DirectControlMotionSpec,
  requestId: string,
  settings: ManualMotionSettings,
): DirectControlOperationIntent {
  const capability = `planning.motion.${motion.kind}`;
  requireMotionStatus(status, capability);
  if (requestId.trim() === '') {
    throw new Error('Robot Control request identity must not be empty.');
  }
  if (!motionSettingsAreValid(status, settings)) throw new Error('Manual motion limits are invalid.');
  // These are optional tighter request caps. Retained Deployment ceilings are
  // enforced by the owner, not copied into each request by the browser.
  const jointLimits = Object.fromEntries(Object.entries(settings.jointLimits).flatMap(([name, limit]) => {
    const values = Object.entries(limit).filter(([, value]) => value != null);
    return values.length === 0 ? [] : [[name, Object.fromEntries(values)]];
  }));
  const targetTcpLimit = status.directControl.targetDescriptor.operationalLimit
    ?.effectiveMaximumTcpLinearSpeedMps ?? null;
  const payload: MotionParticipantPayload = {
    kind: 'motion',
    move_id: `robot-control-${requestId}`,
    base_frame_role_id: status.directControl.baseFrameRoleId,
    tcp_frame_role_id: status.directControl.tcpFrameRoleId,
    tolerances: {
      joint_position_tol_rad: 0.05,
      translation_tol_m: 0.005,
      rotation_tol_rad: 0.05,
    },
    speed_profile: {
      velocity_scale: settings.velocityScale,
      acceleration_scale: settings.accelerationScale,
      max_tcp_linear_speed_mps: Math.min(settings.tcpLinearSpeedMps, targetTcpLimit ?? Infinity),
    },
    timeouts: {
      planning_timeout_s: 2,
      execution_timeout_s: 10,
    },
    cache_mode: 'default',
    planning_profile: 'balanced',
    target_mode: 'absolute',
    target_reference: null,
    offset_reference: null,
    motion,
    contact_policy_id: null,
    per_joint_dynamics_limits: jointLimits,
    motion_interrupt_scopes: [],
    runtime_object_bindings: [],
  };
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: status.ownerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
    },
    program_id: status.directControl.programId,
    program_revision: status.directControl.programRevision,
    program_binding_id: status.directControl.programBindingId,
    program_run_id: status.directControl.programRunId,
    expected_graph_revision: status.graphRevision,
    expected_retained_bundle_id: status.retainedBundleId,
    expected_scene_revision: { ...status.sceneRevision },
    participants: [{
      component_role_id: status.directControl.targetDescriptor.componentRoleId,
      target_role_id: status.directControl.targetDescriptor.targetRoleId,
      required_capability_id: status.directControl.targetDescriptor.contracts.operationCapabilityId,
      action_span: null,
      payload,
    }],
    coordination_policy: 'independent',
  };
}

export function buildJointMotionIntent(
  status: ExecutionOwnerStatus,
  positionsRad: number[],
  requestId: string,
  settings: ManualMotionSettings = initialMotionSettings(status),
): DirectControlOperationIntent {
  if (!positionsAreValid(status, positionsRad)) {
    throw new Error('The authored joint target is outside its retained limits.');
  }
  return buildMotionIntent(status, {
    kind: 'joint_goal',
    joint_names: status.directControl.targetDescriptor.joints.map(
      (joint) => joint.sourceJointName,
    ),
    positions_rad: [...positionsRad],
  }, requestId, settings);
}

function normalizedPose(pose: Pose3Value): Pose3Value {
  if (!poseIsValid(pose)) throw new Error('The authored TCP pose is invalid.');
  const norm = Math.hypot(
    pose.orientation.x,
    pose.orientation.y,
    pose.orientation.z,
    pose.orientation.w,
  );
  return {
    position_m: { ...pose.position_m },
    orientation: {
      x: pose.orientation.x / norm,
      y: pose.orientation.y / norm,
      z: pose.orientation.z / norm,
      w: pose.orientation.w / norm,
    },
  };
}

export function buildPoseMotionIntent(
  status: ExecutionOwnerStatus,
  pose: Pose3Value,
  requestId: string,
  settings: ManualMotionSettings = initialMotionSettings(status),
): DirectControlOperationIntent {
  return buildMotionIntent(status, {
    kind: 'pose_goal',
    goal: normalizedPose(pose),
  }, requestId, settings);
}

export function buildCartesianMotionIntent(
  status: ExecutionOwnerStatus,
  pose: Pose3Value,
  requestId: string,
  settings: ManualMotionSettings = initialMotionSettings(status),
): DirectControlOperationIntent {
  return buildMotionIntent(status, {
    kind: 'cartesian_path',
    waypoints: [
      copyPose(status.observedMotion.currentTcpPose),
      normalizedPose(pose),
    ],
    max_translation_step_m: 0.005,
    max_rotation_step_rad: 0.05,
    speed_mode: 'bounded',
    max_linear_speed_mps: settings.tcpLinearSpeedMps,
    max_angular_speed_rad_s: settings.tcpAngularSpeedRadS,
    max_linear_acceleration_mps2: settings.tcpLinearAccelerationMps2,
    max_angular_acceleration_rad_s2: settings.tcpAngularAccelerationRadS2,
    fallback_policy: 'fail_closed',
  }, requestId, settings);
}

export function buildGripperCommandIntent(
  status: ExecutionOwnerStatus,
  position: number,
  requestId: string,
): DirectControlOperationIntent {
  const gripper = status.directControlGripper;
  const descriptor = gripper.targetDescriptor;
  if (
    status.observedGripper === null
    && gripper.sourceKind !== 'component_local'
  ) {
    throw new Error('Authoritative gripper state is unavailable.');
  }
  if (status.observedGripper !== null && !status.observedGripper.complete) {
    throw new Error('Authoritative gripper state is incomplete.');
  }
  if (!gripperPositionIsValid(status, position)) {
    throw new Error('The authored gripper target is outside its retained limits.');
  }
  if (requestId.trim() === '') {
    throw new Error('Robot Control request identity must not be empty.');
  }
  const commandSchema = descriptor.contracts.commandSchema;
  if (commandSchema === null) {
    throw new Error('The addressed gripper command schema is unavailable.');
  }
  if (descriptor.commandDurationS === null) {
    throw new Error('The addressed gripper command duration is not configured.');
  }
  const coordinate = descriptor.coordinateKind === 'angular_rad'
    ? { kind: 'angular_rad' as const, value_rad: position }
    : { kind: 'linear_m' as const, value_m: position };
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: status.ownerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
    },
    program_id: gripper.programId,
    program_revision: gripper.programRevision,
    program_binding_id: gripper.programBindingId,
    program_run_id: gripper.programRunId,
    expected_graph_revision: status.graphRevision,
    expected_retained_bundle_id: status.retainedBundleId,
    expected_scene_revision: { ...status.sceneRevision },
    participants: [{
      component_role_id: descriptor.componentRoleId,
      target_role_id: descriptor.targetRoleId,
      required_capability_id: descriptor.contracts.operationCapabilityId,
      action_span: null,
      payload: {
        kind: 'capability_command',
        command_schema_id: commandSchema.commandSchemaId,
        command_schema_revision: commandSchema.commandSchemaRevision,
        command_value: {
          kind: 'gripper_position',
          position: coordinate,
          max_effort: null,
        },
        completion_timeout_s: 2,
        caller_options: [{
          option_id: 'command_duration_s',
          value_s: descriptor.commandDurationS,
        }],
      },
    }],
    coordination_policy: 'independent',
  };
}

export function buildPreviewIntent(
  state: Pick<
    RobotControlState,
    | 'status'
    | 'mode'
    | 'moveToKind'
    | 'draftPositionsRad'
    | 'draftPose'
    | 'draftGripperPosition'
    | 'motionSettings'
  >,
  requestId: string,
): DirectControlOperationIntent {
  if (state.status === null) {
    throw new Error('The execution owner contract is unavailable.');
  }
  switch (state.mode) {
    case 'joint':
      return buildJointMotionIntent(
        state.status,
        state.draftPositionsRad,
        requestId,
        state.motionSettings,
      );
    case 'pose_goal':
      return buildPoseMotionIntent(state.status, state.draftPose, requestId, state.motionSettings);
    case 'move_to':
      return state.moveToKind === 'joint'
        ? buildJointMotionIntent(
            state.status,
            state.draftPositionsRad,
            requestId,
            state.motionSettings,
          )
        : buildPoseMotionIntent(state.status, state.draftPose, requestId, state.motionSettings);
    case 'cartesian_path':
      return buildCartesianMotionIntent(
        state.status,
        state.draftPose,
        requestId,
        state.motionSettings,
      );
    case 'gripper':
      if (state.draftGripperPosition === null) {
        throw new Error('Enter an initial component-local gripper target.');
      }
      return buildGripperCommandIntent(
        state.status,
        state.draftGripperPosition,
        requestId,
      );
  }
}

export function quaternionFromRpyDegrees(
  rollDeg: number,
  pitchDeg: number,
  yawDeg: number,
): Pose3Value['orientation'] {
  const halfRoll = rollDeg * Math.PI / 360;
  const halfPitch = pitchDeg * Math.PI / 360;
  const halfYaw = yawDeg * Math.PI / 360;
  const cr = Math.cos(halfRoll);
  const sr = Math.sin(halfRoll);
  const cp = Math.cos(halfPitch);
  const sp = Math.sin(halfPitch);
  const cy = Math.cos(halfYaw);
  const sy = Math.sin(halfYaw);
  return {
    x: sr * cp * cy - cr * sp * sy,
    y: cr * sp * cy + sr * cp * sy,
    z: cr * cp * sy - sr * sp * cy,
    w: cr * cp * cy + sr * sp * sy,
  };
}

export function rpyDegreesFromQuaternion(
  orientation: Pose3Value['orientation'],
): [number, number, number] {
  const { x, y, z, w } = normalizedPose({
    position_m: { x_m: 0, y_m: 0, z_m: 0 },
    orientation,
  }).orientation;
  const roll = Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y));
  const pitchInput = Math.max(-1, Math.min(1, 2 * (w * y - z * x)));
  const pitch = Math.asin(pitchInput);
  const yaw = Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
  return [roll, pitch, yaw].map((value) => value * 180 / Math.PI) as [
    number,
    number,
    number,
  ];
}

export function isCapabilityCommandReceipt(
  receipt: DirectControlPreviewReceipt,
): receipt is CapabilityCommandPreviewReceipt {
  return receipt.schemaVersion
    === 'manyforge.direct_control_capability_command_preview.v1';
}
