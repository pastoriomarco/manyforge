import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';
import { api, ApiError } from '../api/client';
import { parseRuntimeScaleControlAccepted } from '../api/parsers';
import {
  buildPreviewIntent,
  buildJointMotionIntent,
  buildGripperCommandIntent,
  gripperTargetsForStatus,
  initialRobotControlState,
  operationStateIsTerminal,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  robotControlHasActiveOperation,
  robotControlHasLocalDraft,
  robotControlReducer,
  selectedStatusForState,
  motionTargetsForStatus,
} from '../state/robotControl';
import type { ComposerHeadlessState } from '../types/headless';
import type { HeadlessStateSource } from './useHeadlessState';
import type {
  ManualCartesianDefaults,
  ManualMotionSettings,
  RobotControlState,
} from '../state/robotControl';
import type { RobotControlMode } from '../state/robotControl';
import type { CartesianMoveDraftOptions, MoveToDraftSource } from '../utils/moveTo';
import type {
  DirectControlOperationIntent,
  DirectControlGripperStatus,
  DirectControlMotionStatus,
  ExecutionOwnerStatus,
  ManualControlAvailability,
  Pose3Value,
  RuntimeScaleControlAccepted,
  RuntimeScaleControlStatus,
} from '../types/robotControl';

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
  const detail = (error.payload as {
    detail?: { code?: string; details?: { reason?: string } };
  } | null)?.detail;
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

/** Cold ownership/hold revisions only; never pose samples or node tick status. */
export function manualAvailabilityScopeFence(
  snapshot: ComposerHeadlessState | null,
  mode: RobotControlMode,
  selection?: Pick<RobotControlState, 'selectedComponentId' | 'selectedMotionTargetId' | 'selectedGripperTargetId'>,
): string | null {
  const status = snapshot?.executionControlStatus;
  if (status == null || snapshot == null) return null;
  const selected = selection === undefined
    ? status
    : selectedStatusForState({ status, ...selection });
  if (selected === null) return null;
  const cycle = snapshot.programCycle;
  const target = mode === 'gripper' ? selected.directControlGripper : selected.directControl;
  return JSON.stringify([
    status.ownerEpoch, status.retainedBundleId, status.graphRevision,
    target.programBindingId, target.programRunId,
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
  /** Selected addressed robot/tool projection used by all direct-control UI. */
  selectedStatus: ExecutionOwnerStatus | null;
  motionTargets: DirectControlMotionStatus[];
  gripperTargets: DirectControlGripperStatus[];
  manualControlAllowed: boolean;
  manualControlUnavailableReason: string | null;
  setManualPanelVisible: (visible: boolean) => void;
  controlPending: 'stop' | 'pause' | 'resume' | 'reset' | null;
  runtimeScaleStatus: RuntimeScaleControlStatus | null;
  runtimeScaleDraft: number;
  runtimeScalePending: boolean;
  runtimeScaleError: string | null;
  fullStopPending: boolean;
  fullStopError: string | null;
  stopFaultRecoveryPending: boolean;
  stopFaultRecoveryError: string | null;
  selectRobot: (componentId: string, motionTargetId: string) => void;
  selectTool: (gripperTargetId: string) => void;
  selectMode: (mode: RobotControlMode) => void;
  selectMoveToSource: (source: MoveToDraftSource) => void;
  editMoveToOptions: (update: Partial<CartesianMoveDraftOptions>) => void;
  beginPoseDrag: () => boolean;
  finishPoseDrag: (pose: Pose3Value | null) => void;
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
  fullStop: () => Promise<void>;
  recoverStopFault: () => Promise<void>;
  dismissFullStopError: () => void;
  dismissRuntimeScaleError: () => void;
  dismissError: () => void;
  pause: () => Promise<void>;
  resume: () => Promise<void>;
  reset: () => Promise<void>;
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
): RobotControlController {
  const [state, dispatch] = useReducer(
    robotControlReducer,
    initialRobotControlState,
  );
  const [controlPending, setControlPending] = useState<
    'stop' | 'pause' | 'resume' | 'reset' | null
  >(null);
  const [runtimeScaleStatus, setRuntimeScaleStatus] = useState<RuntimeScaleControlStatus | null>(null);
  const [runtimeScaleDraft, setRuntimeScaleDraft] = useState(1);
  const [runtimeScalePending, setRuntimeScalePending] = useState(false);
  const [runtimeScaleError, setRuntimeScaleError] = useState<string | null>(null);
  const [fullStopPending, setFullStopPending] = useState(false);
  const [fullStopError, setFullStopError] = useState<string | null>(null);
  const fullStopRequest = useRef<{ ownerEpoch: string } | null>(null);
  const [stopFaultRecoveryPending, setStopFaultRecoveryPending] = useState(false);
  const [stopFaultRecoveryError, setStopFaultRecoveryError] = useState<string | null>(null);
  const stopFaultRecoveryRequest = useRef<{ ownerEpoch: string } | null>(null);
  const approveInFlight = useRef<{ ownerEpoch: string; generation: number } | null>(null);
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
    control: 'stop' | 'pause' | 'resume' | 'reset'; ownerEpoch: string; operationId: string;
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
  const selectedComponentId = state.selectedComponentId;
  const selectedMotionTargetId = state.selectedMotionTargetId;
  const selectedGripperTargetId = state.selectedGripperTargetId;
  const selectedStatus = useMemo(
    () => selectedStatusForState({
      status: ownerStatus,
      selectedComponentId,
      selectedMotionTargetId,
      selectedGripperTargetId,
    }),
    [ownerStatus, selectedComponentId, selectedMotionTargetId,
      selectedGripperTargetId],
  );
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
  const manualControlAllowed = !observeProgramRun || (currentAvailability?.available === true && cleanupFailure === null);
  const manualControlUnavailableReason = manualControlAllowed ? null : cleanupFailure
    ?? (availability?.scope === availabilityScope ? availability.error : null)
    ?? currentAvailability?.unavailableReason?.replaceAll('_', ' ')
    ?? 'Checking backend manual-control availability.';
  const manualControlBlocked = useRef(!manualControlAllowed);
  manualControlBlocked.current = !manualControlAllowed;
  const projectedOwnerRef = useRef<string | null>(null);
  const previewStatusRef = useRef(selectedStatus);
  previewStatusRef.current = selectedStatus;
  const availabilityStatusRef = useRef(selectedStatus);
  availabilityStatusRef.current = selectedStatus;

  useEffect(() => {
    // An old owner's response cannot block or reject a successor's draft.
    cleanupInFlight.current = null;
    setCleanupFailure(null);
  }, [headlessSnapshot?.executionControlStatus?.ownerEpoch,
    headlessSnapshot?.programCycle?.programRunnerOwnerEpoch,
    headlessSnapshot?.programCycle?.programRunId]);

  useEffect(() => {
    if (!enabled || !observeProgramRun || !observeManualAvailability || availabilityScope === null) return;
    const status = availabilityStatusRef.current;
    if (status == null) return;
    const controller = new AbortController();
    let current = true;
    const read = async () => {
      try {
        if (state.mode === 'gripper' && status.observedGripper === null) {
          throw new Error('Gripper observation is unavailable.');
        }
        const intent = state.mode === 'gripper'
          ? buildGripperCommandIntent(status, status.observedGripper!.position, requestId())
          : buildJointMotionIntent(status, status.observedMotion.positionsRad, requestId());
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
    state.mode]);

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

  useEffect(() => {
    if (cartesianDefaults === null) return;
    dispatch({ type: 'program_cartesian_defaults_changed', defaults: cartesianDefaults });
  }, [cartesianDefaults]);

  useEffect(() => {
    const previewStatus = previewStatusRef.current;
    if (
      !enabled
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
                || cycle.programRunnerOwnerEpoch !== context.program_runner_owner_epoch
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

  const selectRobot = useCallback((componentId: string, motionTargetId: string) => {
    dispatch({ type: 'select_robot', componentId, motionTargetId });
  }, []);

  const selectTool = useCallback((gripperTargetId: string) => {
    dispatch({ type: 'select_tool', gripperTargetId });
  }, []);

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
    if (fullStopRequest.current?.ownerEpoch !== state.status?.ownerEpoch) {
      fullStopRequest.current = null;
      setFullStopPending(false);
      setFullStopError(null);
    }
    if (stopFaultRecoveryRequest.current?.ownerEpoch !== state.status?.ownerEpoch) {
      stopFaultRecoveryRequest.current = null;
      setStopFaultRecoveryPending(false);
      setStopFaultRecoveryError(null);
    }
  }, [state.status?.ownerEpoch]);

  useEffect(() => {
    const pending = approveInFlight.current;
    if (pending !== null && (pending.ownerEpoch !== state.status?.ownerEpoch
      || pending.generation !== state.generation || state.phase !== 'approving')) {
      approveInFlight.current = null;
    }
  }, [state.status?.ownerEpoch, state.generation, state.phase]);

  const sendOperationControl = useCallback(async (
    control: 'stop' | 'pause' | 'resume' | 'reset',
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
    if (manualControlBlocked.current || !enabled || headlessSnapshot?.executionControlStatus == null || state.operation?.resident
      || robotControlHasActiveOperation(state) || approveInFlight.current !== null
      || !['pose_goal', 'cartesian_path', 'move_to'].includes(state.mode)
      || (state.mode === 'move_to' && state.moveToDraft?.source.kind !== 'pose')
      || pendingAuthoringInputs.current.size > 0 || state.poseDragging) return false;
    dragGeneration.current = state.generation + 1;
    dispatch({ type: 'begin_pose_drag' });
    return true;
  }, [state, enabled, headlessSnapshot?.executionControlStatus]);
  const finishPoseDrag = useCallback((pose: Pose3Value | null) => {
    const generation = dragGeneration.current;
    if (generation !== null) dispatch({ type: 'finish_pose_drag', generation, pose });
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
      || dragGeneration.current !== null
      || state.poseDragging
      || pendingAuthoringInputs.current.size > 0
      || manualControlBlocked.current
      || state.phase !== 'ready'
      || state.intent === null
      || state.preview === null
      || state.status === null
    ) return;
    const pending = { ownerEpoch: state.status.ownerEpoch, generation: state.generation };
    approveInFlight.current = pending;
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      const payload = { ...state.intent } as Record<string, unknown>;
      delete payload.request_context;
      dispatch({ type: 'approve_started' });
      await controlSource.sendExecutionCommand(
        'operation_start',
        payload,
        state.intent.request_context.request_id,
      );
      // Receipt confirms transport handling only. The shared owner projection
      // moves this surface from Approving to Executing.
    } catch (error: unknown) {
      if (approveInFlight.current === pending) dispatch({
        type: 'approve_rejected',
        generation: state.generation,
        error: errorMessage(error, 'Motion approval was rejected.'),
      });
    } finally {
      if (approveInFlight.current === pending) approveInFlight.current = null;
    }
  }, [controlSource, state.generation, state.intent, state.phase, state.preview, state.status, state.poseDragging]);

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

  const fullStop = useCallback(async () => {
    const action = state.status?.cellStopAction;
    if (
      !enabled
      || fullStopRequest.current !== null
      || state.status === null
      || action === undefined
      || !action.available
    ) return;
    const pending = { ownerEpoch: state.status.ownerEpoch };
    fullStopRequest.current = pending;
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      clearRuntimeScaleQueue();
      const response = controlSource.sendExecutionCommand(
        'cell_full_stop',
        {
          expected_stop_generation: action.expectedStopGeneration,
        },
        requestId(),
      );
      setFullStopPending(true);
      setFullStopError(null);
      await response;
    } catch (error: unknown) {
      if (fullStopRequest.current === pending) setFullStopError(errorMessage(error, 'Full Stop was rejected.'));
    } finally {
      if (fullStopRequest.current === pending) {
        fullStopRequest.current = null;
        setFullStopPending(false);
      }
    }
  }, [clearRuntimeScaleQueue, controlSource, enabled, state.status]);

  const recoverStopFault = useCallback(async () => {
    const status = state.status;
    const fault = status?.cellStopAction.fault;
    if (
      !enabled
      || status === null
      || fault === null
      || stopFaultRecoveryRequest.current !== null
    ) return;
    const pending = { ownerEpoch: status.ownerEpoch };
    stopFaultRecoveryRequest.current = pending;
    setStopFaultRecoveryPending(true);
    setStopFaultRecoveryError(null);
    try {
      await api.execution.recoverStopFault(status.ownerEpoch, requestId());
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
      }
    }
  }, [enabled, state.status]);

  const controlOperation = useCallback(async (control: 'pause' | 'resume') => {
    const operation = state.operation;
    if (pendingOperationControl.current !== null || state.status === null || operation === null
      || (control === 'pause' && operation.state !== 'executing')
      || (control === 'resume' && operation.state !== 'paused')) return;
    await sendOperationControl(control, control === 'pause' ? 'operation_pause' : 'operation_resume', {
      operation_id: operation.operationId,
      expected_operation_revision: operation.operationRevision,
      reason: `operator Robot Control ${control}`,
    });
  }, [sendOperationControl, state.operation, state.status]);

  const pause = useCallback(async () => controlOperation('pause'), [controlOperation]);
  const resume = useCallback(async () => controlOperation('resume'), [controlOperation]);
  const reset = useCallback(async () => {
    const operation = state.operation;
    if (pendingOperationControl.current !== null || state.status === null || operation === null
      || !operation.resident || !operationStateIsTerminal(operation.state)) return;
    await sendOperationControl('reset', 'operation_reset', {
      operation_id: operation.operationId,
      expected_operation_revision: operation.operationRevision,
      reason: 'operator Reset of terminal Robot Control operation',
    });
  }, [sendOperationControl, state.operation, state.status]);

  const retry = useCallback(() => {
    if (manualControlBlocked.current) {
      setCleanupFailure(null);
      setAvailability(null);
      refreshAvailability();
      return;
    }
    dispatch({ type: 'retry_preview' });
  }, []);
  const dismissFullStopError = useCallback(() => {
    setFullStopError(null);
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
    selectedStatus,
    motionTargets,
    gripperTargets,
    manualControlAllowed,
    manualControlUnavailableReason,
    setManualPanelVisible,
    controlPending,
    runtimeScaleStatus,
    runtimeScaleDraft,
    runtimeScalePending,
    runtimeScaleError,
    fullStopPending,
    fullStopError,
    stopFaultRecoveryPending,
    stopFaultRecoveryError,
    selectRobot,
    selectTool,
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
    fullStop,
    recoverStopFault,
    dismissFullStopError,
    dismissRuntimeScaleError,
    dismissError,
    pause,
    resume,
    reset,
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
    pause,
    reset,
    resume,
    retry,
    revert,
    runtimeScaleDraft,
    runtimeScaleError,
    fullStopPending,
    fullStopError,
    stopFaultRecoveryPending,
    stopFaultRecoveryError,
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
    stop,
    fullStop,
    recoverStopFault,
    selectRobot,
    selectTool,
    dismissFullStopError,
    dismissRuntimeScaleError,
    dismissError,
  ]);
}
