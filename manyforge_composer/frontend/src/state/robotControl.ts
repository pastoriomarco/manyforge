import type {
  CapabilityCommandPreviewReceipt,
  DirectControlGripperStatus,
  DirectControlMotionStatus,
  DirectControlMotionSpec,
  DirectControlOperationIntent,
  DirectControlPreviewReceipt,
  DirectControlTcp,
  ExecutionOwnerStatus,
  MotionParticipantPayload,
  MotionPreviewReceipt,
  MotionPreviewPath,
  OperationObservation,
  OperationStateName,
  ObservedGripperTarget,
  ObservedMotionTarget,
  Pose3Value,
} from '../types/robotControl';
import { tcpEntryIdentityKey } from '../types/robotControl';
import { cartesianMoveOffsetsFromTarget, computeCartesianMoveDraft, DEFAULT_MOVE_TO_OPTIONS } from '../utils/moveTo';
import type { CartesianMoveDraftOptions, MoveToDraft, MoveToDraftSource } from '../utils/moveTo';
import { poseTupleFromValue, poseValueFromTuple, samePose } from '../utils/poseTransforms';
import { manualPneumaticEndpoint } from './pneumaticManual';
import type { StructuredError } from '../types/api';

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

export const ROBOT_CONTROL_MODE_LABELS: Record<RobotControlMode, string> = {
  joint: 'Joint',
  pose_goal: 'Pose plan',
  cartesian_path: 'Cartesian',
  move_to: 'Move To',
  gripper: 'Gripper',
};

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
  cartesianTranslationToleranceM: number;
  cartesianRotationToleranceRad: number;
  /** Collision clearance (m) for manual moves; null keeps the Deployment's. */
  clearanceM: number | null;
  jointLimits: Record<string, {
    max_velocity_rad_s?: number | null;
    max_acceleration_rad_s2?: number | null;
  }>;
}

export interface ManualCartesianDefaults {
  translationToleranceM: number;
}

/**
 * The Program's Cartesian defaults for manual moves. Program values reach
 * motion through Apply, so the Applied Program supplies them. Without an
 * Applied Program the manual defaults stand in for it.
 */
export function appliedCartesianDefaults(
  applied: ManualCartesianDefaults | null,
): ManualCartesianDefaults {
  return applied ?? {
    translationToleranceM: DEFAULT_MANUAL_MOTION_SETTINGS.cartesianTranslationToleranceM,
  };
}

/**
 * The tolerance is a default, not a limit: the operator may set any
 * valid value for a manual move. When the Program default itself changes
 * (Apply), the manual value takes it.
 */
function followCartesianDefault(current: number, previous: number | null, next: number): number {
  return previous === next ? current : next;
}

const DEFAULT_MANUAL_MOTION_SETTINGS: ManualMotionSettings = {
  velocityScale: 1,
  accelerationScale: 1,
  tcpLinearSpeedMps: 0.1,
  tcpAngularSpeedRadS: 0.5,
  tcpLinearAccelerationMps2: 0.5,
  tcpAngularAccelerationRadS2: 2.5,
  cartesianTranslationToleranceM: 0.005,
  cartesianRotationToleranceRad: 0.05,
  clearanceM: null,
  jointLimits: {},
};

export interface RobotControlState {
  status: ExecutionOwnerStatus | null;
  /** Stable addressed identities selected by this browser only. */
  selectedControllableComponentId: string | null;
  selectedMotionTargetId: string | null;
  selectedGripperTargetId: string | null;
  mountedToolByRobot: Readonly<Record<string, string>>;
  phase: RobotControlPhase;
  mode: RobotControlMode;
  moveToKind: 'pose' | 'joint';
  moveToPath: 'free' | 'linear';
  moveToDraft: MoveToDraft | null;
  poseDragging: boolean;
  draftPositionsRad: number[];
  draftPose: Pose3Value;
  draftGripperPosition: number | null;
  motionSettings: ManualMotionSettings;
  /** The Program Cartesian defaults the manual settings last followed. */
  cartesianDefaults: ManualCartesianDefaults | null;
  generation: number;
  intent: DirectControlOperationIntent | null;
  /** The mode from a locally accepted operation, tied to its owner identity. */
  acceptedOperationMode: {
    ownerEpoch: string;
    operationId: string;
    mode: RobotControlMode;
    controllableComponentId: string;
    commandTargetId: string;
  } | null;
  preview: DirectControlPreviewReceipt | null;
  ghost: MotionPreviewReceipt | null;
  ghostStale: boolean;
  diagnosticPreview: MotionPreviewPath | null;
  previewSampleIndex: number;
  operation: OperationObservation | null;
  previewIssue: string | null;
  error: string | null;
  errorRefusal: StructuredError | null;
  /** `error` is the panel's own target validation (no request was sent); it
   * is shown in the panel, never mirrored to the warning row. */
  errorLocal: boolean;
  /** Informational-only surface: uncommitted manual work discarded by an
   * owner/binding replacement, or a gizmo release that changed nothing or was
   * discarded. Never an operational error. */
  ownerNotice: string | null;
}

/** Why a gizmo release produced no new target (shown as an info notice). */
export type PoseDragDiscard =
  /** Released where it started (no snap involved). */
  | 'no_change'
  /** Snap rounded the release back onto the start pose. */
  | 'snapped_no_change'
  /** The robot or its control context changed between press and release. */
  | 'context_changed';

export function poseDragNotice(reason: PoseDragDiscard | 'stale'): string {
  switch (reason) {
    case 'no_change':
      return 'No change: the target was released where it started.';
    case 'snapped_no_change':
      return 'No change: Snap moved the target back to its start. Drag further than half a snap step, or turn Snap off.';
    case 'context_changed':
      return 'The robot moved or its control changed during the drag; the target was discarded.';
    case 'stale':
      return 'The robot control target changed during the drag; the release was discarded.';
  }
}

/** A visible unmounted tool is refused by the owner when selected. */
export function directControlTcpDisabledReason(tcp: DirectControlTcp): string | null {
  if (!tcp.mountedHere) return 'Tool not mounted on this robot.';
  return null;
}

export function visibleRobotControlTcps(motion: DirectControlMotionStatus, showOthers: boolean): DirectControlTcp[] {
  const activeKey = motion.activeTcp?.entry == null ? null : tcpEntryIdentityKey(motion.activeTcp.entry);
  return motion.targetDescriptor.tcps.filter((tcp) => !tcp.mountedElsewhere || showOthers
    || tcpEntryIdentityKey(tcp.entry) === activeKey);
}

export type RobotControlAction =
  | { type: 'preview_hidden' }
  | { type: 'load' }
  | { type: 'status_resolved'; status: ExecutionOwnerStatus; cartesianDefaults?: ManualCartesianDefaults | null }
  | { type: 'status_refreshed'; status: ExecutionOwnerStatus; cartesianDefaults?: ManualCartesianDefaults | null }
  | { type: 'program_cartesian_defaults_changed'; defaults: ManualCartesianDefaults }
  | { type: 'select_robot'; controllableComponentId: string; motionTargetId: string }
  | { type: 'select_tool'; gripperTargetId: string }
  | { type: 'active_tcp_change_started' }
  | { type: 'mounted_tools_changed'; mounts: Readonly<Record<string, string>>; graphRevision?: string; retainedBundleId?: string }
  | { type: 'manual_scope_changed' }
  | { type: 'load_failed'; error: string }
  | { type: 'program_activity_changed' }
  | { type: 'select_mode'; mode: RobotControlMode }
  | { type: 'edit'; positionsRad: number[] }
  | { type: 'edit_joint'; jointIndex: number; positionRad: number }
  | { type: 'edit_move_to_joint'; positionsRad: number[] }
  | { type: 'select_move_source'; source: MoveToDraftSource }
  | { type: 'edit_move_options'; update: Partial<CartesianMoveDraftOptions> }
  | { type: 'set_move_to_path'; path: 'free' | 'linear' }
  | { type: 'begin_pose_drag' }
  | { type: 'finish_pose_drag'; generation: number; pose: Pose3Value | null; discard?: PoseDragDiscard }
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
  | { type: 'preview_rejected'; generation: number; error: string; errorRefusal?: StructuredError | null; targetRejection?: string | null }
  | {
      type: 'operation_mode_accepted';
      generation: number;
      ownerEpoch: string;
      requestId: string;
      operationId: string;
      mode: RobotControlMode;
    }
  | { type: 'preview_diagnostic'; generation: number; diagnostic: MotionPreviewPath }
  | { type: 'select_preview_sample'; index: number }
  | { type: 'approve_started' }
  | { type: 'approve_rejected'; generation: number; error: string; errorRefusal?: StructuredError | null }
  | { type: 'retry_preview' }
  | { type: 'operation_observed'; operation: OperationObservation }
  | { type: 'operation_control_failed'; ownerEpoch: string; operationId: string; error: string; errorRefusal?: StructuredError | null }
  | { type: 'dismiss_error' }
  | { type: 'revert' }
  | { type: 'clear' };

export const initialRobotControlState: RobotControlState = {
  status: null,
  selectedControllableComponentId: null,
  selectedMotionTargetId: null,
  selectedGripperTargetId: null,
  mountedToolByRobot: {},
  phase: 'loading',
  mode: 'joint',
  moveToKind: 'pose',
  moveToPath: 'free',
  moveToDraft: null,
  poseDragging: false,
  draftPositionsRad: [],
  draftPose: IDENTITY_POSE,
  draftGripperPosition: null,
  motionSettings: DEFAULT_MANUAL_MOTION_SETTINGS,
  cartesianDefaults: null,
  generation: 0,
  intent: null,
  acceptedOperationMode: null,
  preview: null,
  diagnosticPreview: null,
  previewSampleIndex: 0,
  ghost: null,
  ghostStale: false,
  operation: null,
  previewIssue: null,
  error: null,
  errorRefusal: null,
  errorLocal: false,
  ownerNotice: null,
};

/**
 * Manual control targets are an owner projection, not a second browser-side
 * model: every addressed target is owned by one listed component.
 */
export function motionTargetsForStatus(
  status: ExecutionOwnerStatus,
): DirectControlMotionStatus[] {
  return status.directControlTargets;
}

export function gripperTargetsForStatus(
  status: ExecutionOwnerStatus,
): DirectControlGripperStatus[] {
  return status.directControlGripperTargets;
}

/** The selected motion target and its observation, for motion-only paths. */
function requireSelectedMotion(status: ExecutionOwnerStatus): {
  motion: DirectControlMotionStatus;
  observed: ObservedMotionTarget;
} {
  if (status.directControl === null || status.observedMotion === null) {
    throw new Error('The selected component has no observed motion target.');
  }
  return { motion: status.directControl, observed: status.observedMotion };
}

export interface RobotTargetSelection {
  controllableComponentId: string | null;
  motionTargetId: string | null;
  gripperTargetId: string | null;
}

function selectedStateFields(selection: RobotTargetSelection): Pick<
  RobotControlState,
  'selectedControllableComponentId' | 'selectedMotionTargetId' | 'selectedGripperTargetId'
> {
  return {
    selectedControllableComponentId: selection.controllableComponentId,
    selectedMotionTargetId: selection.motionTargetId,
    selectedGripperTargetId: selection.gripperTargetId,
  };
}

export function reconcileRobotTargetSelection(
  status: ExecutionOwnerStatus,
  selection: RobotTargetSelection,
  mountedToolByRobot: Readonly<Record<string, string>> = {},
): RobotTargetSelection {
  // Pair the arm with the Applied flange-mounted tool, when one is known.
  const motions = motionTargetsForStatus(status);
  const grippers = gripperTargetsForStatus(status);
  const commandable = new Set([
    ...motions.map((target) => target.controllableComponentId),
    ...grippers.map((target) => target.controllableComponentId),
  ]);
  const controllableComponentId = selection.controllableComponentId !== null && commandable.has(selection.controllableComponentId)
    ? selection.controllableComponentId
    : motions[0]?.controllableComponentId ?? grippers[0]?.controllableComponentId ?? null;
  if (controllableComponentId === null) {
    return { controllableComponentId: null, motionTargetId: null, gripperTargetId: null };
  }
  const ownMotions = motions.filter((target) => target.controllableComponentId === controllableComponentId);
  const toolComponentId = mountedToolByRobot[controllableComponentId] ?? controllableComponentId;
  const ownTools = grippers.filter((target) => target.controllableComponentId === toolComponentId);
  const selectedMotion = ownMotions.find((target) => (
    target.commandTargetId === selection.motionTargetId
  )) ?? ownMotions[0];
  const selectedTool = ownTools.find((target) => (
    target.commandTargetId === selection.gripperTargetId
  )) ?? ownTools[0];
  return {
    controllableComponentId,
    motionTargetId: selectedMotion?.commandTargetId ?? null,
    gripperTargetId: selectedTool?.commandTargetId ?? null,
  };
}

function observedMotionForSelection(
  status: ExecutionOwnerStatus,
  motion: DirectControlMotionStatus,
): ObservedMotionTarget | null {
  return status.observedMotions.find((observed) => (
    observed.controllableComponentId === motion.controllableComponentId
    && observed.commandTargetId === motion.commandTargetId
  )) ?? null;
}

function observedGripperForSelection(
  status: ExecutionOwnerStatus,
  gripper: DirectControlGripperStatus,
): ObservedGripperTarget | null {
  return status.observedGrippers.find((observed) => (
    observed.controllableComponentId === gripper.controllableComponentId
    && observed.commandTargetId === gripper.commandTargetId
  )) ?? null;
}

/** The descriptor row of the owner's Active TCP. */
export function selectedTcpOf(motion: DirectControlMotionStatus): DirectControlTcp | null {
  const entry = motion.activeTcp?.entry;
  return entry == null ? null : motion.targetDescriptor.tcps.find((tcp) => (
    tcpEntryIdentityKey(tcp.entry) === tcpEntryIdentityKey(entry)
  )) ?? null;
}

/**
 * Return the current owner snapshot projected onto one selected robot/tool.
 * The projection's `tcpFrameRoleId` and `currentTcpPose` are the selected
 * TCP's, so every Robot Control draft, intent, gizmo and move follows it.
 */
export function selectedStatusForState(
  state: Pick<RobotControlState, 'status' | 'selectedControllableComponentId' | 'selectedMotionTargetId' | 'selectedGripperTargetId'>
    & Partial<Pick<RobotControlState, 'mountedToolByRobot'>>,
): ExecutionOwnerStatus | null {
  if (state.status === null) return null;
  const selection = reconcileRobotTargetSelection(state.status, {
    controllableComponentId: state.selectedControllableComponentId,
    motionTargetId: state.selectedMotionTargetId,
    gripperTargetId: state.selectedGripperTargetId,
  }, state.mountedToolByRobot);
  const motion = motionTargetsForStatus(state.status).find((target) => (
    target.controllableComponentId === selection.controllableComponentId
    && target.commandTargetId === selection.motionTargetId
  )) ?? null;
  const gripper = gripperTargetsForStatus(state.status).find((target) => (
    target.controllableComponentId === (state.mountedToolByRobot?.[selection.controllableComponentId ?? '']
      ?? selection.controllableComponentId)
    && target.commandTargetId === selection.gripperTargetId
  )) ?? null;
  const tcp = motion === null ? null : selectedTcpOf(motion);
  const observed = motion === null ? null : observedMotionForSelection(state.status, motion);
  return {
    ...state.status,
    directControl: motion === null ? null : {
      ...motion,
      tcpFrameRoleId: tcp?.frameRoleId ?? null,
      selectedTcpEntryKey: tcp === null ? undefined : tcpEntryIdentityKey(tcp.entry),
    },
    directControlGripper: gripper,
    observedMotion: observed === null ? null : {
      ...observed,
      currentTcpPose: tcp === null ? null : observed.tcpPoses[tcpEntryIdentityKey(tcp.entry)] ?? null,
    },
    observedGripper: gripper === null
      ? null
      : observedGripperForSelection(state.status, gripper),
  };
}

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

/** Return the mode only when the accepted receipt names this exact operation. */
export function robotControlAcceptedOperationMode(
  state: RobotControlState,
): RobotControlMode | null {
  const operation = state.operation;
  const accepted = state.acceptedOperationMode;
  if (!robotControlHasActiveOperation(state) || operation === null
    || operationStateIsTerminal(operation.state) || accepted === null
    || accepted.ownerEpoch !== state.status?.ownerEpoch
    || accepted.operationId !== operation.operationId
    || (operation.controllableComponentId !== undefined
      && accepted.controllableComponentId !== operation.controllableComponentId)
    || (operation.commandTargetId !== undefined
      && accepted.commandTargetId !== operation.commandTargetId)) return null;
  return accepted.mode;
}

/**
 * Preview-stage planner/collision refusals are brief feedback. Other failed
 * requests, including Approve and rule holds, remain operational warnings.
 */
export function robotControlHasTransientPreviewRefusal(
  state: RobotControlState,
): boolean {
  if (state.phase !== 'blocked' || state.intent !== null || state.errorLocal) return false;
  if (state.previewIssue !== null) return true;
  const error = state.errorRefusal;
  if (error === null || error.code === 'rule_holds') return false;
  if (error.code === 'validation_collision') return true;
  if (error.code === 'ik_failed') {
    return error.details?.reason === 'native_ik_no_solution';
  }
  if (error.code !== 'planning_failed') return false;
  const namedCollisionPair = typeof error.details?.collision_body_a === 'string'
    && typeof error.details.collision_body_b === 'string';
  return error.details?.classification === 'clearance'
    || namedCollisionPair
    || ['native_goal_state_collision', 'native_start_state_collision', 'native_rrt_failed']
      .includes(String(error.details?.reason ?? ''));
}

/** Candidate presentation follows the backend's manual-control admission.
 * A retained or active manual operation remains the sole owner of its result. */
export function robotControlCandidatePresentationAllowed(
  state: RobotControlState,
  manualControlAllowed: boolean,
): boolean {
  const cleanCompletedResult = state.operation !== null
    && !state.operation.resident
    && state.operation.state === 'completed'
    && state.operation.terminalError === null;
  return manualControlAllowed
    && (state.operation === null || cleanCompletedResult)
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
  if (status.directControl === null) return false;
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

/** Upper bound of a motion's own collision clearance (the backend's). */
export const MAX_CLEARANCE_M = 0.05;

function motionSettingsAreValid(status: ExecutionOwnerStatus, settings: ManualMotionSettings): boolean {
  const { jointLimits, clearanceM, ...positiveValues } = settings;
  const clearanceValid = clearanceM === null
    || (Number.isFinite(clearanceM) && clearanceM >= 0 && clearanceM <= MAX_CLEARANCE_M);
  // Motion-only limits apply only when a motion target is selected.
  const descriptor = status.directControl?.targetDescriptor;
  if (descriptor === undefined) {
    return Object.values(positiveValues).every((value) => Number.isFinite(value) && value > 0)
      && clearanceValid;
  }
  return Object.values(positiveValues).every((value) => Number.isFinite(value) && value > 0)
    && clearanceValid
    && settings.velocityScale <= 1 && settings.accelerationScale <= 1
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

function initialMotionSettings(
  status: ExecutionOwnerStatus,
  cartesianDefaults: ManualCartesianDefaults | null = null,
): ManualMotionSettings {
  return {
    ...DEFAULT_MANUAL_MOTION_SETTINGS,
    cartesianTranslationToleranceM: cartesianDefaults?.translationToleranceM
      ?? DEFAULT_MANUAL_MOTION_SETTINGS.cartesianTranslationToleranceM,
    tcpLinearSpeedMps: Math.min(DEFAULT_MANUAL_MOTION_SETTINGS.tcpLinearSpeedMps,
      status.directControl?.targetDescriptor.operationalLimit?.maximumTcpLinearSpeedMps ?? Infinity),
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
  if (!Number.isFinite(position) || status.directControlGripper === null) return false;
  const { minPosition, maxPosition } = status.directControlGripper.targetDescriptor;
  return minPosition === null && maxPosition === null
    ? true
    : minPosition !== null
      && maxPosition !== null
      && position >= minPosition
      && position <= maxPosition;
}

/**
 * Evidence computed on the latest Scene may be approved: the runtime collision
 * monitor checks the newer Scene during the motion. Owner, bundle, graph and
 * target changes discard the preview before this is asked.
 */
export function robotControlPreviewApprovable(state: RobotControlState): boolean {
  return state.phase === 'ready'
    && state.preview !== null
    && state.intent !== null
    && state.status !== null;
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
    && receipt.target.componentId === participant.target.component_id
    && receipt.target.commandTargetId === participant.target.command_target_id
    && receipt.requiredCapabilityId === participant.required_capability_id;
}

function receiptMatchesCurrentDraft(
  state: RobotControlState,
  receipt: DirectControlPreviewReceipt,
): boolean {
  const { intent } = state;
  const status = selectedStatusForState(state);
  if (intent === null || status === null) return false;
  const payload = intent.participants[0].payload;
  if (!commonReceiptMatches(status, intent, receipt)) return false;
  if (receipt.schemaVersion !== 'manyforge.direct_control_capability_command_preview.v2') {
    if (payload.kind !== 'motion' || status.directControl === null) return false;
    const expectedNames = status.directControl.targetDescriptor.joints.map(
      (joint) => joint.sourceJointName,
    );
    return arraysEqual(receipt.sourceJointNames, expectedNames)
      && receipt.finalPositionsRad.length === expectedNames.length;
  }
  if (payload.kind !== 'capability_command') return false;
  if (status.directControlGripper === null) return false;
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
    draftPositionsRad: status.observedMotion === null
      ? [] : [...status.observedMotion.positionsRad],
    draftPose: status.observedMotion?.currentTcpPose == null
      ? IDENTITY_POSE : copyPose(status.observedMotion.currentTcpPose),
    draftGripperPosition: status.observedGripper?.position ?? null,
  };
}

export function robotControlHasLocalDraft(state: RobotControlState): boolean {
  return state.operation === null
    && (state.poseDragging || state.phase === 'planning' || state.phase === 'ready'
      || state.phase === 'approving' || state.phase === 'blocked');
}

function operationMatchesSelectedTarget(
  _status: ExecutionOwnerStatus,
  selectedStatus: ExecutionOwnerStatus,
  operation: OperationObservation,
): boolean {
  if (operation.controllableComponentId === undefined || operation.commandTargetId === undefined) return false;
  return [selectedStatus.directControl, selectedStatus.directControlGripper].some((target) =>
    target !== null && target.controllableComponentId === operation.controllableComponentId
      && target.commandTargetId === operation.commandTargetId);
}

function resolveStatus(
  state: RobotControlState,
  status: ExecutionOwnerStatus,
  initialize: boolean,
  cartesianDefaults: ManualCartesianDefaults | null = null,
): RobotControlState {
  if (state.status !== null && (state.status.graphRevision !== status.graphRevision
    || state.status.retainedBundleId !== status.retainedBundleId)) {
    state = { ...state, mountedToolByRobot: {} };
  }
  const selection = reconcileRobotTargetSelection(status, {
    controllableComponentId: state.selectedControllableComponentId,
    motionTargetId: state.selectedMotionTargetId,
    gripperTargetId: state.selectedGripperTargetId,
  }, state.mountedToolByRobot);
  const selectedStatus = selectedStatusForState({
    status,
    selectedControllableComponentId: selection.controllableComponentId,
    selectedMotionTargetId: selection.motionTargetId,
    selectedGripperTargetId: selection.gripperTargetId,
    mountedToolByRobot: state.mountedToolByRobot,
  });
  if (selectedStatus === null) return state;
  const previousSelectedStatus = state.status === null
    ? null
    : selectedStatusForState(state);
  if (state.mode === 'gripper' && selectedStatus.directControlGripper === null) {
    // The selected component has no tool (arm-only).
    state = { ...state, mode: 'joint' };
  } else if (state.mode !== 'gripper' && selectedStatus.directControl === null
    && selectedStatus.directControlGripper !== null) {
    // The selected component has no motion target (gripper-only).
    state = { ...state, mode: 'gripper' };
  } else if (state.mode !== 'gripper' && state.mode !== 'joint'
    && selectedStatus.directControl?.tcpFrameRoleId === null) {
    // No TCP: joint jog only.
    state = { ...state, mode: 'joint' };
  }
  const selectedRun = state.mode === 'gripper'
    ? selectedStatus.directControlGripper?.programRunId ?? null
    : selectedStatus.directControl?.programRunId ?? null;
  const projected = status.manualOperations.filter((operation) => {
    if (operation.programRunId !== selectedRun) return false;
    return operationMatchesSelectedTarget(status, selectedStatus, operation);
  });
  const retainedOperation = state.operation?.resident
    ? status.manualOperations.find((operation) => (
      operation.operationId === state.operation?.operationId
    )) ?? null
    : null;
  const projectedOperation = projected.length === 1 ? projected[0] : null;
  const projectionError = projected.length > 1
    ? 'More than one direct-control operation is resident; Robot Control will not guess which operation to own.'
    : null;
  const resolvedOperation = retainedOperation ?? (
    projectionError === null ? projectedOperation : null
  );
  const resolvedPhase = projectionError !== null
    ? 'blocked'
    : resolvedOperation === null
      ? 'idle'
      : phaseForOperation(resolvedOperation);
  const selectionChanged = state.selectedControllableComponentId !== selection.controllableComponentId
    || state.selectedMotionTargetId !== selection.motionTargetId
    || state.selectedGripperTargetId !== selection.gripperTargetId
    || JSON.stringify(previousSelectedStatus?.directControl?.activeTcp?.entry ?? null)
      !== JSON.stringify(selectedStatus.directControl?.activeTcp?.entry ?? null);
  const selectedBindingChanged = previousSelectedStatus !== null && (
    previousSelectedStatus.directControl?.programBindingId
      !== selectedStatus.directControl?.programBindingId
    || previousSelectedStatus.directControlGripper?.programBindingId
      !== selectedStatus.directControlGripper?.programBindingId
    || previousSelectedStatus.directControl?.controllableComponentId
      !== selectedStatus.directControl?.controllableComponentId
    || previousSelectedStatus.directControl?.commandTargetId
      !== selectedStatus.directControl?.commandTargetId
    || previousSelectedStatus.directControlGripper?.controllableComponentId
      !== selectedStatus.directControlGripper?.controllableComponentId
    || previousSelectedStatus.directControlGripper?.commandTargetId
      !== selectedStatus.directControlGripper?.commandTargetId
  );
  if (state.status !== null && (
    state.status.ownerEpoch !== status.ownerEpoch
    || state.status.retainedBundleId !== status.retainedBundleId
    || state.status.graphRevision !== status.graphRevision
    || selectedBindingChanged
  )) {
    // Owner/bundle/graph replacement always discards the prior preview
    // evidence. An accepted, uncertain or resident operation keeps its
    // explicit outcome and attention. Discarded uncommitted manual work gets
    // exactly one informational notice; a purely idle replacement rebases
    // silently without producing an operational error.
    const discardedEvidence = state.status.ownerEpoch !== status.ownerEpoch
      ? 'The execution owner restarted. The prior preview was discarded.'
      : 'The target binding changed. The prior preview was discarded.';
    const discardedUncommittedWork = robotControlHasLocalDraft(state)
      || state.intent !== null
      || state.preview !== null
      || state.ghost !== null;
    const replacementRequiresAttention = resolvedOperation !== null
      && (resolvedOperation.resident || resolvedOperation.terminalError !== null);
    return {
      ...initialRobotControlState,
      mode: state.mode,
      mountedToolByRobot: state.mountedToolByRobot,
      motionSettings: initialMotionSettings(selectedStatus, cartesianDefaults),
      cartesianDefaults,
      status,
      ...selectedStateFields(selection),
      phase: resolvedPhase,
      ...draftFromStatus(selectedStatus),
      generation: state.generation + 1,
      operation: resolvedOperation,
      error: projectionError
        ?? (replacementRequiresAttention ? discardedEvidence : null),
      errorLocal: false,
      ownerNotice: !replacementRequiresAttention && discardedUncommittedWork
        ? 'The execution owner changed; uncommitted manual work was discarded.'
        : null,
    };
  }
  if (initialize || state.status === null) {
    return {
      ...state,
      status,
      moveToPath: moveToPathFor(selectedStatus, state.moveToPath),
      ...selectedStateFields(selection),
      phase: resolvedPhase,
      ...draftFromStatus(selectedStatus),
      motionSettings: state.status === null
        ? initialMotionSettings(selectedStatus, cartesianDefaults)
        : state.motionSettings,
      cartesianDefaults: state.status === null ? cartesianDefaults : state.cartesianDefaults,
      intent: null,
      preview: null,
      diagnosticPreview: null,
      previewSampleIndex: 0,
      ghost: null,
      ghostStale: false,
      operation: resolvedOperation,
      previewIssue: null,
      error: projectionError ?? resolvedOperation?.terminalError ?? null,
      errorRefusal: null,
      errorLocal: false,
      ownerNotice: null,
    };
  }
  if (selectionChanged) {
    return {
      ...state,
      status,
      moveToPath: moveToPathFor(selectedStatus, state.moveToPath),
      ...selectedStateFields(selection),
      phase: resolvedOperation === null ? 'idle' : phaseForOperation(resolvedOperation),
      ...draftFromStatus(selectedStatus),
      generation: state.generation + 1,
      intent: null,
      preview: null,
      diagnosticPreview: null,
      previewSampleIndex: 0,
      ghost: null,
      ghostStale: false,
      operation: resolvedOperation,
      previewIssue: null,
      error: resolvedOperation?.terminalError ?? null,
      errorRefusal: null,
      errorLocal: false,
      ownerNotice: null,
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
      errorRefusal: null,
      errorLocal: false,
      ownerNotice: null,
    };
  }
  // The latest settled result stays visible through the same owner snapshot,
  // but repeated publication must not overwrite a new draft or an approval
  // still waiting for a *new* operation to become visible.
  const alreadyPresentedTerminal = resolvedOperation !== null && !resolvedOperation.resident
    && state.status.manualOperations.some((previous) => !previous.resident
      && previous.operationId === resolvedOperation.operationId);
  if (alreadyPresentedTerminal) {
    return { ...state, status };
  }
  if (resolvedOperation !== null) {
    return {
      ...state,
      status,
      phase: phaseForOperation(resolvedOperation),
      poseDragging: false,
      moveToDraft: null,
      operation: resolvedOperation,
      previewIssue: null,
      intent: null,
      preview: null,
      diagnosticPreview: null,
      previewSampleIndex: 0,
      ghost: null,
      ghostStale: false,
      error: resolvedOperation.terminalError ?? state.error,
      errorRefusal: resolvedOperation.terminalError === null ? state.errorRefusal : null,
      errorLocal: resolvedOperation.terminalError === null ? state.errorLocal : false,
      ...(operationStateIsTerminal(resolvedOperation.state)
        ? draftFromStatus(selectedStatus)
        : {}),
    };
  }
  if (state.operation !== null) {
    return {
      ...state,
      status,
      phase: 'idle',
      ...draftFromStatus(selectedStatus),
      operation: null,
      intent: null,
      preview: null,
      diagnosticPreview: null,
      previewSampleIndex: 0,
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
    diagnosticPreview: null,
    previewSampleIndex: 0,
    ghostStale: state.ghost !== null,
    operation: null,
    previewIssue: null,
    error: null,
    errorRefusal: null,
    errorLocal: false,
    ownerNotice: null,
  };
}

function changeSelection(
  state: RobotControlState,
  selection: RobotTargetSelection,
): RobotControlState {
  if (state.status === null
    || (state.selectedControllableComponentId === selection.controllableComponentId
      && state.selectedMotionTargetId === selection.motionTargetId
      && state.selectedGripperTargetId === selection.gripperTargetId)) {
    return state;
  }
  const selected = selectedStatusForState({
    status: state.status,
    selectedControllableComponentId: selection.controllableComponentId,
    selectedMotionTargetId: selection.motionTargetId,
    selectedGripperTargetId: selection.gripperTargetId,
    mountedToolByRobot: state.mountedToolByRobot,
  });
  if (selected === null) return state;
  return {
    ...state,
    moveToPath: moveToPathFor(selected, state.moveToPath),
    ...selectedStateFields(selection),
    phase: state.operation === null ? 'idle' : phaseForOperation(state.operation),
    ...draftFromStatus(selected),
    generation: state.generation + 1,
    intent: null,
    preview: null,
    diagnosticPreview: null,
    previewSampleIndex: 0,
    ghost: null,
    ghostStale: false,
    previewIssue: null,
    error: state.operation?.terminalError ?? null,
    errorRefusal: null,
    errorLocal: false,
    ownerNotice: null,
  };
}

export function robotControlReducer(
  state: RobotControlState,
  action: RobotControlAction,
): RobotControlState {
  if (state.operation?.resident && ['edit', 'edit_joint', 'edit_move_to_joint', 'edit_pose', 'edit_gripper', 'edit_motion_settings', 'select_move_source', 'edit_move_options', 'begin_pose_drag', 'finish_pose_drag']
    .includes(action.type)) return state;
  switch (action.type) {
    case 'preview_hidden': {
      const draftPhase = state.phase === 'planning' || state.phase === 'ready' || state.phase === 'blocked';
      return { ...state, phase: draftPhase ? 'planning' : state.phase,
        generation: draftPhase ? state.generation + 1 : state.generation,
        intent: null, preview: null, diagnosticPreview: null, previewSampleIndex: 0,
        ghost: null, ghostStale: false, previewIssue: null,
        ...(draftPhase ? { error: null, errorRefusal: null, errorLocal: false } : {}),
      };
    }
    case 'load':
      return state.status === null
        ? { ...state, phase: 'loading', error: null, errorRefusal: null, errorLocal: false, ownerNotice: null }
        : { ...state, error: null, errorRefusal: null, errorLocal: false, ownerNotice: null };
    case 'status_resolved':
      return resolveStatus(state, action.status, true, action.cartesianDefaults);
    case 'status_refreshed':
      return resolveStatus(state, action.status, false, action.cartesianDefaults);
    case 'program_cartesian_defaults_changed': {
      // Re-sent when an operation ends, so a change during one still applies.
      if (state.status === null || robotControlHasActiveOperation(state)) return state;
      const previous = state.cartesianDefaults;
      const motionSettings = {
        ...state.motionSettings,
        cartesianTranslationToleranceM: followCartesianDefault(
          state.motionSettings.cartesianTranslationToleranceM,
          previous?.translationToleranceM ?? null,
          action.defaults.translationToleranceM,
        ),
      };
      const unchanged = motionSettings.cartesianTranslationToleranceM
          === state.motionSettings.cartesianTranslationToleranceM;
      if (unchanged) return { ...state, cartesianDefaults: action.defaults };
      if (!motionSettingsAreValid(selectedStatusForState(state)!, motionSettings)) return state;
      return robotControlHasLocalDraft(state)
        ? invalidateDraft(state, { motionSettings, cartesianDefaults: action.defaults })
        : { ...state, motionSettings, cartesianDefaults: action.defaults };
    }
    case 'select_robot': {
      if (state.status === null) return state;
      const selection = reconcileRobotTargetSelection(state.status, {
        controllableComponentId: action.controllableComponentId,
        motionTargetId: action.motionTargetId,
        gripperTargetId: state.selectedGripperTargetId,
      }, state.mountedToolByRobot);
      return changeSelection(state, selection);
    }
    case 'active_tcp_change_started':
      return {
        ...state,
        ...(state.status === null ? {} : draftFromStatus(selectedStatusForState(state)!)),
        phase: state.operation === null ? 'idle' : phaseForOperation(state.operation),
        poseDragging: false,
        moveToDraft: null,
        generation: state.generation + 1,
        intent: null,
        preview: null,
        diagnosticPreview: null,
        previewSampleIndex: 0,
        ghost: null,
        ghostStale: false,
        previewIssue: null,
      };
    case 'mounted_tools_changed': {
      if (action.graphRevision !== undefined && (state.status === null
        || state.status.graphRevision !== action.graphRevision
        || state.status.retainedBundleId !== action.retainedBundleId)) return state;
      if (JSON.stringify(state.mountedToolByRobot) === JSON.stringify(action.mounts)) return state;
      if (state.phase === 'approving' || robotControlHasActiveOperation(state)) return state;
      const next = { ...state, mountedToolByRobot: action.mounts };
      if (state.status === null) return next;
      const selected = selectedStatusForState(next);
      if (selected === null) return next;
      const selection = reconcileRobotTargetSelection(state.status, {
        controllableComponentId: state.selectedControllableComponentId,
        motionTargetId: state.selectedMotionTargetId,
        gripperTargetId: state.selectedGripperTargetId,
      }, action.mounts);
      return {
        ...next,
        ...selectedStateFields(selection),
        mode: state.mode === 'gripper' && selected.directControlGripper === null && selected.directControl !== null
          ? 'joint' : state.mode,
        phase: state.operation === null ? 'idle' : phaseForOperation(state.operation),
        ...draftFromStatus(selected),
        generation: state.generation + 1,
        intent: null, preview: null, diagnosticPreview: null, previewSampleIndex: 0,
        ghost: null, ghostStale: false, moveToDraft: null,
      };
    }
    case 'select_tool': {
      if (state.status === null) return state;
      const selection = reconcileRobotTargetSelection(state.status, {
        controllableComponentId: state.selectedControllableComponentId,
        motionTargetId: state.selectedMotionTargetId,
        gripperTargetId: action.gripperTargetId,
      }, state.mountedToolByRobot);
      return changeSelection(state, selection);
    }
    case 'manual_scope_changed':
      return robotControlHasLocalDraft(state) ? invalidateDraft(state, {}) : state;
    case 'load_failed':
      return state.status === null
        ? { ...state, phase: 'unavailable', error: action.error, errorRefusal: null, errorLocal: false }
        : { ...state, error: action.error, errorRefusal: null, errorLocal: false };
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
        ...draftFromStatus(selectedStatusForState(state)!),
        generation: state.generation + 1,
        intent: null,
        preview: null,
        diagnosticPreview: null,
        previewSampleIndex: 0,
        ghost: null,
        ghostStale: false,
        operation: null,
        previewIssue: null,
        error: null,
        errorRefusal: null,
        errorLocal: false,
        ownerNotice: null,
      };
    case 'select_mode':
      if (
        state.status === null
        || robotControlHasActiveOperation(state)
        || state.mode === action.mode
        || (action.mode === 'gripper'
          && selectedStatusForState(state)?.directControlGripper == null)
        || (action.mode !== 'gripper'
          && selectedStatusForState(state)?.directControl == null)
        // No TCP: joint jog only.
        || (action.mode !== 'gripper' && action.mode !== 'joint'
          && selectedStatusForState(state)?.directControl?.tcpFrameRoleId === null)
      ) return state;
      // A new editor must not relabel the last settled operation as its own
      // preview. resolveStatus still retains any resident operation/fault.
      return resolveStatus({
        ...state,
        mode: action.mode,
        moveToKind: 'pose',
        phase: 'idle',
        ...draftFromStatus(selectedStatusForState(state)!),
        generation: state.generation + 1,
        intent: null,
        preview: null,
        diagnosticPreview: null,
        previewSampleIndex: 0,
        ghost: null,
        ghostStale: false,
        operation: null,
        previewIssue: null,
        error: null,
        errorRefusal: null,
        errorLocal: false,
        ownerNotice: null,
      }, state.status, false);
    case 'edit':
    case 'edit_joint': {
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      const selectedStatus = selectedStatusForState(state);
      if (selectedStatus === null || selectedStatus.observedMotion === null) return state;
      const baseline = robotControlHasLocalDraft(state)
        ? state.draftPositionsRad : selectedStatus.observedMotion.positionsRad;
      const positionsRad = action.type === 'edit' ? action.positionsRad : [...baseline];
      if (action.type === 'edit_joint') {
        if (!Number.isInteger(action.jointIndex) || action.jointIndex < 0
          || action.jointIndex >= positionsRad.length) return state;
        positionsRad[action.jointIndex] = action.positionRad;
      }
      if (!positionsAreValid(selectedStatus, positionsRad)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          diagnosticPreview: null,
          previewSampleIndex: 0,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The authored joint target is outside its retained limits.',
          errorRefusal: null,
          errorLocal: true,
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
      const selectedStatus = selectedStatusForState(state);
      if (selectedStatus === null) return state;
      const motionSettings = {
        ...state.motionSettings,
        ...action.update,
        jointLimits: { ...state.motionSettings.jointLimits },
      };
      for (const [joint, limit] of Object.entries(action.update.jointLimits ?? {})) {
        motionSettings.jointLimits[joint] = { ...motionSettings.jointLimits[joint], ...limit };
      }
      if (!motionSettingsAreValid(selectedStatus, motionSettings)) return state;
      // Editing limits alone does not invent a target or start a no-op preview.
      return robotControlHasLocalDraft(state)
        ? invalidateDraft(state, { motionSettings })
        : { ...state, motionSettings };
    }
    case 'select_move_source': {
      if (state.status === null || robotControlHasActiveOperation(state)) return state;
      const selectedStatus = selectedStatusForState(state);
      if (selectedStatus === null || selectedStatus.observedMotion === null) return state;
      const currentTcpPose = selectedStatus.observedMotion.currentTcpPose;
      if (action.source.kind === 'pose' && currentTcpPose === null) return state;
      const moveToDraft: MoveToDraft = {
        source: structuredClone(action.source),
        currentTcpPose: poseTupleFromValue(currentTcpPose ?? IDENTITY_POSE),
        options: structuredClone(state.moveToDraft?.options ?? DEFAULT_MOVE_TO_OPTIONS),
      };
      if (action.source.kind === 'joint') {
        return robotControlReducer({ ...state, moveToDraft }, { type: 'edit_move_to_joint', positionsRad: action.source.positionsRad });
      }
      const pose = computeCartesianMoveDraft(action.source.pose, moveToDraft.currentTcpPose, moveToDraft.options);
      if (pose === null) return state;
      return invalidateDraft(state, { mode: 'move_to', moveToKind: 'pose', moveToDraft, draftPose: poseValueFromTuple(pose) });
    }
    case 'set_move_to_path': {
      if (state.mode !== 'move_to' || state.moveToKind !== 'pose'
        || robotControlHasActiveOperation(state) || state.moveToPath === action.path) return state;
      const selected = selectedStatusForState(state);
      if (action.path === 'linear' && !moveToLinearAvailable(selected)) return state;
      if (action.path === 'free' && !moveToFreeAvailable(selected)) return state;
      return invalidateDraft(state, { moveToPath: action.path });
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
        || selectedStatusForState(state)?.observedMotion?.currentTcpPose == null
        || !['pose_goal', 'cartesian_path', 'move_to'].includes(state.mode)
        || (state.mode === 'move_to' && state.moveToDraft?.source.kind !== 'pose')) return state;
      return {
        ...state,
        draftPose: robotControlHasLocalDraft(state) ? state.draftPose
          : copyPose(selectedStatusForState(state)!.observedMotion!.currentTcpPose!),
        poseDragging: true,
        generation: state.generation + 1,
        diagnosticPreview: null, previewSampleIndex: 0,
        ownerNotice: null,
      };
    case 'finish_pose_drag': {
      if (!state.poseDragging) return state;
      // A refresh replaced the target during the drag: its release is stale.
      if (action.generation !== state.generation) {
        return { ...state, poseDragging: false, ownerNotice: poseDragNotice('stale') };
      }
      const settled = { ...state, poseDragging: false };
      if (action.discard === 'context_changed') {
        return { ...robotControlReducer(settled, { type: 'revert' }), ownerNotice: poseDragNotice('context_changed') };
      }
      if (action.discard !== undefined) return { ...settled, ownerNotice: poseDragNotice(action.discard) };
      if (action.pose === null) return settled;
      if (samePose(poseTupleFromValue(state.draftPose), poseTupleFromValue(action.pose))) {
        return { ...settled, ownerNotice: poseDragNotice('no_change') };
      }
      return robotControlReducer(settled, { type: 'edit_pose', pose: action.pose });
    }
    case 'edit_move_to_joint': {
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      const selectedStatus = selectedStatusForState(state);
      if (selectedStatus === null) return state;
      if (!positionsAreValid(selectedStatus, action.positionsRad)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          diagnosticPreview: null,
          previewSampleIndex: 0,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The stored joint target is outside its retained limits.',
          errorRefusal: null,
          errorLocal: true,
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
      const selectedStatus = selectedStatusForState(state);
      if (selectedStatus === null || selectedStatus.observedMotion?.currentTcpPose == null) return state;
      if (!poseIsValid(action.pose)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          diagnosticPreview: null,
          previewSampleIndex: 0,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The authored TCP pose is invalid.',
          errorRefusal: null,
          errorLocal: true,
        };
      }
      if (posesEqual(robotControlHasLocalDraft(state)
        ? state.draftPose : selectedStatus.observedMotion.currentTcpPose, action.pose)
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
    case 'edit_gripper': {
      if (state.status === null || robotControlHasActiveOperation(state)) {
        return state;
      }
      const selectedStatus = selectedStatusForState(state);
      if (selectedStatus === null || selectedStatus.directControlGripper === null) {
        return state;
      }
      if (!gripperPositionIsValid(selectedStatus, action.position)) {
        return {
          ...state,
          phase: 'blocked',
          generation: state.generation + 1,
          intent: null,
          preview: null,
          diagnosticPreview: null,
          previewSampleIndex: 0,
          ghostStale: state.ghost !== null,
          previewIssue: null,
          error: 'The authored gripper target is outside its retained limits.',
          errorRefusal: null,
          errorLocal: true,
        };
      }
      if (
        state.mode === 'gripper'
          && (robotControlHasLocalDraft(state)
          ? state.draftGripperPosition : selectedStatus.observedGripper?.position) === action.position
      ) return state;
      return invalidateDraft(state, {
        mode: 'gripper',
        draftGripperPosition: action.position,
      });
    }
    case 'preview_started':
      if (
        action.generation !== state.generation
        || state.poseDragging
        || state.phase !== 'planning'
      ) return state;
      return { ...state, intent: action.intent, acceptedOperationMode: null };
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
          diagnosticPreview: null,
          previewSampleIndex: 0,
          ghostStale: state.ghost !== null,
          error: 'The preview evidence does not match the current draft.',
          errorRefusal: null,
          errorLocal: false,
        };
      }
      return {
        ...state,
        phase: 'ready',
        diagnosticPreview: null,
        previewSampleIndex: action.receipt.schemaVersion === 'manyforge.direct_control_motion_preview.v4'
          ? Math.max(0, action.receipt.pathPreview.samples.length - 1) : 0,
        preview: action.receipt,
        ghost: action.receipt.schemaVersion
          !== 'manyforge.direct_control_capability_command_preview.v2'
          ? action.receipt
          : state.ghost,
        ghostStale: action.receipt.schemaVersion
          !== 'manyforge.direct_control_capability_command_preview.v2'
          ? false
          : state.ghost !== null,
        previewIssue: null,
        error: null,
        errorRefusal: null,
        errorLocal: false,
        ownerNotice: null,
      };
    case 'preview_rejected':
      if (action.generation !== state.generation || state.poseDragging) return state;
      return {
        ...state,
        phase: 'blocked',
        intent: null,
        preview: null,
        diagnosticPreview: null,
        previewSampleIndex: 0,
        ghostStale: state.ghost !== null,
        previewIssue: action.targetRejection ?? null,
        error: action.targetRejection ? null : action.error,
        errorRefusal: action.targetRejection ? null : action.errorRefusal ?? null,
        errorLocal: false,
      };
    case 'preview_diagnostic':
      if (action.generation !== state.generation || state.phase !== 'blocked' || state.poseDragging) return state;
      return { ...state, diagnosticPreview: action.diagnostic,
        previewSampleIndex: Math.max(0, action.diagnostic.samples.length - 1) };
    case 'select_preview_sample': {
      const path = state.diagnosticPreview ?? (state.preview?.schemaVersion === 'manyforge.direct_control_motion_preview.v4'
        ? state.preview.pathPreview : undefined);
      if (path === undefined || path === null || !Number.isInteger(action.index)
          || action.index < 0 || action.index >= path.samples.length) return state;
      return { ...state, previewSampleIndex: action.index };
    }
    case 'approve_started':
      if (!robotControlPreviewApprovable(state) || state.poseDragging) return state;
      return { ...state, phase: 'approving', acceptedOperationMode: null,
        error: null, errorRefusal: null, errorLocal: false };
    case 'operation_mode_accepted': {
      const intent = state.intent;
      if (action.generation !== state.generation || state.status?.ownerEpoch !== action.ownerEpoch
        || intent === null || intent.request_context.request_id !== action.requestId) return state;
      const participant = intent.participants[0];
      return {
        ...state,
        acceptedOperationMode: {
          ownerEpoch: action.ownerEpoch,
          operationId: action.operationId,
          mode: action.mode,
          controllableComponentId: participant.target.component_id,
          commandTargetId: participant.target.command_target_id,
        },
      };
    }
    case 'approve_rejected':
      if (state.phase !== 'approving' || action.generation !== state.generation) return state;
      return {
        ...state,
        phase: 'blocked',
        preview: null,
        diagnosticPreview: null,
        previewSampleIndex: 0,
        ghostStale: state.ghost !== null,
        error: action.error,
        errorRefusal: action.errorRefusal ?? null,
        errorLocal: false,
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
        errorRefusal: action.operation.terminalError === null ? state.errorRefusal : null,
        errorLocal: action.operation.terminalError === null ? state.errorLocal : false,
      };
    case 'operation_control_failed':
      return state.status?.ownerEpoch === action.ownerEpoch
        && state.operation?.operationId === action.operationId
        ? { ...state, error: action.error, errorRefusal: action.errorRefusal ?? null, errorLocal: false }
        : state;
    case 'dismiss_error':
      return state.error === null ? state : { ...state, error: null, errorRefusal: null, errorLocal: false };
    case 'revert': {
      if (robotControlHasActiveOperation(state)) return state;
      const selectedStatus = state.status === null ? null : selectedStatusForState(state);
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
          : draftFromStatus(selectedStatus!)),
        generation: state.generation + 1,
        intent: null,
        preview: null,
        diagnosticPreview: null,
        previewSampleIndex: 0,
        ghost: null,
        ghostStale: false,
        operation: state.operation?.resident ? state.operation : null,
        previewIssue: null,
        error: state.operation?.resident ? state.operation.terminalError : null,
        errorRefusal: null,
        errorLocal: false,
        ownerNotice: null,
      };
    }
    case 'clear':
      return {
        ...initialRobotControlState,
        phase: 'unavailable',
        generation: state.generation + 1,
      };
  }
}

/** Whether the selected target advertises the same Cartesian planner as the Cartesian tab. */
export function moveToLinearAvailable(status: ExecutionOwnerStatus | null): boolean {
  return status?.directControl?.targetDescriptor.contracts.supportingProviders.some(
    (provider) => provider.capabilityId === 'planning.motion.cartesian_path',
  ) ?? false;
}

/** A Free Move To plans a pose goal. */
export function moveToFreeAvailable(status: ExecutionOwnerStatus | null): boolean {
  return status?.directControl?.targetDescriptor.contracts.supportingProviders.some(
    (provider) => provider.capabilityId === 'planning.motion.pose_goal',
  ) ?? false;
}

/** Keep the chosen path when the target offers it; otherwise the one it does offer. */
function moveToPathFor(
  status: ExecutionOwnerStatus | null,
  current: RobotControlState['moveToPath'],
): RobotControlState['moveToPath'] {
  if (!moveToLinearAvailable(status)) return 'free';
  if (!moveToFreeAvailable(status)) return 'linear';
  return current;
}

function requireMotionStatus(
  status: ExecutionOwnerStatus,
  capability: string,
): void {
  if (!requireSelectedMotion(status).observed.complete) {
    throw new Error('Authoritative joint state is incomplete.');
  }
  const descriptor = requireSelectedMotion(status).motion.targetDescriptor;
  const motion = requireSelectedMotion(status).motion;
  const selectedTcp = selectedTcpOf(motion);
  if (!descriptor.frames.some((frame) => frame.frameComponentId === motion.baseFrame.componentId && frame.frameRoleId === motion.baseFrame.frameId)) {
    throw new Error('The direct-control base/TCP frame contract is incomplete.');
  }
  // A joint move needs no TCP. Cartesian use must be represented by
  // the old endpoint binding and carried by this robot until A4 replaces it.
  if (capability !== 'planning.motion.joint_goal') {
    if (selectedTcp === null) {
      throw new Error('The direct-control base/TCP frame contract is incomplete.');
    }
    const disabledReason = directControlTcpDisabledReason(selectedTcp);
    if (disabledReason !== null) throw new Error(disabledReason);
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
  const selectedMotion = requireSelectedMotion(status).motion;
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
  const payload: MotionParticipantPayload = {
    kind: 'motion',
    move_id: `robot-control-${requestId}`,
    base_frame: { component_id: selectedMotion.baseFrame.componentId, frame_id: selectedMotion.baseFrame.frameId },
    tcp_endpoint: { kind: 'current' },
    tolerances: {
      joint_position_tol_rad: 0.05,
      translation_tol_m: settings.cartesianTranslationToleranceM,
      rotation_tol_rad: settings.cartesianRotationToleranceRad,
    },
    speed_profile: {
      velocity_scale: settings.velocityScale,
      acceleration_scale: settings.accelerationScale,
      max_tcp_linear_speed_mps: settings.tcpLinearSpeedMps,
    },
    timeouts: {
      planning_timeout_s: 2,
      execution_timeout_s: 10,
    },
    cache_mode: 'default',
    target_mode: 'absolute',
    target_reference: null,
    offset_reference: null,
    motion,
    contact_policy_id: null,
    per_joint_dynamics_limits: jointLimits,
    motion_interrupt_scopes: [],
    runtime_object_bindings: [],
    // Sent only when the operator set one; absent keeps the Deployment's.
    ...(settings.clearanceM === null ? {} : { clearance_m: settings.clearanceM }),
  };
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: status.ownerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
    },
    program_id: requireSelectedMotion(status).motion.programId,
    program_revision: requireSelectedMotion(status).motion.programRevision,
    program_binding_id: requireSelectedMotion(status).motion.programBindingId,
    program_run_id: requireSelectedMotion(status).motion.programRunId,
    expected_graph_revision: status.graphRevision,
    expected_retained_bundle_id: status.retainedBundleId,
    expected_scene_revision: null,
    participants: [{
      target: { component_id: selectedMotion.controllableComponentId, command_target_id: selectedMotion.commandTargetId },
      required_capability_id: requireSelectedMotion(status).motion.targetDescriptor.contracts.operationCapabilityId,
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
    joint_names: requireSelectedMotion(status).motion.targetDescriptor.joints.map(
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
  requireMotionStatus(status, 'planning.motion.cartesian_path');
  const currentTcpPose = requireSelectedMotion(status).observed.currentTcpPose;
  if (currentTcpPose === null) {
    throw new Error('The current TCP pose is unavailable.');
  }
  return buildMotionIntent(status, {
    kind: 'cartesian_path',
    waypoints: [
      copyPose(currentTcpPose),
      normalizedPose(pose),
    ],
    max_translation_step_m: 0.005,
    max_rotation_step_rad: 0.05,
    max_angular_speed_rad_s: settings.tcpAngularSpeedRadS,
    max_linear_acceleration_mps2: settings.tcpLinearAccelerationMps2,
    max_angular_acceleration_rad_s2: settings.tcpAngularAccelerationRadS2,
    fallback_policy: 'fail_closed',
    // Exact corners; timing follows the Deployment's cartesian_timing.
    blend_radii_m: [0, 0],
  }, requestId, settings);
}

/** The strict current descriptor is a projection, never command authority. */
export function hasPneumaticGripperDescriptor(status: ExecutionOwnerStatus): boolean {
  const gripper = status.directControlGripper;
  if (gripper === null) return false;
  const descriptor = gripper.targetDescriptor;
  return descriptor.schemaVersion === 'manyforge.manual_control_gripper_descriptor.v5'
    && descriptor.commandKind === 'gripper_position'
    && descriptor.actuationKind === 'pneumatic'
    && descriptor.graspConfirmationPolicy === 'not_required'
    && ((descriptor.outputMode === 'commanded' && (
      (descriptor.pneumaticOutputs === undefined && descriptor.completionPolicy === 'output_delivery')
      || (descriptor.pneumaticOutputs !== undefined && descriptor.pneumaticOutputs.length > 0
        && descriptor.completionPolicy === 'no_output')))
      || (descriptor.outputMode === 'none' && descriptor.completionPolicy === 'no_output'))
    && descriptor.controllableComponentId === gripper.controllableComponentId
    && descriptor.commandTargetId === gripper.commandTargetId
    && descriptor.sourceCoordinateName === gripper.sourceCoordinateName
    && descriptor.sourceKind === gripper.sourceKind
    && descriptor.coordinateKind === gripper.coordinateKind;
}

export function buildGripperCommandIntent(
  status: ExecutionOwnerStatus,
  position: number,
  requestId: string,
): DirectControlOperationIntent {
  const gripper = status.directControlGripper;
  if (gripper === null) {
    throw new Error('The selected component has no gripper target.');
  }
  const descriptor = gripper.targetDescriptor;
  const pneumatic = hasPneumaticGripperDescriptor(status);
  if (
    !pneumatic
    && status.observedGripper === null
    && gripper.sourceKind !== 'component_local'
  ) {
    throw new Error('Authoritative gripper state is unavailable.');
  }
  if (!pneumatic && status.observedGripper !== null && !status.observedGripper.complete) {
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
  // The valve has only two positions. Preview and submit the selected endpoint,
  // so the logical gripper and the physical outputs describe the same state.
  const selectedPosition = descriptor.pneumaticOutputs === undefined
    ? position : manualPneumaticEndpoint(status, position).position;
  const coordinate = descriptor.coordinateKind === 'angular_rad'
    ? { kind: 'angular_rad' as const, value_rad: selectedPosition }
    : { kind: 'linear_m' as const, value_m: selectedPosition };
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
    expected_scene_revision: null,
    participants: [{
      target: { component_id: descriptor.controllableComponentId, command_target_id: descriptor.commandTargetId },
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
    | 'moveToPath'
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
        : state.moveToPath === 'linear'
          ? buildCartesianMotionIntent(state.status, state.draftPose, requestId, state.motionSettings)
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
  const sinrCosp = 2 * (w * x + y * z);
  const cosrCosp = 1 - 2 * (x * x + y * y);
  let roll = Math.atan2(sinrCosp, cosrCosp);
  const pitchInput = Math.max(-1, Math.min(1, 2 * (w * y - z * x)));
  const pitch = Math.asin(pitchInput);
  let yaw = Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
  // Gimbal lock (cos pitch ~ 0): both atan2 pairs are rounding noise. Yaw is
  // 0 there and roll carries the whole turn, read from R01 and R11.
  if (Math.hypot(sinrCosp, cosrCosp) < 1e-9) {
    roll = Math.atan2(Math.sign(pitchInput) * 2 * (x * y - w * z), 1 - 2 * (x * x + z * z));
    yaw = 0;
  }
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
    === 'manyforge.direct_control_capability_command_preview.v2';
}
