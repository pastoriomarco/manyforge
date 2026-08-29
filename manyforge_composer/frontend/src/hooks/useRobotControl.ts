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
  OperationRuntimeScaleStatus,
  Pose3Value,
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

const RUNTIME_SCALE_SEND_INTERVAL_MS = 100;

export interface RobotControlController {
  state: RobotControlState;
  controlPending: 'pause' | 'resume' | null;
  runtimeScaleStatus: OperationRuntimeScaleStatus | null;
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
  setRuntimeScale: (scale: number) => void;
  retry: () => void;
}

export function useRobotControl(enabled: boolean): RobotControlController {
  const [state, dispatch] = useReducer(
    robotControlReducer,
    initialRobotControlState,
  );
  const [reloadToken, setReloadToken] = useState(0);
  const [controlPending, setControlPending] = useState<'pause' | 'resume' | null>(null);
  const [runtimeScaleStatus, setRuntimeScaleStatus] = useState<OperationRuntimeScaleStatus | null>(null);
  const [runtimeScaleDraft, setRuntimeScaleDraft] = useState(1);
  const [runtimeScalePending, setRuntimeScalePending] = useState(false);
  const [runtimeScaleError, setRuntimeScaleError] = useState<string | null>(null);
  const [fullStopPending, setFullStopPending] = useState(false);
  const [fullStopError, setFullStopError] = useState<string | null>(null);
  const approveInFlight = useRef(false);
  const stopInFlight = useRef(false);
  const runtimeScaleStatusRef = useRef<OperationRuntimeScaleStatus | null>(null);
  const runtimeScaleQueued = useRef<number | null>(null);
  const runtimeScaleInFlight = useRef(false);
  const runtimeScaleTimer = useRef<number | null>(null);
  const runtimeScaleAbort = useRef<AbortController | null>(null);
  const runtimeScaleFlush = useRef<() => void>(() => undefined);

  useEffect(() => {
    if (!enabled) {
      dispatch({ type: 'clear' });
      return undefined;
    }
    const controller = new AbortController();
    let current = true;
    dispatch({ type: 'load' });
    void api.execution.status(controller.signal)
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
    void api.execution.status(controller.signal)
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

  const scalableOperationId = state.operation !== null
    && (state.operation.state === 'executing' || state.operation.state === 'paused')
    ? state.operation.operationId
    : null;
  const runtimeScaleContext = useRef<{ ownerEpoch: string; operationId: string } | null>(null);
  runtimeScaleContext.current = enabled
    && state.status !== null
    && scalableOperationId !== null
    ? { ownerEpoch: state.status.ownerEpoch, operationId: scalableOperationId }
    : null;

  const clearRuntimeScaleQueue = useCallback(() => {
    runtimeScaleQueued.current = null;
    runtimeScaleInFlight.current = false;
    runtimeScaleAbort.current?.abort();
    runtimeScaleAbort.current = null;
    if (runtimeScaleTimer.current !== null) {
      window.clearTimeout(runtimeScaleTimer.current);
      runtimeScaleTimer.current = null;
    }
    setRuntimeScalePending(false);
  }, []);

  const scheduleRuntimeScaleFlush = useCallback(() => {
    if (
      runtimeScaleTimer.current !== null
      || runtimeScaleInFlight.current
      || runtimeScaleQueued.current === null
    ) return;
    runtimeScaleTimer.current = window.setTimeout(() => {
      runtimeScaleTimer.current = null;
      runtimeScaleFlush.current();
    }, RUNTIME_SCALE_SEND_INTERVAL_MS);
  }, []);

  const flushRuntimeScale = useCallback(async () => {
    const context = runtimeScaleContext.current;
    const observed = runtimeScaleStatusRef.current;
    const scale = runtimeScaleQueued.current;
    if (
      context === null
      || observed === null
      || observed.operationId !== context.operationId
      || !observed.supported
      || scale === null
      || runtimeScaleInFlight.current
    ) return;
    runtimeScaleQueued.current = null;
    runtimeScaleInFlight.current = true;
    setRuntimeScalePending(true);
    const controller = new AbortController();
    runtimeScaleAbort.current = controller;
    try {
      await api.execution.setRuntimeScale(
        context.ownerEpoch,
        observed,
        scale,
        requestId(),
        controller.signal,
      );
      const next = await api.execution.runtimeScale(
        context.operationId,
        controller.signal,
      );
      if (
        !controller.signal.aborted
        && runtimeScaleContext.current?.operationId === context.operationId
      ) {
        runtimeScaleStatusRef.current = next;
        setRuntimeScaleStatus(next);
        if (runtimeScaleQueued.current === null) {
          setRuntimeScaleDraft(next.requestedScale);
        }
        setRuntimeScaleError(null);
      }
    } catch (error: unknown) {
      if (!controller.signal.aborted) {
        setRuntimeScaleError(errorMessage(
          error,
          'Runtime velocity could not be updated; owner state will be re-read.',
        ));
        try {
          const next = await api.execution.runtimeScale(context.operationId);
          if (runtimeScaleContext.current?.operationId === context.operationId) {
            runtimeScaleStatusRef.current = next;
            setRuntimeScaleStatus(next);
            if (runtimeScaleQueued.current === null) {
              setRuntimeScaleDraft(next.requestedScale);
            }
          }
        } catch {
          // The explicit error stays visible; a later operation or Retry reads
          // fresh owner state without treating the browser value as accepted.
        }
      }
    } finally {
      if (runtimeScaleAbort.current === controller) runtimeScaleAbort.current = null;
      runtimeScaleInFlight.current = false;
      setRuntimeScalePending(runtimeScaleQueued.current !== null);
      scheduleRuntimeScaleFlush();
    }
  }, [scheduleRuntimeScaleFlush]);
  runtimeScaleFlush.current = () => { void flushRuntimeScale(); };

  useEffect(() => {
    clearRuntimeScaleQueue();
    runtimeScaleStatusRef.current = null;
    setRuntimeScaleStatus(null);
    setRuntimeScaleDraft(1);
    setRuntimeScaleError(null);
    if (scalableOperationId === null) return undefined;
    const controller = new AbortController();
    void api.execution.runtimeScale(scalableOperationId, controller.signal)
      .then((observed) => {
        if (controller.signal.aborted) return;
        runtimeScaleStatusRef.current = observed;
        setRuntimeScaleStatus(observed);
        setRuntimeScaleDraft(observed.requestedScale);
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setRuntimeScaleError(errorMessage(
            error,
            'This operation does not expose runtime velocity control.',
          ));
        }
      });
    return () => controller.abort();
  }, [clearRuntimeScaleQueue, reloadToken, scalableOperationId]);

  useEffect(() => () => {
    runtimeScaleAbort.current?.abort();
    if (runtimeScaleTimer.current !== null) {
      window.clearTimeout(runtimeScaleTimer.current);
    }
  }, []);

  const setRuntimeScale = useCallback((scale: number) => {
    const context = runtimeScaleContext.current;
    const observed = runtimeScaleStatusRef.current;
    if (
      context === null
      || observed === null
      || observed.operationId !== context.operationId
      || !observed.supported
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
    dispatch({ type: 'revert' });
  }, []);

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
      setFullStopError(errorMessage(error, 'Full-cell Stop was rejected.'));
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
