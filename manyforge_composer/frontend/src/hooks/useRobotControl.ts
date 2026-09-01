import {
  useCallback,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';
import { api } from '../api/client';
import {
  buildPreviewIntent,
  initialRobotControlState,
  operationStateIsTerminal,
  ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS,
  ROBOT_CONTROL_OPERATION_POLL_MS,
  robotControlHasActiveOperation,
  robotControlReducer,
} from '../state/robotControl';
import type { RobotControlState } from '../state/robotControl';
import type { RobotControlMode } from '../state/robotControl';
import type {
  DirectControlOperationIntent,
  Pose3Value,
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
    status.activeControlConfigurationId ?? 'no-active-control-configuration',
    status.activeControlConfigurationGeneration
      ?? 'no-active-control-configuration-generation',
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

export interface RobotControlController {
  state: RobotControlState;
  controlPending: 'pause' | 'resume' | 'reset' | null;
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
  retry: () => void;
}

export function useRobotControl(
  enabled: boolean,
  observeProgramRun = false,
  programStopActive = false,
): RobotControlController {
  const [state, dispatch] = useReducer(
    robotControlReducer,
    initialRobotControlState,
  );
  const [reloadToken, setReloadToken] = useState(0);
  const [controlPending, setControlPending] = useState<
    'pause' | 'resume' | 'reset' | null
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
  const runtimeScaleAbort = useRef<AbortController | null>(null);
  const runtimeScaleFlush = useRef<() => void>(() => undefined);
  const runtimeScaleStopActive = useRef(programStopActive);
  runtimeScaleStopActive.current = programStopActive;
  const programRunWasObserved = useRef(false);

  useEffect(() => {
    if (!enabled) {
      dispatch({ type: 'clear' });
      return undefined;
    }
    const controller = new AbortController();
    let current = true;
    dispatch({ type: 'load' });
    void api.execution.controlStatus(controller.signal)
      .then((status) => {
        if (current) dispatch({ type: 'status_resolved', status });
      })
      .catch((error: unknown) => {
        if (current && !controller.signal.aborted) {
          dispatch({
            type: 'load_failed',
            error: errorMessage(error, 'Execution owner status is unavailable.'),
          });
        }
      });
    return () => {
      current = false;
      controller.abort();
    };
  }, [enabled, reloadToken]);

  useEffect(() => {
    if (!enabled || !observeProgramRun) return;
    programRunWasObserved.current = true;
  }, [enabled, observeProgramRun]);

  useEffect(() => {
    if (!enabled) {
      programRunWasObserved.current = false;
      return undefined;
    }
    if (observeProgramRun || !programRunWasObserved.current) return undefined;
    programRunWasObserved.current = false;
    const controller = new AbortController();
    void api.execution.controlStatus(controller.signal)
      .then((status) => dispatch({ type: 'status_refreshed', status }))
      .catch(() => undefined);
    return () => controller.abort();
  }, [enabled, observeProgramRun]);

  useEffect(() => {
    if (
      !enabled
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
    dispatch({ type: 'select_mode', mode });
  }, []);

  const operationId = state.operation?.operationId ?? null;
  const operationActive = state.operation !== null
    && !operationStateIsTerminal(state.operation.state);
  useEffect(() => {
    if (!enabled || operationId === null || !operationActive) return undefined;
    let current = true;
    let timer: number | null = null;
    let controller: AbortController | null = null;
    const poll = () => {
      if (!current) return;
      controller = new AbortController();
      void api.execution.read(operationId, controller.signal)
        .then((operation) => {
          if (!current) return;
          dispatch({ type: 'operation_observed', operation });
          if (!operationStateIsTerminal(operation.state)) {
            timer = window.setTimeout(poll, ROBOT_CONTROL_OPERATION_POLL_MS);
          }
        })
        .catch((error: unknown) => {
          if (!current || controller?.signal.aborted) return;
          dispatch({
            type: 'operation_poll_failed',
            error: errorMessage(error, 'Operation status is temporarily unavailable.'),
          });
          timer = window.setTimeout(poll, ROBOT_CONTROL_OPERATION_POLL_MS);
        });
    };
    timer = window.setTimeout(poll, 0);
    return () => {
      current = false;
      if (timer !== null) window.clearTimeout(timer);
      controller?.abort();
    };
  }, [enabled, operationActive, operationId]);

  const terminalOperation = state.operation !== null
    && operationStateIsTerminal(state.operation.state)
    ? `${state.operation.operationId}:${state.operation.operationRevision}`
    : null;
  useEffect(() => {
    if (!enabled || terminalOperation === null) return undefined;
    const controller = new AbortController();
    let current = true;
    void api.execution.controlStatus(controller.signal)
      .then((status) => {
        if (current) dispatch({ type: 'status_refreshed', status });
      })
      .catch(() => {
        // The terminal operation remains authoritative. A later explicit retry
        // refreshes observed state without rewriting the terminal outcome.
      });
    return () => {
      current = false;
      controller.abort();
    };
  }, [enabled, terminalOperation]);

  const settleResidentOperation = useCallback(async (
    control: 'acknowledge_completed' | 'reset',
    ownerEpoch: string,
    operationId: string,
    operationRevision: number,
    signal?: AbortSignal,
  ) => {
    let admissionError: unknown = null;
    try {
      if (control === 'acknowledge_completed') {
        await api.execution.acknowledgeCompleted(
          ownerEpoch,
          operationId,
          operationRevision,
          requestId(),
          signal,
        );
      } else {
        await api.execution.resetOperation(
          ownerEpoch,
          operationId,
          operationRevision,
          requestId(),
          signal,
        );
      }
    } catch (error: unknown) {
      admissionError = error;
    }
    for (let attempt = 0; attempt < 40; attempt += 1) {
      const status = await api.execution.controlStatus(signal);
      if (!status.residentOperations.some(
        (operation) => operation.operationId === operationId,
      )) return status;
      if (admissionError !== null) throw admissionError;
      await new Promise((resolve) => window.setTimeout(resolve, 50));
    }
    throw new Error('The terminal operation remains resident after settlement.');
  }, []);

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
    const controller = new AbortController();
    let current = true;
    dispatch({ type: 'operation_settlement_started' });
    const settle = async () => {
      try {
        const status = await settleResidentOperation(
          automaticSettlementControl,
          settlementOwnerEpoch,
          automaticSettlementOperationId,
          automaticSettlementOperationRevision,
          controller.signal,
        );
        if (current) dispatch({ type: 'status_refreshed', status });
      } catch (error: unknown) {
        if (!current || controller.signal.aborted) return;
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
    return () => {
      current = false;
      controller.abort();
    };
  }, [
    automaticSettlementControl,
    automaticSettlementOperationId,
    automaticSettlementOperationRevision,
    enabled,
    reloadToken,
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
    runtimeScaleAbort.current?.abort();
    runtimeScaleAbort.current = null;
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
    setRuntimeScalePending(true);
    const controller = new AbortController();
    runtimeScaleAbort.current = controller;
    try {
      const accepted = await api.execution.setRuntimeScaleControl(
        context.ownerEpoch,
        observed,
        scale,
        requestId(),
        controller.signal,
      );
      if (
        !controller.signal.aborted
        && runtimeScaleContext.current?.scopeFence === context.scopeFence
      ) {
        const next: RuntimeScaleControlStatus = {
          ...observed,
          controlGeneration: accepted.controlGeneration,
          requestedScale: accepted.requestedScale,
          activeOperationId: accepted.activeOperationId,
          activeOperationRevision: accepted.activeOperationRevision,
          activeOperationScaleGeneration: accepted.activeOperationScaleGeneration,
        };
        runtimeScaleStatusRef.current = next;
        setRuntimeScaleStatus(next);
        if (runtimeScaleQueued.current === null) {
          setRuntimeScaleDraft(next.requestedScale);
        }
        setRuntimeScaleError(null);
        if (runtimeScaleQueued.current === null) {
          // Read once after the latest accepted value, outside the transport
          // cadence fence. This captures backend-owned effective/application
          // evidence without doubling every coalesced pointer update.
          void api.execution.runtimeScaleControl(context.selector)
            .then((projected) => {
              if (runtimeScaleContext.current?.scopeFence !== context.scopeFence) return;
              const merged = mergeRuntimeScaleStatus(
                runtimeScaleStatusRef.current,
                projected,
              );
              runtimeScaleStatusRef.current = merged;
              setRuntimeScaleStatus(merged);
              if (runtimeScaleQueued.current === null) {
                setRuntimeScaleDraft(merged.requestedScale);
              }
            })
            .catch(() => {
              // The accepted value remains authoritative. A later focused
              // projection refreshes application evidence.
            });
        }
      }
    } catch (error: unknown) {
      if (!controller.signal.aborted) {
        setRuntimeScaleError(errorMessage(
          error,
          'Runtime velocity could not be updated; owner state will be re-read.',
        ));
        try {
          const next = await api.execution.runtimeScaleControl(context.selector);
          if (runtimeScaleContext.current?.scopeFence === context.scopeFence) {
            runtimeScaleStatusRef.current = next;
            setRuntimeScaleStatus(next);
            if (runtimeScaleQueued.current === null) {
              setRuntimeScaleDraft(next.requestedScale);
            }
          }
        } catch {
          // The explicit error stays visible; a later owner projection or
          // automatic reconnect reads fresh state without treating the browser
          // value as accepted.
        }
      }
    } finally {
      if (runtimeScaleAbort.current === controller) runtimeScaleAbort.current = null;
      runtimeScaleInFlight.current = false;
      setRuntimeScalePending(runtimeScaleQueued.current !== null);
      scheduleRuntimeScaleFlush();
    }
  }, [runtimeScaleScopeFence, scheduleRuntimeScaleFlush]);
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
    runtimeScaleAbort.current?.abort();
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

  const editJoint = useCallback((jointIndex: number, positionRad: number) => {
    if (
      state.status === null
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
    dispatch({ type: 'edit_move_to_joint', positionsRad: [...positionsRad] });
  }, []);

  const editPose = useCallback((pose: Pose3Value) => {
    dispatch({ type: 'edit_pose', pose });
  }, []);

  const editGripper = useCallback((position: number) => {
    dispatch({ type: 'edit_gripper', position });
  }, []);

  const approve = useCallback(async () => {
    if (
      approveInFlight.current
      || state.phase !== 'ready'
      || state.intent === null
      || state.preview === null
    ) return;
    approveInFlight.current = true;
    dispatch({ type: 'approve_started' });
    try {
      const acceptance = await api.execution.submit(state.intent);
      dispatch({ type: 'approve_resolved', acceptance });
    } catch (error: unknown) {
      dispatch({
        type: 'approve_rejected',
        error: errorMessage(error, 'Motion approval was rejected.'),
      });
    } finally {
      approveInFlight.current = false;
    }
  }, [state.intent, state.phase, state.preview]);

  const revert = useCallback(() => {
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
    const ownerEpoch = state.status.ownerEpoch;
    dispatch({ type: 'operation_settlement_started' });
    void settleResidentOperation(
      state.operation.state === 'completed' ? 'acknowledge_completed' : 'reset',
      ownerEpoch,
      operationId,
      operationRevision,
    ).then((status) => {
      dispatch({ type: 'status_refreshed', status });
    }).catch((error: unknown) => {
      dispatch({
        type: 'operation_settlement_failed',
        error: errorMessage(
          error,
          'The terminal Robot Control operation could not be cleared.',
        ),
      });
    });
  }, [settleResidentOperation, state.operation, state.status]);

  const stop = useCallback(async () => {
    if (
      stopInFlight.current
      || state.status === null
      || state.operation === null
      || !robotControlHasActiveOperation(state)
    ) return;
    clearRuntimeScaleQueue();
    stopInFlight.current = true;
    dispatch({ type: 'stop_started' });
    try {
      await api.execution.stop(
        state.status.ownerEpoch,
        state.operation,
        requestId(),
      );
    } catch (error: unknown) {
      dispatch({
        type: 'stop_rejected',
        error: errorMessage(error, 'Operation Stop was rejected.'),
      });
    } finally {
      stopInFlight.current = false;
    }
  }, [clearRuntimeScaleQueue, state]);

  const fullStop = useCallback(async () => {
    const action = state.status?.cellStopAction;
    if (
      !enabled
      || fullStopPending
      || state.status === null
      || action === undefined
      || !action.available
    ) return;
    clearRuntimeScaleQueue();
    setFullStopPending(true);
    setFullStopError(null);
    try {
      await api.execution.fullStop(
        state.status.ownerEpoch,
        action.expectedStopGeneration,
        requestId(),
      );
      setReloadToken((current) => current + 1);
    } catch (error: unknown) {
      setFullStopError(errorMessage(error, 'Full Stop was rejected.'));
    } finally {
      setFullStopPending(false);
    }
  }, [clearRuntimeScaleQueue, enabled, fullStopPending, state.status]);

  const controlOperation = useCallback(async (control: 'pause' | 'resume') => {
    if (
      controlPending !== null
      || state.status === null
      || state.operation === null
      || (control === 'pause' && state.operation.state !== 'executing')
      || (control === 'resume' && state.operation.state !== 'paused')
    ) return;
    setControlPending(control);
    try {
      await api.execution[control](
        state.status.ownerEpoch,
        state.operation,
        requestId(),
      );
      const observed = await api.execution.read(state.operation.operationId);
      dispatch({ type: 'operation_observed', operation: observed });
    } catch (error: unknown) {
      dispatch({
        type: 'operation_poll_failed',
        error: errorMessage(error, `Operation ${control} was rejected.`),
      });
    } finally {
      setControlPending(null);
    }
  }, [controlPending, state]);

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
    setControlPending('reset');
    dispatch({ type: 'operation_settlement_started' });
    try {
      const status = await settleResidentOperation(
        'reset',
        state.status.ownerEpoch,
        state.operation.operationId,
        state.operation.operationRevision,
      );
      dispatch({ type: 'status_refreshed', status });
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
    setReloadToken((current) => current + 1);
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
    state,
    stop,
    fullStop,
    dismissFullStopError,
    dismissRuntimeScaleError,
    dismissError,
  ]);
}
