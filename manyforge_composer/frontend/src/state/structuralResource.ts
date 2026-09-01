import { useCallback, useEffect, useReducer, useState } from 'react';

export type StructuralResourceState<T> =
  | { status: 'loading' }
  | { status: 'ready'; data: T }
  | { status: 'absent'; message: string }
  | { status: 'error'; message: string };

export type StructuralResourceAction<T> =
  | { type: 'load' }
  | { type: 'resolve'; data: T }
  | { type: 'absent'; message: string }
  | { type: 'reject'; message: string };

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
      return { status: 'error', message: action.message };
  }
}

export function errorMessage(error: unknown, fallback: string): string {
  return error instanceof Error && error.message.trim()
    ? error.message
    : fallback;
}

interface StructuralResourceOptions {
  absentWhen?: (error: unknown) => boolean;
  absentMessage?: string;
  disabledMessage?: string;
  enabled?: boolean;
  failureMessage: string;
}

export function useStructuralResource<T>(
  load: (signal: AbortSignal) => Promise<T>,
  options: StructuralResourceOptions,
): [StructuralResourceState<T>, () => void] {
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
  ]);

  const retry = useCallback(() => {
    setReloadToken((current) => current + 1);
  }, []);

  return [state, retry];
}
