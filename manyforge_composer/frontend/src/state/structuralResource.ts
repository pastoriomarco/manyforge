import { useCallback, useEffect, useReducer, useRef, useState } from 'react';

export type StructuralResourceState<T> =
  | { status: 'loading' }
  | { status: 'ready'; data: T }
  | { status: 'absent'; message: string }
  | { status: 'error'; message: string; retryable?: boolean; requestId?: number };

export type StructuralResourceAction<T> =
  | { type: 'load' }
  | { type: 'resolve'; data: T }
  | { type: 'absent'; message: string }
  | { type: 'reject'; message: string; retryable?: boolean; requestId?: number };

export function structuralResourceReducer<T>(
  state: StructuralResourceState<T>,
  action: StructuralResourceAction<T>,
): StructuralResourceState<T> {
  switch (action.type) {
    case 'load':
      // A refresh of an already coherent structural projection must not turn
      // that projection into a momentary absence. Exact-identity consumers
      // continue to validate the retained value while the replacement read is
      // in flight, and resolve/reject still publishes the read's real result.
      if (state.status === 'ready' || state.status === 'loading') return state;
      return { status: 'loading' };
    case 'resolve':
      return { status: 'ready', data: action.data };
    case 'absent':
      return { status: 'absent', message: action.message };
    case 'reject':
      return {
        status: 'error',
        message: action.message,
        ...(action.retryable === true ? { retryable: true } : {}),
        ...(action.requestId !== undefined ? { requestId: action.requestId } : {}),
      };
  }
}

// What a refusal waits for, as the owner's structured details name it.
const WAITING_FOR_DETAILS = [
  ['blocking_conditions', 'waiting for'],
  ['active_operation_ids', 'operations'],
  ['busy_resource_ids', 'resources'],
] as const;

export function errorMessage(error: unknown, fallback: string): string {
  const message = error instanceof Error && error.message.trim()
    ? error.message
    : fallback;
  const details = (error as { structuredError?: { details?: Record<string, unknown> | null } } | null)
    ?.structuredError?.details;
  const waiting = WAITING_FOR_DETAILS.flatMap(([key, label]) => (
    typeof details?.[key] === 'string' && details[key] !== ''
      ? [`${label}: ${(details[key] as string).split(',').join(', ')}`]
      : []
  ));
  return waiting.length === 0 ? message : `${message} (${waiting.join('; ')})`;
}

interface StructuralResourceOptions {
  absentWhen?: (error: unknown) => boolean;
  absentMessage?: string;
  disabledMessage?: string;
  enabled?: boolean;
  failureMessage: string;
  retryableWhen?: (error: unknown) => boolean;
}

export function useStructuralResource<T>(
  load: (signal: AbortSignal) => Promise<T>,
  options: StructuralResourceOptions,
): [StructuralResourceState<T>, () => number] {
  const [reloadToken, setReloadToken] = useState(0);
  const [state, dispatch] = useReducer(structuralResourceReducer<T>, {
    status: 'loading',
  });
  const {
    absentMessage,
    absentWhen,
    disabledMessage,
    enabled = true,
    failureMessage,
    retryableWhen,
  } = options;

  useEffect(() => {
    if (!enabled) {
      dispatch({
        type: 'absent',
        message: disabledMessage ?? 'This resource is unavailable in the current mode.',
      });
      return undefined;
    }
    const controller = new AbortController();
    let current = true;
    dispatch({ type: 'load' });
    void load(controller.signal)
      .then((data) => {
        if (current) {
          dispatch({ type: 'resolve', data });
        }
      })
      .catch((error: unknown) => {
        if (!current || controller.signal.aborted) {
          return;
        }
        if (absentWhen?.(error)) {
          dispatch({
            type: 'absent',
            message: absentMessage ?? 'No authored resource is available.',
          });
          return;
        }
        dispatch({
          type: 'reject',
          message: errorMessage(error, failureMessage),
          ...(retryableWhen?.(error) === true ? { retryable: true } : {}),
          requestId: reloadToken,
        });
      });
    return () => {
      current = false;
      controller.abort();
    };
  }, [
    absentMessage,
    absentWhen,
    disabledMessage,
    enabled,
    failureMessage,
    load,
    reloadToken,
    retryableWhen,
  ]);

  const reloadTokenRef = useRef(0);
  const retry = useCallback((): number => {
    const next = reloadTokenRef.current + 1;
    reloadTokenRef.current = next;
    setReloadToken(next);
    return next;
  }, []);

  return [state, retry];
}
