import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';
import { api, ApiError, apiErrorCode } from '../api/client';
import { CommandRefusalError, commandRefusalCause } from '../state/commandRefusal';
import { parseRuntimeScaleControlAccepted } from '../api/parsers';
import {
  buildPreviewIntent,
  buildJointMotionIntent,
  buildGripperCommandIntent,
  hasPneumaticGripperDescriptor,
  gripperTargetsForStatus,
  initialRobotControlState,
  operationStateIsTerminal,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  robotControlHasActiveOperation,
  robotControlHasLocalDraft,
  robotControlPreviewApprovable,
  robotControlReducer,
  selectedStatusForState,
  motionTargetsForStatus,
} from '../state/robotControl';
import type { ComposerHeadlessState } from '../types/headless';
import type { ProgramCycleStatus, StructuredError } from '../types/api';
import { CommandDeliveryUnconfirmedError, type HeadlessStateSource } from './useHeadlessState';
import {
  cellRecoverMessage,
  useComponentIncidents,
} from './useComponentIncidents';
import type { OpenComponentIncident } from './useComponentIncidents';
import type {
  ManualCartesianDefaults,
  ManualMotionSettings,
  RobotControlState,
} from '../state/robotControl';
import type { PoseDragDiscard, RobotControlMode } from '../state/robotControl';
import { acknowledgedUncertainOutputs, type UncertainOutput } from '../utils/uncertainOutputs';
import { tcpEntryIdentityKey } from '../types/robotControl';
import { writeManualPneumaticValves } from '../state/pneumaticManual';
import type { CartesianMoveDraftOptions, MoveToDraftSource } from '../utils/moveTo';
import type {
  CellRecoverResult,
  DirectControlOperationIntent,
  DirectControlSetTcpIntent,
  DirectControlGripperStatus,
  DirectControlMotionStatus,
  ExecutionOwnerStatus,
  ManualControlAvailability,
  Pose3Value,
  RuntimeScaleControlAccepted,
  RuntimeScaleControlStatus,
  TcpEntryIdentity,
} from '../types/robotControl';

// Recover cell keeps its press alive across HTTP timeouts by replaying its
// request ID; a replay answering `cell_recover_in_progress` is asked again
// after this pause.
export const CELL_RECOVER_REPLAY_PAUSE_MS = 1000;

/** Pair a robot with the component selected at its flange, including TCP-less tools. */
export function mountedToolsFromStatus(
  status: ExecutionOwnerStatus | null,
): Record<string, string> {
  const mounts: Record<string, string> = {};
  if (status === null) return mounts;
  for (const robot of status.directControlTargets) {
    const endEffectorComponentId = robot.targetDescriptor.endEffectorComponentId;
    if (endEffectorComponentId !== null
      && endEffectorComponentId !== robot.controllableComponentId) {
      mounts[robot.controllableComponentId] = endEffectorComponentId;
    }
  }
  return mounts;
}

type TcpFence = {
  componentId: string;
  targetId: string;
  graphRevision: string;
  retainedBundleId: string;
  value: NonNullable<DirectControlMotionStatus['activeTcp']>;
  strict: boolean;
  adopt: boolean;
};

function tcpAtLeast(current: DirectControlMotionStatus['activeTcp'], fence: TcpFence): boolean {
  if (current === null || current.ownerEpoch !== fence.value.ownerEpoch) return false;
  if (current.registrationGeneration !== fence.value.registrationGeneration)
    return current.registrationGeneration > fence.value.registrationGeneration;
  if (current.sequence !== fence.value.sequence) return current.sequence > fence.value.sequence;
  if (fence.strict) return false;
  return current.entry === null ? fence.value.entry === null
    : fence.value.entry !== null && tcpEntryIdentityKey(current.entry) === tcpEntryIdentityKey(fence.value.entry);
}

function setTcpReceiptFence(result: unknown, status: ExecutionOwnerStatus,
  motion: DirectControlMotionStatus, requestId: string, admittedFailure = false): TcpFence {
  if (result === null || typeof result !== 'object' || Array.isArray(result))
    throw new Error('Set TCP returned no confirmed Active TCP.');
  const envelope = result as Record<string, unknown>;
  const accepted = envelope.accepted;
  if ((!admittedFailure && (envelope.ok !== true || envelope.command !== 'submit'
    || accepted === null || typeof accepted !== 'object' || Array.isArray(accepted)
    || (accepted as Record<string, unknown>).request_id !== requestId
    || typeof (accepted as Record<string, unknown>).operation_id !== 'string'
    || !(accepted as Record<string, unknown>).operation_id)) || !Array.isArray(envelope.active_tcp))
    throw new Error('Set TCP returned an invalid acceptance.');
  const rows = envelope.active_tcp.filter((item: unknown) => item !== null && typeof item === 'object'
    && !Array.isArray(item) && (item as Record<string, unknown>).component_id === motion.controllableComponentId);
  if (rows.length !== 1) throw new Error('Set TCP returned no unique robot Active TCP.');
  const row = rows[0] as Record<string, unknown>;
  const entry = row.entry;
  if (row.owner_epoch !== status.ownerEpoch || !Number.isSafeInteger(row.registration_generation)
    || (row.registration_generation as number) < 1 || !Number.isSafeInteger(row.sequence)
    || (row.sequence as number) < 0 || (entry !== null && (typeof entry !== 'object' || Array.isArray(entry))))
    throw new Error('Set TCP returned an invalid Active TCP identity.');
  let selected: TcpEntryIdentity | null = null;
  let display: string | null = null;
  if (entry !== null) {
    const wire = entry as Record<string, unknown>;
    if (typeof wire.component_id !== 'string' || typeof wire.target_id !== 'string'
      || typeof wire.frame_role_id !== 'string') throw new Error('Set TCP returned an invalid TCP entry.');
    selected = { componentId: wire.component_id, targetId: wire.target_id, frameRoleId: wire.frame_role_id };
    const descriptor = motion.targetDescriptor.tcps.find((tcp) => tcpEntryIdentityKey(tcp.entry) === tcpEntryIdentityKey(selected!));
    if (descriptor === undefined) throw new Error("Set TCP returned a TCP outside this robot's list.");
    display = descriptor.qualifiedName;
  }
  return { componentId: motion.controllableComponentId, targetId: motion.commandTargetId,
    graphRevision: status.graphRevision, retainedBundleId: status.retainedBundleId,
    strict: false, adopt: true, value: { entry: selected, display, ownerEpoch: status.ownerEpoch,
      registrationGeneration: row.registration_generation as number, sequence: row.sequence as number } };
}

function requestId(): string {
  return globalThis.crypto?.randomUUID?.()
    ?? `composer-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message.trim()
    ? error.message
    : fallback;
}

export function runtimeScaleSelectorIdentity(selector: RuntimeScaleControlStatus['selector']): string {
  const compareTuples = (left: unknown[], right: unknown[]): number => {
    const leftJson = JSON.stringify(left);
    const rightJson = JSON.stringify(right);
    return leftJson < rightJson ? -1 : leftJson > rightJson ? 1 : 0;
  };
  return selector.kind === 'singular'
    ? JSON.stringify(['singular', selector.programBindingId, selector.componentRoleId, selector.targetRoleId])
    : JSON.stringify(['group', selector.programBindingId, ...selector.groupTargets
      .map((target) => [target.componentRoleId, target.targetRoleId])
      .sort(compareTuples)]);
}

/** Expected draft rejection is panel feedback, not an execution incident. */
export function previewTargetRejection(error: unknown): string | null {
  if (!(error instanceof ApiError)) return null;
  return motionPreviewRejectionMessage(error.structuredError);
}

/** The plain meaning of a native motion-preview rejection, if it is a known target result. */
export function motionPreviewRejectionMessage(
  detail: Pick<StructuredError, 'code' | 'details'> | null,
): string | null {
  // The current proxy also uses HTTP 500 for these normal negative results.
  // Match the owner's exact result, never its broad code or HTTP status alone:
  // IK/planner transport failures use those same broad codes.
  if (detail?.code === 'ik_failed' && detail.details?.reason === 'native_ik_no_solution') {
    return 'No collision-free pose found. The target may be out of reach, outside joint limits, or in collision. Change its position or orientation.';
  }
  if (detail?.code === 'planning_failed' && detail.details?.reason === 'native_goal_state_collision') {
    return 'The robot would collide at the target. Change its position or orientation.';
  }
  if (detail?.code === 'planning_failed' && detail.details?.reason === 'native_start_state_collision') {
    return 'The current robot pose is in collision. Planning cannot start from this pose.';
  }
  if (detail?.code === 'planning_failed' && detail.details?.reason === 'native_rrt_failed') {
    return 'No valid path found for this target. Try another position or orientation.';
  }
  return null;
}

const RUNTIME_SCALE_SEND_INTERVAL_MS = 250;

export function runtimeScaleFlushDelay(
  nowMs: number,
  lastSentAtMs: number,
  hasQueuedValue: boolean,
  inFlight: boolean,
): number | null {
  if (!hasQueuedValue || inFlight) return null;
  return Math.max(0, RUNTIME_SCALE_SEND_INTERVAL_MS - (nowMs - lastSentAtMs));
}

export function runtimeScaleControlScopeFence(
  ownerEpoch: string,
  status: RuntimeScaleControlStatus,
): string {
  const memberScope = status.selector.kind === 'group'
    ? (status.groupMembers ?? []).map((member) => [
      member.componentRoleId,
      member.targetRoleId,
      member.controlDomainId,
      member.controlDomainRevision,
      member.controlBindingId,
      member.controlBindingRevision,
      member.scalingDomainId,
      member.activeControlConfigurationId,
      member.activeControlConfigurationGeneration,
    ]).sort((left, right) => {
      const leftJson = JSON.stringify(left);
      const rightJson = JSON.stringify(right);
      return leftJson < rightJson ? -1 : leftJson > rightJson ? 1 : 0;
    })
    : undefined;
  return JSON.stringify([
    ownerEpoch,
    status.selector.programBindingId,
    runtimeScaleSelectorIdentity(status.selector),
    status.controlDomainId ?? 'no-control-domain',
    status.controlDomainRevision ?? 'no-control-domain-revision',
    status.controlBindingId ?? 'no-control-binding',
    status.controlBindingRevision ?? 'no-control-binding-revision',
    status.scalingDomainId ?? 'no-scaling-domain',
    memberScope,
  ]);
}

/**
 * Owner projections can arrive after a command response.  Preserve the
 * highest accepted owner generation so an older projection cannot make the
 * slider jump backwards; an equal/newer projection remains authoritative for
 * effective/application state.
 */
export function mergeRuntimeScaleStatus(
  current: RuntimeScaleControlStatus | null,
  projected: RuntimeScaleControlStatus,
): RuntimeScaleControlStatus {
  return current !== null
    && current.controlGeneration > projected.controlGeneration
    ? current
    : projected;
}

export function acceptedRuntimeScaleStatus(
  submitted: RuntimeScaleControlStatus,
  accepted: RuntimeScaleControlAccepted,
  expectedRequestId: string,
  expectedScale: number,
): RuntimeScaleControlStatus {
  if (
    accepted.requestId !== expectedRequestId
    || accepted.controlGeneration !== submitted.controlGeneration + 1
    || accepted.requestedScale !== expectedScale
    || runtimeScaleSelectorIdentity(accepted.selector)
      !== runtimeScaleSelectorIdentity(submitted.selector)
  ) {
    throw new Error('Runtime velocity acceptance does not match its submitted command.');
  }
  return {
    ...submitted,
    controlGeneration: accepted.controlGeneration,
    requestedScale: accepted.requestedScale,
    activeOperationId: accepted.activeOperationId,
    activeOperationRevision: accepted.activeOperationRevision,
    activeOperationScaleGeneration: accepted.activeOperationScaleGeneration,
  };
}

/** Builds only an advisory query; it never selects or initializes a command draft. */
export function buildManualAvailabilityIntent(
  status: ExecutionOwnerStatus,
  mode: RobotControlMode,
  draftGripperPosition: number | null,
  requestIdentity: string,
): DirectControlOperationIntent {
  if (mode !== 'gripper') {
    if (status.observedMotion === null) throw new Error('Motion observation is unavailable.');
    return buildJointMotionIntent(status, status.observedMotion.positionsRad, requestIdentity);
  }
  if (!hasPneumaticGripperDescriptor(status)) {
    if (status.observedGripper === null) throw new Error('Gripper observation is unavailable.');
    return buildGripperCommandIntent(status, status.observedGripper.position, requestIdentity);
  }
  if (draftGripperPosition !== null) {
    try {
      return buildGripperCommandIntent(status, draftGripperPosition, requestIdentity);
    } catch {
      // An invalid draft is never repaired. Only the query may use its configured preset.
    }
  }
  const descriptor = status.directControlGripper!.targetDescriptor;
  const presets = descriptor.namedPresets.filter((preset) => preset.name === descriptor.openStateRef);
  if (descriptor.openStateRef === null || presets.length !== 1) {
    throw new Error('The configured gripper open preset is unavailable or ambiguous.');
  }
  return buildGripperCommandIntent(status, presets[0].masterPosition, requestIdentity);
}

/** Cold ownership/hold revisions only; never pose samples or node tick status. */
export function manualAvailabilityScopeFence(
  snapshot: ComposerHeadlessState | null,
  mode: RobotControlMode,
  selection?: Pick<RobotControlState, 'selectedControllableComponentId' | 'selectedMotionTargetId' | 'selectedGripperTargetId'>
    & Partial<Pick<RobotControlState, 'mountedToolByRobot'>>,
): string | null {
  const status = snapshot?.executionControlStatus;
  if (status == null || snapshot == null) return null;
  const selected = selection === undefined
    ? status
    : selectedStatusForState({ status, ...selection });
  if (selected === null) return null;
  const cycle = snapshot.programCycle;
  const target = mode === 'gripper' ? selected.directControlGripper : selected.directControl;
  if (target === null) return null;
  return JSON.stringify([
    status.ownerEpoch, status.retainedBundleId, status.graphRevision,
    target.programBindingId, target.programRunId,
    ...(mode === 'gripper' && hasPneumaticGripperDescriptor(selected) ? [target.targetDescriptor] : []),
    snapshot.authoringAdmission.admissionGeneration,
    cycle.programRunnerOwnerEpoch, cycle.cycleGeneration, cycle.state,
    cycle.executionControlRevision,
    cycle.runtimeControls?.executionSelections,
    status.runtimeScaleControl?.activeControlConfigurationGeneration,
    status.manualOperations.map((operation) => [operation.operationId, operation.operationRevision, operation.resident]),
  ]);
}

export interface RobotControlController {
  state: RobotControlState;
  /** Current owner projection, unlike state.status retained for presentation. */
  currentStatus: ExecutionOwnerStatus | null;
  /** Fresh owner rows for the Resume comparison, including a confirmed Set TCP receipt. */
  tcpResumeStatus?: ExecutionOwnerStatus | null;
  tcpResumeFresh?: boolean;
  tcpResumeConnectionLost?: boolean;
  /** Program cycle from that same current Headless snapshot. */
  tcpResumeCycle?: ProgramCycleStatus | null;
  /** Selected addressed robot/tool projection used by all direct-control UI. */
  selectedStatus: ExecutionOwnerStatus | null;
  motionTargets: DirectControlMotionStatus[];
  gripperTargets: DirectControlGripperStatus[];
  manualControlAllowed: boolean;
  manualControlUnavailableReason: string | null;
  unappliedChanges?: boolean;
  controlSource?: HeadlessStateSource | null;
  setManualPanelVisible: (visible: boolean) => void;
  controlPending: 'stop' | 'reset' | null;
  runtimeScaleStatus: RuntimeScaleControlStatus | null;
  runtimeScaleDraft: number;
  runtimeScalePending: boolean;
  runtimeScaleError: string | null;
  stopFaultRecoveryPending: boolean;
  /** The press outlived one HTTP call; Headless is still running it. */
  stopFaultRecoveryInProgress: boolean;
  /** SX-679: the operator's abort of the running press was sent, not yet answered. */
  stopFaultRecoveryAbortPending: boolean;
  /** SX-679/684: why Recover cell is unavailable now (Scenario Close runs), or null. */
  cellRecoverBlockedReason: string | null;
  setCellRecoverBlockedReason: (reason: string | null) => void;
  stopFaultRecoveryError: string | null;
  /** Open component incidents (CCLC snapshots) while the cell is latched. */
  componentIncidents: OpenComponentIncident[];
  componentIncidentsUnavailableReason: string | null;
  selectRobot: (controllableComponentId: string, motionTargetId: string) => void;
  selectTool: (gripperTargetId: string) => void;
  /** TCP-C: move and show the selected robot at another of its TCPs. */
  selectTcp: (entryKey: string) => void;
  tcpApplying: boolean;
  tcpError: string | null;
  selectMode: (mode: RobotControlMode) => void;
  selectMoveToSource: (source: MoveToDraftSource) => void;
  editMoveToOptions: (update: Partial<CartesianMoveDraftOptions>) => void;
  beginPoseDrag: () => boolean;
  finishPoseDrag: (pose: Pose3Value | null, discard?: PoseDragDiscard) => void;
  authoringPending: boolean;
  reportAuthoringPending: (label: string, pending: boolean) => void;
  editMoveToJoints: (positionsRad: number[]) => void;
  editJoint: (jointIndex: number, positionRad: number) => void;
  editPose: (pose: Pose3Value) => void;
  editGripper: (position: number) => void;
  editMotionSettings: (update: Partial<ManualMotionSettings>) => void;
  approve: () => Promise<void>;
  revert: () => void;
  stop: () => Promise<void>;
  recoverStopFault: () => Promise<void>;
  /** SX-679: abort the running Recover cell; its result then lists what was done. */
  abortRecoverStopFault: () => Promise<void>;
  dismissRuntimeScaleError: () => void;
  dismissError: () => void;
  /** Reset the current terminal operation; approved uncertain outputs of it are acknowledged (contract 66). */
  reset: (approvedUncertainOutputs?: readonly UncertainOutput[]) => Promise<void>;
  /**
   * One targeted operation Reset per approved acceptance-unknown output's
   * operation, each acknowledging only its own outputs. Null when every
   * Reset was received, else why; stops at the first refusal.
   */
  resetUncertainOutputs: (approved: readonly UncertainOutput[]) => Promise<string | null>;
  setRuntimeScale: (scale: number) => void;
  commitRuntimeScale: () => void;
  retry: () => void;
}

export function useRobotControl(
  enabled: boolean,
  observeProgramRun = false,
  programStopActive = false,
  headlessSnapshot: ComposerHeadlessState | null = null,
  controlSource: HeadlessStateSource | null = null,
  cartesianDefaults: ManualCartesianDefaults | null = null,
  unappliedChanges = false,
): RobotControlController {
  const [state, dispatch] = useReducer(
    robotControlReducer,
    initialRobotControlState,
  );
  const [controlPending, setControlPending] = useState<
    'stop' | 'reset' | null
  >(null);
  const [tcpApplying, setTcpApplying] = useState(false);
  const [tcpError, setTcpError] = useState<string | null>(null);
  const [tcpFence, setTcpFence] = useState<TcpFence | null>(null);
  const tcpInFlight = useRef<{ id: string; ownerEpoch: string } | null>(null);
  const [tcpWaitOwner, setTcpWaitOwner] = useState<{ ownerEpoch: string; graphRevision: string; retainedBundleId: string;
    componentId: string; targetId: string } | null>(null);
  const [runtimeScaleStatus, setRuntimeScaleStatus] = useState<RuntimeScaleControlStatus | null>(null);
  const [runtimeScaleDraft, setRuntimeScaleDraft] = useState(1);
  const [runtimeScalePending, setRuntimeScalePending] = useState(false);
  const [runtimeScaleError, setRuntimeScaleError] = useState<string | null>(null);
  const [stopFaultRecoveryPending, setStopFaultRecoveryPending] = useState(false);
  const [stopFaultRecoveryInProgress, setStopFaultRecoveryInProgress] = useState(false);
  const [stopFaultRecoveryAbortPending, setStopFaultRecoveryAbortPending] = useState(false);
  const [cellRecoverBlockedReason, setCellRecoverBlockedReason] = useState<string | null>(null);
  const [stopFaultRecoveryError, setStopFaultRecoveryError] = useState<string | null>(null);
  const stopFaultRecoveryRequest = useRef<{ abort: AbortController } | null>(null);
  const approveInFlight = useRef<{ ownerEpoch: string; generation: number } | null>(null);
  const pneumaticWriteAbort = useRef<AbortController | null>(null);
  useEffect(() => () => pneumaticWriteAbort.current?.abort(), []);
  // Leaving ends the wait for a Recover cell press (Headless keeps running it).
  useEffect(() => () => {
    stopFaultRecoveryRequest.current?.abort.abort();
    stopFaultRecoveryRequest.current = null;
  }, []);
  const dragGeneration = useRef<number | null>(null);
  useLayoutEffect(() => {
    if (!state.poseDragging) dragGeneration.current = null;
  }, [state.poseDragging, state.generation]);
  const pendingAuthoringInputs = useRef(new Set<string>());
  const [authoringPending, setAuthoringPending] = useState(false);
  const reportAuthoringPending = useCallback((label: string, pending: boolean) => {
    if (pending) pendingAuthoringInputs.current.add(label);
    else pendingAuthoringInputs.current.delete(label);
    setAuthoringPending(pendingAuthoringInputs.current.size > 0);
  }, []);
  const pendingOperationControl = useRef<{
    control: 'stop' | 'reset'; ownerEpoch: string; operationId: string;
  } | null>(null);
  const runtimeScaleStatusRef = useRef<RuntimeScaleControlStatus | null>(null);
  const runtimeScaleQueued = useRef<number | null>(null);
  const runtimeScaleInFlight = useRef<{ scopeFence: string } | null>(null);
  const runtimeScaleTimer = useRef<number | null>(null);
  const runtimeScaleLastSentAt = useRef(Number.NEGATIVE_INFINITY);
  const runtimeScaleFlush = useRef<() => void>(() => undefined);
  const runtimeScaleStopActive = useRef(programStopActive);
  runtimeScaleStopActive.current = programStopActive;
  const [availability, setAvailability] = useState<{
    scope: string; result: ManualControlAvailability | null; error: string | null;
  } | null>(null);
  const [manualPanelVisible, setManualPanelVisible] = useState(false);
  const observeManualAvailability = manualPanelVisible
    || robotControlHasLocalDraft(state) || state.operation !== null;
  const [availabilityRefresh, refreshAvailability] = useReducer((value: number) => value + 1, 0);
  const [cleanupFailure, setCleanupFailure] = useState<string | null>(null);
  const cleanupInFlight = useRef<Promise<void> | null>(null);
  const latestSnapshot = useRef(headlessSnapshot);
  latestSnapshot.current = headlessSnapshot;
  const ownerStatus = state.status;
  const selectedControllableComponentId = state.selectedControllableComponentId;
  const selectedMotionTargetId = state.selectedMotionTargetId;
  const selectedGripperTargetId = state.selectedGripperTargetId;
  const mountedToolByRobot = state.mountedToolByRobot;
  const ownerSelectedStatus = useMemo(
    () => selectedStatusForState({
      status: ownerStatus,
      selectedControllableComponentId,
      selectedMotionTargetId,
      selectedGripperTargetId,
      mountedToolByRobot,
    }),
    [ownerStatus, selectedControllableComponentId, selectedMotionTargetId,
      selectedGripperTargetId, mountedToolByRobot],
  );
  const selectedStatus = useMemo(() => {
    const motion = ownerSelectedStatus?.directControl;
    if (ownerSelectedStatus === null || motion == null || tcpFence === null
      || !tcpFence.adopt || ownerSelectedStatus.ownerEpoch !== tcpFence.value.ownerEpoch
      || ownerSelectedStatus.graphRevision !== tcpFence.graphRevision
      || ownerSelectedStatus.retainedBundleId !== tcpFence.retainedBundleId
      || motion.controllableComponentId !== tcpFence.componentId || motion.commandTargetId !== tcpFence.targetId
      || tcpAtLeast(motion.activeTcp, tcpFence)) return ownerSelectedStatus;
    const entry = tcpFence.value.entry;
    const row = entry === null ? null : motion.targetDescriptor.tcps.find((tcp) => (
      tcpEntryIdentityKey(tcp.entry) === tcpEntryIdentityKey(entry)
    ));
    if (row === undefined) return ownerSelectedStatus;
    const observed = ownerSelectedStatus.observedMotion;
    return { ...ownerSelectedStatus,
      directControl: { ...motion, activeTcp: tcpFence.value, tcpFrameRoleId: row?.frameRoleId ?? null,
        tcpTargetRoleId: row?.targetRoleId ?? null, selectedTcpEntryKey: row === null ? undefined : tcpEntryIdentityKey(row.entry) },
      observedMotion: observed === null ? null : { ...observed,
        currentTcpPose: row === null ? null : observed.tcpPoses[tcpEntryIdentityKey(row.entry)] ?? null },
    };
  }, [ownerSelectedStatus, tcpFence]);

  // Presentation retains the last status on disconnect. Resume comparison uses
  // only the current shell projection and the already-confirmed TCP receipt.
  const shell = controlSource?.getShellSnapshot?.();
  const tcpResumeConnectionLost = shell == null || shell.transportStatus === 'connecting'
    || shell.transportStatus === 'disconnected';
  const tcpResumeFresh = shell?.transportStatus === 'connected' && shell.semanticStatus === 'current'
    && shell.transportError === null && shell.snapshot === headlessSnapshot;
  const freshStatus = tcpResumeFresh ? headlessSnapshot?.executionControlStatus ?? null : null;
  const tcpResumeCycle = tcpResumeFresh ? headlessSnapshot?.programCycle ?? null : null;
  const tcpResumeStatus = freshStatus === null || !tcpApplying || tcpWaitOwner === null
    || freshStatus.ownerEpoch !== tcpWaitOwner.ownerEpoch
    || freshStatus.graphRevision !== tcpWaitOwner.graphRevision
    || freshStatus.retainedBundleId !== tcpWaitOwner.retainedBundleId
    ? freshStatus : {
      ...freshStatus,
      directControlTargets: freshStatus.directControlTargets.map((row) => {
        if (row.controllableComponentId !== tcpWaitOwner.componentId || row.commandTargetId !== tcpWaitOwner.targetId) return row;
        if (tcpFence === null || !tcpFence.adopt
          || tcpFence.componentId !== tcpWaitOwner.componentId || tcpFence.targetId !== tcpWaitOwner.targetId
          || tcpFence.value.ownerEpoch !== freshStatus.ownerEpoch
          || tcpFence.graphRevision !== freshStatus.graphRevision || tcpFence.retainedBundleId !== freshStatus.retainedBundleId)
          return { ...row, activeTcp: null };
        return tcpAtLeast(row.activeTcp, tcpFence) ? row : { ...row, activeTcp: tcpFence.value };
      }),
    };

  useEffect(() => {
    if (tcpFence === null) return;
    // Wait for the reducer's own projection, not merely a newer transport
    // snapshot that has not yet replaced the drafts and pose it presents.
    const live = state.status;
    if (live == null) return;
    if (live.ownerEpoch !== tcpFence.value.ownerEpoch || live.graphRevision !== tcpFence.graphRevision
      || live.retainedBundleId !== tcpFence.retainedBundleId) {
      setTcpFence(null);
      setTcpApplying(false);
      setTcpWaitOwner(null);
      return;
    }
    const current = live.directControlTargets.find((row) => row.controllableComponentId === tcpFence.componentId
      && row.commandTargetId === tcpFence.targetId);
    if (current !== undefined && tcpAtLeast(current.activeTcp, tcpFence)) {
      setTcpFence(null);
      setTcpApplying(false);
      setTcpWaitOwner(null);
    }
  }, [tcpFence, state.status]);

  useEffect(() => {
    const live = headlessSnapshot?.executionControlStatus;
    if (tcpWaitOwner !== null && live != null && (live.ownerEpoch !== tcpWaitOwner.ownerEpoch
      || live.graphRevision !== tcpWaitOwner.graphRevision || live.retainedBundleId !== tcpWaitOwner.retainedBundleId)) {
      tcpInFlight.current = null;
      setTcpFence(null);
      setTcpApplying(false);
      setTcpWaitOwner(null);
      setTcpError(null);
    }
  }, [tcpWaitOwner, headlessSnapshot?.executionControlStatus]);
  const motionTargets = useMemo(
    () => ownerStatus === null ? [] : motionTargetsForStatus(ownerStatus),
    [ownerStatus],
  );
  const gripperTargets = useMemo(
    () => ownerStatus === null ? [] : gripperTargetsForStatus(ownerStatus),
    [ownerStatus],
  );
  const availabilityScope = observeProgramRun
    ? manualAvailabilityScopeFence(headlessSnapshot, state.mode, state)
    : null;
  const currentAvailability = availability?.scope === availabilityScope ? availability.result : null;
  const mappedPneumatic = state.mode === 'gripper'
    && selectedStatus?.directControlGripper?.targetDescriptor.pneumaticOutputs !== undefined;
  const pneumaticNeedsIdle = mappedPneumatic
    && (observeProgramRun || headlessSnapshot?.programCycle.state !== 'idle_after_reset');
  const manualControlAllowed = !pneumaticNeedsIdle
    && (!observeProgramRun || (currentAvailability?.available === true && cleanupFailure === null));
  const manualControlUnavailableReason = manualControlAllowed ? null : pneumaticNeedsIdle
    ? 'Mapped pneumatic manual control is available when the Program is idle after Reset.'
    : cleanupFailure
    ?? (availability?.scope === availabilityScope ? availability.error : null)
    ?? currentAvailability?.unavailableReason?.replaceAll('_', ' ')
    ?? 'Checking backend manual-control availability.';
  const manualControlBlocked = useRef(!manualControlAllowed);
  manualControlBlocked.current = !manualControlAllowed;
  const cartesianMode = state.mode === 'pose_goal' || state.mode === 'cartesian_path'
    || (state.mode === 'move_to' && state.moveToKind === 'pose');
  const tcpBlocksCartesian = tcpApplying && cartesianMode;
  const projectedOwnerRef = useRef<string | null>(null);
  const previewStatusRef = useRef(selectedStatus);
  previewStatusRef.current = selectedStatus;
  const availabilityStatusRef = useRef(selectedStatus);
  const currentAvailabilityStatus = headlessSnapshot?.executionControlStatus == null
    ? null
    : selectedStatusForState({
      status: headlessSnapshot.executionControlStatus,
      selectedControllableComponentId, selectedMotionTargetId, selectedGripperTargetId,
      mountedToolByRobot,
    });
  // A pneumatic transition must query the same snapshot as its scope fence.
  // Missing/invalid current descriptors must never fall back to the old one.
  const pneumaticAvailabilityTransition = state.mode === 'gripper'
    && ((selectedStatus !== null && hasPneumaticGripperDescriptor(selectedStatus))
      || (currentAvailabilityStatus !== null && hasPneumaticGripperDescriptor(currentAvailabilityStatus)));
  availabilityStatusRef.current = pneumaticAvailabilityTransition
    ? currentAvailabilityStatus : selectedStatus;

  useEffect(() => {
    const current = headlessSnapshot?.executionControlStatus;
    const mounts = enabled ? mountedToolsFromStatus(current ?? null) : {};
    dispatch({ type: 'mounted_tools_changed', mounts,
      graphRevision: current?.graphRevision, retainedBundleId: current?.retainedBundleId });
  }, [enabled, headlessSnapshot?.executionControlStatus, state.status, state.phase, state.operation]);

  useEffect(() => {
    // An old owner's response cannot block or reject a successor's draft.
    cleanupInFlight.current = null;
    setCleanupFailure(null);
  }, [headlessSnapshot?.executionControlStatus?.ownerEpoch,
    headlessSnapshot?.programCycle?.programRunnerOwnerEpoch,
    headlessSnapshot?.programCycle?.programRunId]);

  const availabilityGripperDraft = selectedStatus !== null
    && state.mode === 'gripper' && hasPneumaticGripperDescriptor(selectedStatus)
    ? state.draftGripperPosition : null;
  useEffect(() => {
    if (!enabled || !observeProgramRun || !observeManualAvailability || availabilityScope === null) return;
    const status = availabilityStatusRef.current;
    if (status == null) return;
    const controller = new AbortController();
    let current = true;
    const read = async () => {
      try {
        const intent = buildManualAvailabilityIntent(
          status, state.mode, availabilityGripperDraft, requestId(),
        );
        const result = await api.execution.manualAvailability(intent, controller.signal);
        if (current) setAvailability({ scope: availabilityScope, result, error: null });
      } catch (error: unknown) {
        if (current) setAvailability({ scope: availabilityScope, result: null,
          error: errorMessage(error, 'Backend manual-control availability is unavailable.') });
      }
    };
    void read();
    return () => { current = false; controller.abort(); };
  }, [enabled, observeProgramRun, observeManualAvailability, availabilityScope, availabilityRefresh,
    state.mode, availabilityGripperDraft]);

  useEffect(() => {
    if (observeProgramRun) dispatch({ type: 'manual_scope_changed' });
  }, [observeProgramRun, availabilityScope]);

  useEffect(() => {
    if (!enabled) {
      projectedOwnerRef.current = null;
      dispatch({ type: 'clear' });
      return;
    }
    const status = headlessSnapshot?.executionControlStatus ?? null;
    if (status === null) {
      // Connection errors and command eligibility belong to the shared
      // transport surface. Retain the last coherent direct-control status;
      // transport absence is not evidence that a robot command failed.
      return;
    }
    const initialize = projectedOwnerRef.current === null;
    projectedOwnerRef.current = status.ownerEpoch;
    dispatch({
      type: initialize ? 'status_resolved' : 'status_refreshed',
      status,
      cartesianDefaults,
    });
  }, [cartesianDefaults, enabled, headlessSnapshot?.executionControlStatus]);

  // Re-sent when an operation ends, so a default that changed during it
  // still reaches the manual settings before the next move.
  const operationActive = robotControlHasActiveOperation(state);
  useEffect(() => {
    if (cartesianDefaults === null || operationActive) return;
    dispatch({ type: 'program_cartesian_defaults_changed', defaults: cartesianDefaults });
  }, [cartesianDefaults, operationActive]);

  useEffect(() => {
    const previewStatus = previewStatusRef.current;
    if (
      !enabled
      || tcpBlocksCartesian
      || !manualControlAllowed
      || state.phase !== 'planning'
      || state.poseDragging
      || previewStatus === null
    ) return undefined;
    const generation = state.generation;
    const previewDraft = {
      status: previewStatus,
      mode: state.mode,
      moveToKind: state.moveToKind,
      draftPositionsRad: state.draftPositionsRad,
      draftPose: state.draftPose,
      draftGripperPosition: state.draftGripperPosition,
      motionSettings: state.motionSettings,
    };
    const controller = new AbortController();
    let current = true;
    const timer = window.setTimeout(() => {
      if (dragGeneration.current !== null) return;
      let intent: DirectControlOperationIntent;
      try {
        intent = buildPreviewIntent(previewDraft, requestId());
      } catch (error: unknown) {
        if (current) {
          dispatch({
            type: 'preview_rejected',
            generation,
            error: errorMessage(error, 'Could not form a motion preview.'),
          });
        }
        return;
      }
      const preparePreview = async () => {
        if (observeProgramRun) {
          const context = currentAvailability?.manualHandover;
          if (!currentAvailability?.available || context == null) {
            throw new Error('The backend has not made this held target available.');
          }
          intent = { ...intent, manual_handover: context };
          if (currentAvailability.cleanupRequired) {
            if (cleanupInFlight.current === null) {
              const cycle = latestSnapshot.current?.programCycle;
              if (controlSource === null || cycle?.programRunId !== context.program_run_id
                || cycle.runRevision == null) {
                throw new Error('The held Program changed before manual cleanup.');
              }
              const cleanupId = requestId();
              const pending = controlSource.sendExecutionCommand('program_manual_handover', {
                request_id: cleanupId, program_run_id: context.program_run_id,
                expected_owner_epoch: context.program_runner_owner_epoch,
                expected_run_revision: cycle.runRevision, operation: intent,
              }, cleanupId).then(() => undefined);
              cleanupInFlight.current = pending;
              void pending.then(() => {
                if (cleanupInFlight.current !== pending) return;
                setAvailability(null);
                refreshAvailability();
              }, (error: unknown) => {
                if (cleanupInFlight.current !== pending) return;
                // Never automatically retry an effect with an uncertain result.
                setCleanupFailure(errorMessage(error, 'Manual handover failed. Check Program recovery.'));
              }).finally(() => { if (cleanupInFlight.current === pending) cleanupInFlight.current = null; });
            }
            await cleanupInFlight.current;
            // Fresh owner availability wakes this same pending draft after
            // cleanup. It is not a second preview or an implicit dispatch.
            return null;
          }
        }
        if (!current) return null;
        dispatch({ type: 'preview_started', generation, intent });
        return intent.participants[0].payload.kind === 'motion'
          ? api.execution.previewMotion(intent, controller.signal)
          : api.execution.previewCapabilityCommand(intent, controller.signal);
      };
      void preparePreview()
        .then((receipt) => {
          if (current && receipt !== null) {
            dispatch({ type: 'preview_resolved', generation, receipt });
          }
        })
        .catch((error: unknown) => {
          if (current && !controller.signal.aborted) {
            dispatch({
              type: 'preview_rejected',
              generation,
              error: errorMessage(error, 'Motion preview failed.'),
              errorRefusal: commandRefusalCause(error),
              targetRejection: previewTargetRejection(error),
            });
          }
        });
    }, ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS);
    return () => {
      current = false;
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [
    enabled,
    tcpBlocksCartesian,
    observeProgramRun,
    manualControlAllowed,
    currentAvailability,
    controlSource,
    state.draftPositionsRad,
    state.draftPose,
    state.draftGripperPosition,
    state.motionSettings,
    state.generation,
    state.mode,
    state.moveToKind,
    state.phase,
    state.poseDragging,
    // The reducer advances generation when an owner/binding/scene fence changes.
    // Fresh observations alone must not restart a valid in-progress preview.
  ]);

  const selectMode = useCallback((mode: RobotControlMode) => {
    dispatch({ type: 'select_mode', mode });
  }, []);

  const selectRobot = useCallback((controllableComponentId: string, motionTargetId: string) => {
    dispatch({ type: 'select_robot', controllableComponentId, motionTargetId });
  }, []);

  const selectTool = useCallback((gripperTargetId: string) => {
    dispatch({ type: 'select_tool', gripperTargetId });
  }, []);

  const selectTcp = useCallback((entryKey: string) => {
    const status = selectedStatus;
    const motion = status?.directControl;
    const tcp = motion?.targetDescriptor.tcps.find((row) => tcpEntryIdentityKey(row.entry) === entryKey);
    if (operationActive || tcpInFlight.current !== null || tcpApplying || cleanupInFlight.current !== null || cleanupFailure !== null
      || !enabled || controlSource === null || status === null || motion == null || motion.activeTcp === null
      || tcp === undefined || (motion.activeTcp?.entry != null
        && tcpEntryIdentityKey(motion.activeTcp.entry) === entryKey)) return;
    const id = requestId();
    const intent: DirectControlSetTcpIntent = {
      request_context: { request_id: id, request_owner_epoch: status.ownerEpoch,
        source_label: 'composer_ui', correlation_id: null },
      program_id: motion.programId, program_revision: motion.programRevision,
      program_binding_id: motion.programBindingId, program_run_id: motion.programRunId,
      expected_graph_revision: status.graphRevision, expected_retained_bundle_id: status.retainedBundleId,
      expected_scene_revision: null, coordination_policy: 'independent',
      participants: [{ component_role_id: motion.componentRoleId, target_role_id: motion.targetRoleId,
        required_capability_id: motion.operationCapabilityId, action_span: null,
        payload: { kind: 'set_tcp', tcp: { kind: 'entry', entry: {
          component_id: tcp.entry.componentId, target_id: tcp.entry.targetId,
          frame_role_id: tcp.entry.frameRoleId,
        } } } }],
    };
    const flight = { id, ownerEpoch: status.ownerEpoch };
    tcpInFlight.current = flight;
    dispatch({ type: 'active_tcp_change_started' });
    setTcpApplying(true);
    setTcpWaitOwner({ ownerEpoch: status.ownerEpoch, graphRevision: status.graphRevision,
      retainedBundleId: status.retainedBundleId, componentId: motion.controllableComponentId,
      targetId: motion.commandTargetId });
    setTcpError(null);
    let waitingForStatus = false;
    let setTcpSubmitted = false;
    const fallbackFence: TcpFence = {
      componentId: motion.controllableComponentId, targetId: motion.commandTargetId,
      graphRevision: status.graphRevision, retainedBundleId: status.retainedBundleId,
      strict: true, adopt: false, value: motion.activeTcp,
    };
    void (async () => {
      if (observeProgramRun) {
        let availability = await api.execution.manualAvailability(intent);
        if (!availability.available || availability.manualHandover === null)
          throw new Error(availability.unavailableReason ?? 'The held target is not available for manual control.');
        if (availability.cleanupRequired) {
          if (cleanupInFlight.current !== null)
            throw new Error('Manual handover is already in progress.');
          const context = availability.manualHandover;
          const cycle = latestSnapshot.current?.programCycle;
          if (cycle?.programRunId !== context.program_run_id || cycle.runRevision == null)
            throw new Error('The held Program changed before manual cleanup.');
          const cleanupId = requestId();
          const cleanup = controlSource.sendExecutionCommand('program_manual_handover', {
            request_id: cleanupId, program_run_id: context.program_run_id,
            expected_owner_epoch: context.program_runner_owner_epoch,
            expected_run_revision: cycle.runRevision, operation: { ...intent, manual_handover: context },
          }, cleanupId).then(() => undefined);
          cleanupInFlight.current = cleanup;
          try {
            await cleanup;
          } catch (error: unknown) {
            setCleanupFailure(errorMessage(error, 'Manual handover failed. Check Program recovery.'));
            throw error;
          } finally {
            if (cleanupInFlight.current === cleanup) cleanupInFlight.current = null;
            setAvailability(null);
            refreshAvailability();
          }
          availability = await api.execution.manualAvailability(intent);
          if (!availability.available || availability.cleanupRequired || availability.manualHandover === null)
            throw new Error(availability.unavailableReason ?? 'The held target changed after manual cleanup.');
        }
        intent.manual_handover = availability.manualHandover;
      }
      const live = latestSnapshot.current?.executionControlStatus;
      if (live == null || live.ownerEpoch !== status.ownerEpoch
        || live.graphRevision !== status.graphRevision || live.retainedBundleId !== status.retainedBundleId)
        throw new Error('The robot control target changed before Set TCP.');
      const target = previewStatusRef.current?.directControl;
      if (target == null || target.activeTcp === null || target.programBindingId !== motion.programBindingId
        || target.controllableComponentId !== motion.controllableComponentId
        || target.commandTargetId !== motion.commandTargetId)
        throw new Error('The robot control target changed before Set TCP.');
      const payload: Record<string, unknown> = { ...intent };
      delete payload.request_context;
      setTcpSubmitted = true;
      const receipt = await controlSource.sendExecutionCommand('operation_start', payload, id);
      const afterReply = latestSnapshot.current?.executionControlStatus;
      if (tcpInFlight.current !== flight || afterReply == null || afterReply.ownerEpoch !== status.ownerEpoch
        || afterReply.graphRevision !== status.graphRevision || afterReply.retainedBundleId !== status.retainedBundleId) return;
      // Once a result is received, do not re-enable Cartesian work until the
      // corresponding owner status has caught up. If the reply is invalid,
      // require a version strictly newer than the pre-command status.
      waitingForStatus = true;
      setTcpFence(fallbackFence);
      const fence = setTcpReceiptFence(receipt.result, status, motion, id);
      setTcpFence(fence);
    })().catch((error: unknown) => {
      if (tcpInFlight.current === flight && setTcpSubmitted && error instanceof CommandDeliveryUnconfirmedError) {
        waitingForStatus = true;
        setTcpFence(fallbackFence);
      }
      const current = latestSnapshot.current?.executionControlStatus;
      if (tcpInFlight.current === flight && current?.ownerEpoch === status.ownerEpoch && current.graphRevision === status.graphRevision
        && current.retainedBundleId === status.retainedBundleId) {
        const raw = error instanceof CommandRefusalError ? error.executionResult : null;
        const result = raw !== null && typeof raw === 'object' && !Array.isArray(raw)
          ? raw as Record<string, unknown> : null;
        const ownerError = result?.error !== null && typeof result?.error === 'object' && !Array.isArray(result.error)
          ? result.error as Record<string, unknown> : null;
        const details = ownerError?.details !== null && typeof ownerError?.details === 'object' && !Array.isArray(ownerError.details)
          ? ownerError.details as Record<string, unknown> : null;
        if (result?.ok === false && details?.immediate_step === 'admitted') {
          waitingForStatus = true;
          try {
            setTcpFence(setTcpReceiptFence(raw, status, motion, id, true));
          } catch {
            setTcpFence(fallbackFence);
          }
        }
        setTcpError(errorMessage(error, 'Set TCP failed.'));
      }
    }).finally(() => {
      if (tcpInFlight.current === flight) {
        tcpInFlight.current = null;
        if (!waitingForStatus) {
          setTcpApplying(false);
          setTcpWaitOwner(null);
        }
      }
    });
  }, [selectedStatus, enabled, controlSource, observeProgramRun, cleanupFailure, tcpApplying, operationActive]);

  useEffect(() => {
    dispatch({ type: 'program_activity_changed' });
  }, [observeProgramRun]);

  useEffect(() => {
    const pending = pendingOperationControl.current;
    if (pending !== null && (pending.ownerEpoch !== state.status?.ownerEpoch
      || pending.operationId !== state.operation?.operationId)) {
      pendingOperationControl.current = null;
      setControlPending(null);
    }
  }, [state.status?.ownerEpoch, state.operation?.operationId]);

  useEffect(() => {
    const pending = approveInFlight.current;
    if (pending !== null && (pending.ownerEpoch !== state.status?.ownerEpoch
      || pending.generation !== state.generation || state.phase !== 'approving')) {
      approveInFlight.current = null;
    }
  }, [state.status?.ownerEpoch, state.generation, state.phase]);

  const sendOperationControl = useCallback(async (
    control: 'stop' | 'reset',
    kind: Parameters<HeadlessStateSource['sendExecutionCommand']>[0],
    payload: Record<string, unknown>,
  ) => {
    if (state.status === null || state.operation === null) return;
    const pending = { control, ownerEpoch: state.status.ownerEpoch, operationId: state.operation.operationId };
    pendingOperationControl.current = pending;
    try {
      if (controlSource === null) throw new Error('Control connection is not synchronized.');
      // Dispatch first. Local click feedback is not a command prerequisite.
      const response = controlSource.sendExecutionCommand(kind, payload, requestId());
      setControlPending(control);
      await response;
    } catch (error: unknown) {
      if (pendingOperationControl.current === pending) dispatch({
        type: 'operation_control_failed', ownerEpoch: pending.ownerEpoch, operationId: pending.operationId,
        error: errorMessage(error, `Operation ${control} was rejected.`),
        errorRefusal: commandRefusalCause(error),
      });
    } finally {
      if (pendingOperationControl.current === pending) {
        pendingOperationControl.current = null;
        setControlPending(null);
      }
    }
  }, [controlSource, state.operation, state.status]);

  const projectedRuntimeScale = state.status?.runtimeScaleControl ?? null;
  const projectedRuntimeScaleRef = useRef(projectedRuntimeScale);
  projectedRuntimeScaleRef.current = projectedRuntimeScale;
  const runtimeScaleScopeFence = enabled
    && state.status !== null
    && projectedRuntimeScale !== null
    ? runtimeScaleControlScopeFence(state.status.ownerEpoch, projectedRuntimeScale)
    : null;
  const runtimeScaleContext = useRef<{
    ownerEpoch: string;
    scopeFence: string;
    selector: RuntimeScaleControlStatus['selector'];
  } | null>(null);
  runtimeScaleContext.current = runtimeScaleScopeFence !== null
    && state.status !== null
    && projectedRuntimeScale !== null
    ? {
        ownerEpoch: state.status.ownerEpoch,
        scopeFence: runtimeScaleScopeFence,
        selector: projectedRuntimeScale.selector,
      }
    : null;

  const clearRuntimeScaleQueue = useCallback(() => {
    runtimeScaleQueued.current = null;
    if (runtimeScaleTimer.current !== null) {
      window.clearTimeout(runtimeScaleTimer.current);
      runtimeScaleTimer.current = null;
    }
    setRuntimeScalePending(false);
  }, []);

  const scheduleRuntimeScaleFlush = useCallback(() => {
    if (runtimeScaleTimer.current !== null) return;
    const delay = runtimeScaleFlushDelay(
      performance.now(),
      runtimeScaleLastSentAt.current,
      runtimeScaleQueued.current !== null,
      runtimeScaleInFlight.current !== null,
    );
    if (delay === null) return;
    if (delay === 0) {
      runtimeScaleFlush.current();
      return;
    }
    runtimeScaleTimer.current = window.setTimeout(() => {
      runtimeScaleTimer.current = null;
      runtimeScaleFlush.current();
    }, delay);
  }, []);

  const flushRuntimeScale = useCallback(async () => {
    const context = runtimeScaleContext.current;
    const observed = runtimeScaleStatusRef.current;
    const scale = runtimeScaleQueued.current;
    if (
      context === null
      || observed === null
      || runtimeScaleScopeFence !== context.scopeFence
      || !observed.supported
      || observed.frozen
      || runtimeScaleStopActive.current
      || scale === null
      || runtimeScaleInFlight.current
    ) return;
    runtimeScaleQueued.current = null;
    const pending = { scopeFence: context.scopeFence };
    runtimeScaleInFlight.current = pending;
    runtimeScaleLastSentAt.current = performance.now();
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      const commandRequestId = requestId();
      const receipt = await controlSource.sendExecutionCommand(
        'runtime_scale',
        {
          selector: observed.selector.kind === 'group' ? {
            program_binding_id: observed.selector.programBindingId,
            group_targets: observed.selector.groupTargets.map((target) => ({
              component_role_id: target.componentRoleId,
              target_role_id: target.targetRoleId,
            })),
          } : {
            program_binding_id: observed.selector.programBindingId,
            component_role_id: observed.selector.componentRoleId,
            target_role_id: observed.selector.targetRoleId,
          },
          expected_control_generation: observed.controlGeneration,
          scale,
        },
        commandRequestId,
      );
      if (runtimeScaleInFlight.current !== pending
        || runtimeScaleContext.current?.scopeFence !== pending.scopeFence) return;
      const accepted = acceptedRuntimeScaleStatus(
        observed,
        parseRuntimeScaleControlAccepted(receipt.result),
        commandRequestId,
        scale,
      );
      const current = runtimeScaleStatusRef.current;
      if (
        runtimeScaleContext.current?.scopeFence === context.scopeFence
        && (current === null || current.controlGeneration < accepted.controlGeneration)
      ) {
        runtimeScaleStatusRef.current = accepted;
        setRuntimeScaleStatus(accepted);
      }
      if (runtimeScaleContext.current?.scopeFence === context.scopeFence) {
        setRuntimeScaleError(null);
      }
    } catch (error: unknown) {
      if (runtimeScaleInFlight.current === pending
        && runtimeScaleContext.current?.scopeFence === pending.scopeFence) setRuntimeScaleError(errorMessage(
        error,
        'Runtime velocity could not be updated; owner state remains authoritative.',
      ));
    } finally {
      if (runtimeScaleInFlight.current === pending) {
        runtimeScaleInFlight.current = null;
        setRuntimeScalePending(runtimeScaleQueued.current !== null);
        scheduleRuntimeScaleFlush();
      }
    }
  }, [controlSource, runtimeScaleScopeFence, scheduleRuntimeScaleFlush]);
  runtimeScaleFlush.current = () => { void flushRuntimeScale(); };

  useEffect(() => {
    clearRuntimeScaleQueue();
    runtimeScaleInFlight.current = null;
    runtimeScaleLastSentAt.current = Number.NEGATIVE_INFINITY;
    runtimeScaleStatusRef.current = null;
    setRuntimeScaleStatus(null);
    setRuntimeScaleDraft(1);
    setRuntimeScaleError(null);
    // Scope changes initialize from the projection current on that render.
    // Later revisions in the same scope are merged by the effect below.
    const projected = projectedRuntimeScaleRef.current;
    if (projected !== null) {
      runtimeScaleStatusRef.current = projected;
      setRuntimeScaleStatus(projected);
      setRuntimeScaleDraft(projected.requestedScale);
    }
    return undefined;
  }, [
    clearRuntimeScaleQueue,
    runtimeScaleScopeFence,
  ]);

  useEffect(() => {
    if (runtimeScaleScopeFence === null || projectedRuntimeScale === null) return;
    const merged = mergeRuntimeScaleStatus(
      runtimeScaleStatusRef.current,
      projectedRuntimeScale,
    );
    runtimeScaleStatusRef.current = merged;
    setRuntimeScaleStatus(merged);
    if (runtimeScaleQueued.current === null && !runtimeScaleInFlight.current) {
      setRuntimeScaleDraft(merged.requestedScale);
    }
  }, [projectedRuntimeScale, runtimeScaleScopeFence]);

  useEffect(() => () => {
    runtimeScaleInFlight.current = null;
    if (runtimeScaleTimer.current !== null) {
      window.clearTimeout(runtimeScaleTimer.current);
    }
  }, []);

  useEffect(() => {
    if (programStopActive) clearRuntimeScaleQueue();
  }, [clearRuntimeScaleQueue, programStopActive]);

  const setRuntimeScale = useCallback((scale: number) => {
    const context = runtimeScaleContext.current;
    const observed = runtimeScaleStatusRef.current;
    if (
      context === null
      || observed === null
      || !observed.supported
      || observed.frozen
      || runtimeScaleStopActive.current
      || !Number.isFinite(scale)
    ) return;
    const bounded = Math.min(1, Math.max(0, scale));
    setRuntimeScaleDraft(bounded);
    setRuntimeScaleError(null);
    runtimeScaleQueued.current = bounded;
    setRuntimeScalePending(true);
    scheduleRuntimeScaleFlush();
  }, [scheduleRuntimeScaleFlush]);

  const commitRuntimeScale = useCallback(() => {
    if (runtimeScaleTimer.current !== null) {
      window.clearTimeout(runtimeScaleTimer.current);
      runtimeScaleTimer.current = null;
    }
    runtimeScaleFlush.current();
  }, []);

  const editJoint = useCallback((jointIndex: number, positionRad: number) => {
    if (manualControlBlocked.current) return;
    // Accumulate against the reducer's current draft, including several
    // joint edits delivered in one React batch.
    dispatch({ type: 'edit_joint', jointIndex, positionRad });
  }, []);

  const editMoveToJoints = useCallback((positionsRad: number[]) => {
    if (manualControlBlocked.current) return;
    dispatch({ type: 'edit_move_to_joint', positionsRad: [...positionsRad] });
  }, []);

  const selectMoveToSource = useCallback((source: MoveToDraftSource) => {
    if (!manualControlBlocked.current) dispatch({ type: 'select_move_source', source });
  }, []);
  const editMoveToOptions = useCallback((update: Partial<CartesianMoveDraftOptions>) => {
    if (!manualControlBlocked.current) dispatch({ type: 'edit_move_options', update });
  }, []);
  const beginPoseDrag = useCallback(() => {
    if (manualControlBlocked.current || tcpBlocksCartesian || (tcpInFlight.current !== null && cartesianMode) || !enabled || headlessSnapshot?.executionControlStatus == null || state.operation?.resident
      || robotControlHasActiveOperation(state) || approveInFlight.current !== null
      || !['pose_goal', 'cartesian_path', 'move_to'].includes(state.mode)
      || (state.mode === 'move_to' && state.moveToDraft?.source.kind !== 'pose')
      || pendingAuthoringInputs.current.size > 0 || state.poseDragging) return false;
    dragGeneration.current = state.generation + 1;
    dispatch({ type: 'begin_pose_drag' });
    return true;
  }, [state, enabled, headlessSnapshot?.executionControlStatus, tcpBlocksCartesian, cartesianMode]);
  const finishPoseDrag = useCallback((pose: Pose3Value | null, discard?: PoseDragDiscard) => {
    const generation = dragGeneration.current;
    if (generation !== null) dispatch({ type: 'finish_pose_drag', generation, pose, discard });
  }, []);

  const editPose = useCallback((pose: Pose3Value) => {
    if (manualControlBlocked.current) return;
    dispatch({ type: 'edit_pose', pose });
  }, []);

  const editGripper = useCallback((position: number) => {
    if (manualControlBlocked.current) return;
    dispatch({ type: 'edit_gripper', position });
  }, []);

  const editMotionSettings = useCallback((update: Partial<ManualMotionSettings>) => {
    if (manualControlBlocked.current) return;
    dispatch({ type: 'edit_motion_settings', update });
  }, []);

  const approve = useCallback(async () => {
    if (
      approveInFlight.current !== null
      || tcpBlocksCartesian
      || (tcpInFlight.current !== null && cartesianMode)
      || dragGeneration.current !== null
      || state.poseDragging
      || pendingAuthoringInputs.current.size > 0
      || manualControlBlocked.current
      || !robotControlPreviewApprovable(state)
      || state.intent === null
      || state.status === null
    ) return;
    const pending = { ownerEpoch: state.status.ownerEpoch, generation: state.generation };
    approveInFlight.current = pending;
    let pneumaticAbort: AbortController | null = null;
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      const payload = { ...state.intent } as Record<string, unknown>;
      delete payload.request_context;
      dispatch({ type: 'approve_started' });
      const currentTarget = selectedStatusForState(state);
      if (state.mode === 'gripper' && currentTarget?.directControlGripper?.targetDescriptor.pneumaticOutputs !== undefined) {
        if (state.draftGripperPosition === null) throw new Error('Choose a gripper target.');
        const start = latestSnapshot.current;
        if (start === null || start.programCycle.state !== 'idle_after_reset') {
          throw new Error('Mapped pneumatic manual control is available when the Program is idle after Reset.');
        }
        const cycleGeneration = start.programCycle.cycleGeneration;
        pneumaticAbort = new AbortController();
        pneumaticWriteAbort.current = pneumaticAbort;
        const abort = pneumaticAbort;
        const checkCurrent = () => {
          const snapshot = latestSnapshot.current;
          const live = snapshot?.executionControlStatus;
          if (abort.signal.aborted || runtimeScaleStopActive.current || snapshot == null
            || snapshot.programCycle.state !== 'idle_after_reset'
            || snapshot.programCycle.cycleGeneration !== cycleGeneration
            || live == null
            || live.graphRevision !== currentTarget.graphRevision
            || live.retainedBundleId !== currentTarget.retainedBundleId) {
            throw new Error('Manual pneumatic sequence stopped because the current Program or Deployment changed.');
          }
        };
        await writeManualPneumaticValves(currentTarget, state.draftGripperPosition, {
          nextRequestId: requestId,
          checkCurrent,
          submit: async (intent) => {
            const command = { ...intent } as Record<string, unknown>;
            delete command.request_context;
            return controlSource.sendExecutionCommand('operation_start', command, intent.request_context.request_id);
          },
          read: (operationId, intent, valve) => api.execution.readManualValveOperation(
            operationId, intent, valve.componentId, valve.binding.commandTargetId, abort.signal,
          ),
          wait: () => new Promise<void>((resolve) => window.setTimeout(resolve, 100)),
        });
        checkCurrent();
      }
      await controlSource.sendExecutionCommand(
        'operation_start',
        payload,
        state.intent.request_context.request_id,
      );
      // Receipt confirms transport handling only. The shared owner projection
      // moves this surface from Approving to Executing.
    } catch (error: unknown) {
      if (approveInFlight.current === pending && !pneumaticAbort?.signal.aborted) dispatch({
        type: 'approve_rejected',
        generation: state.generation,
        error: errorMessage(error, 'Motion approval was rejected.'),
        errorRefusal: commandRefusalCause(error),
      });
    } finally {
      if (pneumaticWriteAbort.current === pneumaticAbort) pneumaticWriteAbort.current = null;
      if (approveInFlight.current === pending) approveInFlight.current = null;
    }
  }, [controlSource, state, tcpBlocksCartesian, cartesianMode]);

  const revert = useCallback(() => {
    if (manualControlBlocked.current) return;
    dispatch({ type: 'revert' });
  }, []);

  const stop = useCallback(async () => {
    const operation = state.operation;
    const pending = pendingOperationControl.current;
    if (state.status === null || operation === null || !robotControlHasActiveOperation(state)
      || (pending?.control === 'stop' && pending.ownerEpoch === state.status.ownerEpoch
        && pending.operationId === operation.operationId)) return;
    clearRuntimeScaleQueue();
    await sendOperationControl('stop', 'operation_stop', {
      operations: [{
        operation_id: operation.operationId,
        expected_operation_revision: operation.operationRevision,
        expected_stop_generation: operation.activeStopGeneration ?? 0,
      }],
      reason: 'operator Robot Control Stop',
    });
  }, [clearRuntimeScaleQueue, sendOperationControl, state]);

  const cellFaultOpen = state.status?.cellStopAction.fault != null
    && state.status.cellStopAction.fault.state !== 'cleared';
  const statusComponents = state.status?.components;
  const incidentComponentIds = useMemo(
    () => (statusComponents ?? []).map((component) => component.controllableComponentId),
    [statusComponents],
  );
  const componentIncidents = useComponentIncidents(enabled && cellFaultOpen, incidentComponentIds);
  const refreshComponentIncidents = componentIncidents.refresh;
  const shownIncidents = componentIncidents.incidents;
  const shownIncidentsUnavailable = componentIncidents.unavailableReason;

  // "Recover cell" is one operator action (addendum 2, contract item 6). One
  // Headless call acknowledges every open component incident, waits briefly
  // for the cell latch, and clears it only when its owner reports it ready;
  // otherwise it returns the typed reason, shown here. A press may outlast
  // one HTTP call (each component re-enables within its own lifecycle
  // allowance): after an HTTP timeout the SAME request ID is sent again,
  // replay-only, and Headless answers `cell_recover_in_progress` or the
  // stored final result. A resend never starts a press: after a Headless
  // restart or eviction it answers an unknown outcome, shown as such.
  // The wait ends only on a final result or when the operator leaves.
  const recoverStopFault = useCallback(async () => {
    const status = state.status;
    const fault = status?.cellStopAction.fault;
    if (
      !enabled
      || status === null
      || fault === null
      || stopFaultRecoveryRequest.current !== null
      || cellRecoverBlockedReason !== null
    ) return;
    // M6 (item 36): the press acknowledges only the incidents on screen; an
    // unread list sends none (today's press).
    const shownIncidentIds = cellFaultOpen && shownIncidentsUnavailable === null
      ? shownIncidents.map(({ incident }) => incident.incidentId) : null;
    const pending = { abort: new AbortController() };
    const pressId = requestId();
    stopFaultRecoveryRequest.current = pending;
    setStopFaultRecoveryPending(true);
    setStopFaultRecoveryInProgress(false);
    setStopFaultRecoveryError(null);
    try {
      let result: CellRecoverResult;
      for (let resend = false; ; resend = true) {
        let reply: CellRecoverResult | null = null;
        try {
          reply = await api.execution.cellRecover(
            pressId, shownIncidentIds, pending.abort.signal, resend,
          );
        } catch (error: unknown) {
          if (apiErrorCode(error) !== 'headless_http_timeout') throw error;
        }
        if (stopFaultRecoveryRequest.current !== pending) return;
        if (reply !== null && reply.reason?.code !== 'cell_recover_in_progress') {
          result = reply;
          break;
        }
        setStopFaultRecoveryInProgress(true);
        if (reply !== null) {
          // Headless answered at once that the press still runs: ask again shortly.
          await new Promise((resolve) => window.setTimeout(resolve, CELL_RECOVER_REPLAY_PAUSE_MS));
          if (stopFaultRecoveryRequest.current !== pending) return;
        }
      }
      const message = cellRecoverMessage(result);
      if (message !== null && stopFaultRecoveryRequest.current === pending) {
        setStopFaultRecoveryError(message);
      }
    } catch (error: unknown) {
      if (stopFaultRecoveryRequest.current === pending) {
        setStopFaultRecoveryError(
          errorMessage(error, 'Cell stop-fault recovery was rejected.'),
        );
      }
    } finally {
      if (stopFaultRecoveryRequest.current === pending) {
        stopFaultRecoveryRequest.current = null;
        setStopFaultRecoveryPending(false);
        setStopFaultRecoveryInProgress(false);
      }
      refreshComponentIncidents();
    }
  }, [
    cellFaultOpen, cellRecoverBlockedReason, enabled, refreshComponentIncidents, shownIncidents,
    shownIncidentsUnavailable, state.status,
  ]);

  // SX-679: the operator's voluntary abort. Headless starts no later step of
  // the running press; the step in flight settles within its own limit and
  // the press then answers `cell_recover_aborted` with what was done, which
  // the pending press above receives and shows.
  const abortRecoverStopFault = useCallback(async () => {
    if (stopFaultRecoveryRequest.current === null) return;
    setStopFaultRecoveryAbortPending(true);
    try {
      await api.execution.cellRecoverAbort(requestId());
    } catch (error: unknown) {
      setStopFaultRecoveryError(errorMessage(error, 'Abort of Recover cell was rejected.'));
    } finally {
      setStopFaultRecoveryAbortPending(false);
    }
  }, []);

  const reset = useCallback(async (approvedUncertainOutputs: readonly UncertainOutput[] = []) => {
    const operation = state.operation;
    if (pendingOperationControl.current !== null || state.status === null || operation === null
      || !operation.resident || !operationStateIsTerminal(operation.state)) return;
    const acknowledged = acknowledgedUncertainOutputs(operation.operationId, approvedUncertainOutputs);
    await sendOperationControl('reset', 'operation_reset', {
      operation_id: operation.operationId,
      expected_operation_revision: operation.operationRevision,
      reason: 'operator Reset of terminal Robot Control operation',
      ...(acknowledged === null ? {} : { acknowledged_uncertain_outputs: acknowledged }),
    });
  }, [sendOperationControl, state.operation, state.status]);

  const resetUncertainOutputs = useCallback(async (approved: readonly UncertainOutput[]) => {
    if (approved.length === 0) return null;
    if (controlSource === null) return 'Control connection is not synchronized.';
    const operations = new Map<string, number>();
    for (const output of approved) {
      operations.set(output.operationId, Math.max(operations.get(output.operationId) ?? 0, output.operationRevision));
    }
    for (const [operationId, revision] of [...operations.entries()].sort(([left], [right]) => (left < right ? -1 : 1))) {
      try {
        const receipt = await controlSource.sendExecutionCommand('operation_reset', {
          operation_id: operationId,
          expected_operation_revision: revision,
          reason: 'operator Reset acknowledging outputs with unknown acceptance',
          acknowledged_uncertain_outputs: acknowledgedUncertainOutputs(operationId, approved),
        }, requestId());
        if (receipt.disposition === 'rejected') return `Reset of ${operationId} was refused (${receipt.code}).`;
      } catch (error: unknown) {
        return `Reset of ${operationId}: ${errorMessage(error, 'rejected.')}`;
      }
    }
    return null;
  }, [controlSource]);

  const retry = useCallback(() => {
    if (manualControlBlocked.current) {
      setCleanupFailure(null);
      setAvailability(null);
      refreshAvailability();
      return;
    }
    dispatch({ type: 'retry_preview' });
  }, []);
  const dismissRuntimeScaleError = useCallback(() => {
    setRuntimeScaleError(null);
  }, []);
  const dismissError = useCallback(() => {
    dispatch({ type: 'dismiss_error' });
  }, []);

  return useMemo(() => ({
    state,
    // Keep the complete owner projection for cross-target recovery and cell
    // controls. Target-specific authoring uses selectedStatus below.
    currentStatus: state.status,
    tcpResumeStatus,
    tcpResumeFresh,
    tcpResumeConnectionLost,
    tcpResumeCycle,
    selectedStatus,
    motionTargets,
    gripperTargets,
    manualControlAllowed,
    manualControlUnavailableReason,
    unappliedChanges,
    controlSource,
    setManualPanelVisible,
    controlPending,
    runtimeScaleStatus,
    runtimeScaleDraft,
    runtimeScalePending,
    runtimeScaleError,
    stopFaultRecoveryPending,
    stopFaultRecoveryInProgress,
    stopFaultRecoveryAbortPending,
    cellRecoverBlockedReason,
    setCellRecoverBlockedReason,
    stopFaultRecoveryError,
    componentIncidents: componentIncidents.incidents,
    componentIncidentsUnavailableReason: componentIncidents.unavailableReason,
    selectRobot,
    selectTool,
    selectTcp,
    tcpApplying,
    tcpError,
    selectMode,
    selectMoveToSource,
    editMoveToOptions,
    beginPoseDrag,
    finishPoseDrag,
    authoringPending,
    reportAuthoringPending,
    editMoveToJoints,
    editJoint,
    editPose,
    editGripper,
    editMotionSettings,
    approve,
    revert,
    stop,
    recoverStopFault,
    abortRecoverStopFault,
    dismissRuntimeScaleError,
    dismissError,
    reset,
    resetUncertainOutputs,
    setRuntimeScale,
    commitRuntimeScale,
    retry,
  }), [
    approve,
    controlPending,
    editGripper,
    editMotionSettings,
    editMoveToJoints,
    editJoint,
    editPose,
    reset,
    resetUncertainOutputs,
    retry,
    revert,
    runtimeScaleDraft,
    runtimeScaleError,
    stopFaultRecoveryPending,
    stopFaultRecoveryInProgress,
    stopFaultRecoveryAbortPending,
    cellRecoverBlockedReason,
    stopFaultRecoveryError,
    componentIncidents.incidents,
    componentIncidents.unavailableReason,
    runtimeScalePending,
    runtimeScaleStatus,
    selectMode,
    selectMoveToSource,
    editMoveToOptions,
    beginPoseDrag,
    finishPoseDrag,
    authoringPending,
    reportAuthoringPending,
    setRuntimeScale,
    commitRuntimeScale,
    state,
    selectedStatus,
    motionTargets,
    gripperTargets,
    manualControlAllowed,
    manualControlUnavailableReason,
    unappliedChanges,
    controlSource,
    stop,
    recoverStopFault,
    abortRecoverStopFault,
    selectRobot,
    selectTool,
    selectTcp,
    tcpResumeStatus,
    tcpResumeFresh,
    tcpResumeConnectionLost,
    tcpResumeCycle,
    tcpApplying,
    tcpError,
    dismissRuntimeScaleError,
    dismissError,
  ]);
}
