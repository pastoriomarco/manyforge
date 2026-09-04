import {
  useCallback,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';
import { api } from '../api/client';
import { parseRuntimeScaleControlAccepted } from '../api/parsers';
import {
  buildPreviewIntent,
  initialRobotControlState,
  operationStateIsTerminal,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  robotControlHasActiveOperation,
  robotControlReducer,
} from '../state/robotControl';
import type { ComposerHeadlessState } from '../types/headless';
import type { HeadlessStateSource } from './useHeadlessState';
import type { RobotControlState } from '../state/robotControl';
import type { RobotControlMode } from '../state/robotControl';
import type {
  DirectControlOperationIntent,
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
  return [
    ownerEpoch,
    status.selector.programBindingId,
    status.selector.componentRoleId,
    status.selector.targetRoleId,
    status.controlDomainId ?? 'no-control-domain',
    status.controlDomainRevision ?? 'no-control-domain-revision',
    status.controlBindingId ?? 'no-control-binding',
    status.controlBindingRevision ?? 'no-control-binding-revision',
    status.scalingDomainId ?? 'no-scaling-domain',
  ].join('\u0000');
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
    || accepted.selector.programBindingId !== submitted.selector.programBindingId
    || accepted.selector.componentRoleId !== submitted.selector.componentRoleId
    || accepted.selector.targetRoleId !== submitted.selector.targetRoleId
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

export interface RobotControlController {
  state: RobotControlState;
  controlPending: 'stop' | 'pause' | 'resume' | 'reset' | null;
  runtimeScaleStatus: RuntimeScaleControlStatus | null;
  runtimeScaleDraft: number;
  runtimeScalePending: boolean;
  runtimeScaleError: string | null;
  fullStopPending: boolean;
  fullStopError: string | null;
  selectMode: (mode: RobotControlMode) => void;
  editMoveToJoints: (positionsRad: number[]) => void;
  editJoint: (jointIndex: number, positionRad: number) => void;
  editPose: (pose: Pose3Value) => void;
  editGripper: (position: number) => void;
  approve: () => Promise<void>;
  revert: () => void;
  stop: () => Promise<void>;
  fullStop: () => Promise<void>;
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
  const approveInFlight = useRef(false);
  const stopInFlight = useRef(false);
  const runtimeScaleStatusRef = useRef<RuntimeScaleControlStatus | null>(null);
  const runtimeScaleQueued = useRef<number | null>(null);
  const runtimeScaleInFlight = useRef(false);
  const runtimeScaleTimer = useRef<number | null>(null);
  const runtimeScaleLastSentAt = useRef(Number.NEGATIVE_INFINITY);
  const runtimeScaleFlush = useRef<() => void>(() => undefined);
  const runtimeScaleStopActive = useRef(programStopActive);
  runtimeScaleStopActive.current = programStopActive;
  const programRunActive = useRef(observeProgramRun);
  programRunActive.current = observeProgramRun;
  const projectedOwnerRef = useRef<string | null>(null);
  const requestedSettlementRef = useRef<string | null>(null);

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
      type: observeProgramRun
        ? 'program_status_refreshed'
        : initialize
          ? 'status_resolved'
          : 'status_refreshed',
      status,
    });
  }, [enabled, headlessSnapshot?.executionControlStatus, observeProgramRun]);

  useEffect(() => {
    if (
      !enabled
      || observeProgramRun
      || state.phase !== 'planning'
      || state.status === null
    ) return undefined;
    const generation = state.generation;
    const previewDraft = {
      status: state.status,
      mode: state.mode,
      moveToKind: state.moveToKind,
      draftPositionsRad: state.draftPositionsRad,
      draftPose: state.draftPose,
      draftGripperPosition: state.draftGripperPosition,
    };
    const controller = new AbortController();
    let current = true;
    const timer = window.setTimeout(() => {
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
      dispatch({ type: 'preview_started', generation, intent });
      const preview = intent.participants[0].payload.kind === 'motion'
        ? api.execution.previewMotion(intent, controller.signal)
        : api.execution.previewCapabilityCommand(intent, controller.signal);
      void preview
        .then((receipt) => {
          if (current) {
            dispatch({ type: 'preview_resolved', generation, receipt });
          }
        })
        .catch((error: unknown) => {
          if (current && !controller.signal.aborted) {
            dispatch({
              type: 'preview_rejected',
              generation,
              error: errorMessage(error, 'Motion preview failed.'),
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
    state.draftPositionsRad,
    state.draftPose,
    state.draftGripperPosition,
    state.generation,
    state.mode,
    state.moveToKind,
    state.phase,
    state.status,
  ]);

  const selectMode = useCallback((mode: RobotControlMode) => {
    if (programRunActive.current) return;
    dispatch({ type: 'select_mode', mode });
  }, []);

  useEffect(() => {
    dispatch({ type: 'program_activity_changed' });
  }, [observeProgramRun]);

  const settleResidentOperation = useCallback(async (
    control: 'acknowledge_completed' | 'reset',
    operationId: string,
    operationRevision: number,
  ) => {
    if (controlSource === null) {
      throw new Error('Control connection is not synchronized.');
    }
    const correlationId = requestId();
    await controlSource.sendExecutionCommand(
      control === 'acknowledge_completed'
        ? 'operation_acknowledge_completed'
        : 'operation_reset',
      {
        operation_id: operationId,
        expected_operation_revision: operationRevision,
        reason: control === 'acknowledge_completed'
          ? 'settle completed Robot Control operation'
          : 'settle terminal Robot Control operation',
      },
      correlationId,
    );
  }, [controlSource]);

  const automaticSettlementControl = state.operation?.resident
    && (state.operation.state === 'completed'
      || state.operation.state === 'stopped'
      || state.operation.state === 'cancelled_pre_dispatch')
    ? state.operation.state === 'completed'
      ? 'acknowledge_completed'
      : 'reset'
    : null;
  const automaticSettlementOperationId = automaticSettlementControl === null
    ? null
    : state.operation!.operationId;
  const automaticSettlementOperationRevision = automaticSettlementControl === null
    ? null
    : state.operation!.operationRevision;
  const settlementOwnerEpoch = state.status?.ownerEpoch ?? null;
  useEffect(() => {
    if (
      !enabled
      || automaticSettlementControl === null
      || automaticSettlementOperationId === null
      || automaticSettlementOperationRevision === null
      || settlementOwnerEpoch === null
    ) return undefined;
    const settlementKey = [
      automaticSettlementControl,
      automaticSettlementOperationId,
      automaticSettlementOperationRevision,
    ].join(':');
    if (requestedSettlementRef.current === settlementKey) return undefined;
    requestedSettlementRef.current = settlementKey;
    const settle = async () => {
      try {
        await settleResidentOperation(
          automaticSettlementControl,
          automaticSettlementOperationId,
          automaticSettlementOperationRevision,
        );
      } catch (error: unknown) {
        requestedSettlementRef.current = null;
        dispatch({
          type: 'operation_settlement_failed',
          error: errorMessage(
            error,
            'The terminal Robot Control operation could not be settled.',
          ),
        });
      }
    };
    void settle();
    return undefined;
  }, [
    automaticSettlementControl,
    automaticSettlementOperationId,
    automaticSettlementOperationRevision,
    enabled,
    settleResidentOperation,
    settlementOwnerEpoch,
  ]);

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
      runtimeScaleInFlight.current,
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
    runtimeScaleInFlight.current = true;
    runtimeScaleLastSentAt.current = performance.now();
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      const commandRequestId = requestId();
      const receipt = await controlSource.sendExecutionCommand(
        'runtime_scale',
        {
          selector: {
            program_binding_id: observed.selector.programBindingId,
            component_role_id: observed.selector.componentRoleId,
            target_role_id: observed.selector.targetRoleId,
          },
          expected_control_generation: observed.controlGeneration,
          scale,
        },
        commandRequestId,
      );
      const accepted = acceptedRuntimeScaleStatus(
        observed,
        parseRuntimeScaleControlAccepted(receipt.result),
        commandRequestId,
        scale,
      );
      const current = runtimeScaleStatusRef.current;
      if (
        runtimeScaleContext.current?.scopeFence === context.scopeFence
        && (current === null || current.controlGeneration <= accepted.controlGeneration)
      ) {
        runtimeScaleStatusRef.current = accepted;
        setRuntimeScaleStatus(accepted);
      }
      if (runtimeScaleContext.current?.scopeFence === context.scopeFence) {
        setRuntimeScaleError(null);
      }
    } catch (error: unknown) {
      setRuntimeScaleError(errorMessage(
        error,
        'Runtime velocity could not be updated; owner state remains authoritative.',
      ));
    } finally {
      runtimeScaleInFlight.current = false;
      setRuntimeScalePending(runtimeScaleQueued.current !== null);
      scheduleRuntimeScaleFlush();
    }
  }, [controlSource, runtimeScaleScopeFence, scheduleRuntimeScaleFlush]);
  runtimeScaleFlush.current = () => { void flushRuntimeScale(); };

  useEffect(() => {
    clearRuntimeScaleQueue();
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
    if (
      programRunActive.current
      || state.status === null
      || robotControlHasActiveOperation(state)
      || !Number.isFinite(positionRad)
      || jointIndex < 0
      || jointIndex >= state.draftPositionsRad.length
    ) return;
    const positionsRad = [...state.draftPositionsRad];
    positionsRad[jointIndex] = positionRad;
    dispatch({ type: 'edit_joint', positionsRad });
  }, [state]);

  const editMoveToJoints = useCallback((positionsRad: number[]) => {
    if (programRunActive.current) return;
    dispatch({ type: 'edit_move_to_joint', positionsRad: [...positionsRad] });
  }, []);

  const editPose = useCallback((pose: Pose3Value) => {
    if (programRunActive.current) return;
    dispatch({ type: 'edit_pose', pose });
  }, []);

  const editGripper = useCallback((position: number) => {
    if (programRunActive.current) return;
    dispatch({ type: 'edit_gripper', position });
  }, []);

  const approve = useCallback(async () => {
    if (
      approveInFlight.current
      || programRunActive.current
      || state.phase !== 'ready'
      || state.intent === null
      || state.preview === null
    ) return;
    approveInFlight.current = true;
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
      dispatch({
        type: 'approve_rejected',
        error: errorMessage(error, 'Motion approval was rejected.'),
      });
    } finally {
      approveInFlight.current = false;
    }
  }, [controlSource, state.intent, state.phase, state.preview]);

  const revert = useCallback(() => {
    if (programRunActive.current) return;
    if (
      state.status === null
      || state.operation === null
      || !state.operation.resident
      || !operationStateIsTerminal(state.operation.state)
    ) {
      dispatch({ type: 'revert' });
      return;
    }
    const operationId = state.operation.operationId;
    const operationRevision = state.operation.operationRevision;
    if (controlPending !== null) return;
    setControlPending('reset');
    void settleResidentOperation(
      state.operation.state === 'completed' ? 'acknowledge_completed' : 'reset',
      operationId,
      operationRevision,
    ).catch((error: unknown) => {
      dispatch({
        type: 'operation_settlement_failed',
        error: errorMessage(
          error,
          'The terminal Robot Control operation could not be cleared.',
        ),
      });
    }).finally(() => setControlPending(null));
  }, [controlPending, settleResidentOperation, state.operation, state.status]);

  const stop = useCallback(async () => {
    if (
      stopInFlight.current
      || state.status === null
      || state.operation === null
      || !robotControlHasActiveOperation(state)
    ) return;
    stopInFlight.current = true;
    const operation = state.operation;
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      clearRuntimeScaleQueue();
      setControlPending('stop');
      await controlSource.sendExecutionCommand(
        'operation_stop',
        {
          operations: [{
            operation_id: operation.operationId,
            expected_operation_revision: operation.operationRevision,
            expected_stop_generation: operation.activeStopGeneration ?? 0,
          }],
          reason: 'operator Robot Control Stop',
        },
        requestId(),
      );
    } catch (error: unknown) {
      dispatch({
        type: 'operation_poll_failed',
        error: errorMessage(error, 'Operation Stop was rejected.'),
      });
    } finally {
      stopInFlight.current = false;
      setControlPending(null);
    }
  }, [clearRuntimeScaleQueue, controlSource, state]);

  const fullStop = useCallback(async () => {
    const action = state.status?.cellStopAction;
    if (
      !enabled
      || fullStopPending
      || state.status === null
      || action === undefined
      || !action.available
    ) return;
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      clearRuntimeScaleQueue();
      setFullStopPending(true);
      setFullStopError(null);
      await controlSource.sendExecutionCommand(
        'cell_full_stop',
        {
          expected_stop_generation: action.expectedStopGeneration,
        },
        requestId(),
      );
    } catch (error: unknown) {
      setFullStopError(errorMessage(error, 'Full Stop was rejected.'));
    } finally {
      setFullStopPending(false);
    }
  }, [clearRuntimeScaleQueue, controlSource, enabled, fullStopPending, state.status]);

  const controlOperation = useCallback(async (control: 'pause' | 'resume') => {
    if (
      controlPending !== null
      || state.status === null
      || state.operation === null
      || (control === 'pause' && state.operation.state !== 'executing')
      || (control === 'resume' && state.operation.state !== 'paused')
    ) return;
    const operation = state.operation;
    try {
      if (controlSource === null) {
        throw new Error('Control connection is not synchronized.');
      }
      setControlPending(control);
      await controlSource.sendExecutionCommand(
        control === 'pause' ? 'operation_pause' : 'operation_resume',
        {
          operation_id: operation.operationId,
          expected_operation_revision: operation.operationRevision,
          reason: `operator Robot Control ${control}`,
        },
        requestId(),
      );
    } catch (error: unknown) {
      dispatch({
        type: 'operation_poll_failed',
        error: errorMessage(error, `Operation ${control} was rejected.`),
      });
    } finally {
      setControlPending(null);
    }
  }, [controlPending, controlSource, state]);

  const pause = useCallback(
    async () => controlOperation('pause'),
    [controlOperation],
  );
  const resume = useCallback(
    async () => controlOperation('resume'),
    [controlOperation],
  );
  const reset = useCallback(async () => {
    if (
      controlPending !== null
      || state.status === null
      || state.operation === null
      || !state.operation.resident
      || !operationStateIsTerminal(state.operation.state)
    ) return;
    try {
      setControlPending('reset');
      await settleResidentOperation(
        'reset',
        state.operation.operationId,
        state.operation.operationRevision,
      );
    } catch (error: unknown) {
      dispatch({
        type: 'operation_settlement_failed',
        error: errorMessage(error, 'Operation Reset was rejected.'),
      });
    } finally {
      setControlPending(null);
    }
  }, [controlPending, settleResidentOperation, state.operation, state.status]);

  const retry = useCallback(() => {
    controlSource?.reconnect();
  }, [controlSource]);
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
    controlPending,
    runtimeScaleStatus,
    runtimeScaleDraft,
    runtimeScalePending,
    runtimeScaleError,
    fullStopPending,
    fullStopError,
    selectMode,
    editMoveToJoints,
    editJoint,
    editPose,
    editGripper,
    approve,
    revert,
    stop,
    fullStop,
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
    runtimeScalePending,
    runtimeScaleStatus,
    selectMode,
    setRuntimeScale,
    commitRuntimeScale,
    state,
    stop,
    fullStop,
    dismissFullStopError,
    dismissRuntimeScaleError,
    dismissError,
  ]);
}
