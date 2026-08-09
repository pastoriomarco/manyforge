import { useCallback, useEffect, useReducer, useRef, useState } from 'react';
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
import type {
  RobotControlMode,
} from '../state/robotControl';
import type {
  DirectControlOperationIntent,
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

export interface RobotControlController {
  state: RobotControlState;
  selectMode: (mode: RobotControlMode) => void;
  editJoint: (jointIndex: number, positionRad: number) => void;
  editPose: (pose: Pose3Value) => void;
  editGripper: (position: number) => void;
  approve: () => Promise<void>;
  revert: () => void;
  stop: () => Promise<void>;
  retry: () => void;
}

export function useRobotControl(enabled: boolean): RobotControlController {
  const [state, dispatch] = useReducer(
    robotControlReducer,
    initialRobotControlState,
  );
  const [reloadToken, setReloadToken] = useState(0);
  const approveInFlight = useRef(false);
  const stopInFlight = useRef(false);

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
  }, [state]);

  const retry = useCallback(() => {
    setReloadToken((current) => current + 1);
  }, []);

  return {
    state,
    selectMode,
    editJoint,
    editPose,
    editGripper,
    approve,
    revert,
    stop,
    retry,
  };
}
