import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';

import type { ClientOperationalError } from '../components/HeadlessRuntimeSurfaces';

export type OperationalErrorAttentionPolicy =
  | 'transient'
  | 'acknowledgement_required';

export interface ClientOperationalHistoryEvent {
  id: string;
  timestampNs: number;
  timestamp: string;
  severity: 'ERROR' | 'INFO';
  channel: 'manyforge.composer';
  message: string;
  type:
    | 'composer_client_error_activated'
    | 'composer_client_error_updated'
    | 'composer_client_error_resolved'
    | 'composer_client_error_acknowledged'
    | 'composer_client_error_dismissed';
  details: {
    source: string;
    title: string;
    detail: string;
    attentionPolicy: OperationalErrorAttentionPolicy;
  };
}

export interface OperationalErrorRecord extends ClientOperationalError {
  source: string;
  attentionPolicy: OperationalErrorAttentionPolicy;
  dismissSource?: () => void;
}

interface OperationalErrorRegistry {
  errors: ClientOperationalError[];
  history: ClientOperationalHistoryEvent[];
  report: (
    source: string,
    title: string,
    message: string,
    attentionPolicy: OperationalErrorAttentionPolicy,
    dismissSource?: () => void,
  ) => void;
  resolve: (source: string) => void;
  dismiss: (id: string) => void;
}

const EMPTY_REGISTRY: OperationalErrorRegistry = {
  errors: [],
  history: [],
  report: () => undefined,
  resolve: () => undefined,
  dismiss: () => undefined,
};

export interface OperationalErrorState {
  records: Map<string, OperationalErrorRecord>;
  history: ClientOperationalHistoryEvent[];
  sequence: number;
}

export type OperationalErrorAction =
  | {
      kind: 'report';
      source: string;
      title: string;
      message: string;
      attentionPolicy: OperationalErrorAttentionPolicy;
      dismissSource?: () => void;
      observedAtMs: number;
    }
  | { kind: 'resolve'; source: string; observedAtMs: number }
  | { kind: 'dismiss'; id: string; observedAtMs: number };

const MAXIMUM_CLIENT_HISTORY_EVENTS = 128;

export function initialOperationalErrorState(): OperationalErrorState {
  return { records: new Map(), history: [], sequence: 0 };
}

function historyEvent(
  sequence: number,
  record: OperationalErrorRecord,
  type: ClientOperationalHistoryEvent['type'],
  observedAtMs: number,
): ClientOperationalHistoryEvent {
  const resolved = type === 'composer_client_error_resolved';
  const cleared = type === 'composer_client_error_acknowledged'
    || type === 'composer_client_error_dismissed';
  return {
    id: `composer-session:${sequence}`,
    timestampNs: Math.trunc(observedAtMs) * 1_000_000,
    timestamp: new Date(observedAtMs).toISOString(),
    severity: resolved || cleared ? 'INFO' : 'ERROR',
    channel: 'manyforge.composer',
    message: `${record.title}: ${record.message}`,
    type,
    details: {
      source: record.source,
      title: record.title,
      detail: record.message,
      attentionPolicy: record.attentionPolicy,
    },
  };
}

function withHistory(
  state: OperationalErrorState,
  record: OperationalErrorRecord,
  type: ClientOperationalHistoryEvent['type'],
  observedAtMs: number,
): Pick<OperationalErrorState, 'history' | 'sequence'> {
  const sequence = state.sequence + 1;
  return {
    sequence,
    history: [historyEvent(sequence, record, type, observedAtMs), ...state.history]
      .slice(0, MAXIMUM_CLIENT_HISTORY_EVENTS),
  };
}

export function reduceOperationalErrors(
  state: OperationalErrorState,
  action: OperationalErrorAction,
): OperationalErrorState {
  if (action.kind === 'report') {
    const existing = state.records.get(action.source);
    if (
      existing?.active === true
      && existing.message === action.message
      && existing.title === action.title
      && existing.attentionPolicy === action.attentionPolicy
    ) return state;
    const record: OperationalErrorRecord = {
      id: existing?.active === true
        ? existing.id
        : `${action.source}:${state.sequence + 1}`,
      source: action.source,
      title: action.title,
      message: action.message,
      active: true,
      acknowledgementRequired:
        action.attentionPolicy === 'acknowledgement_required',
      attentionPolicy: action.attentionPolicy,
      dismissSource: action.dismissSource,
    };
    const records = new Map(state.records);
    records.delete(action.source);
    records.set(action.source, record);
    return {
      records,
      ...withHistory(
        state,
        record,
        existing?.active === true
          ? 'composer_client_error_updated'
          : 'composer_client_error_activated',
        action.observedAtMs,
      ),
    };
  }
  const match = action.kind === 'resolve'
    ? state.records.get(action.source)
    : [...state.records.values()].find((record) => record.id === action.id);
  if (match === undefined || (action.kind === 'resolve' && !match.active)) {
    return state;
  }
  const records = new Map(state.records);
  if (
    action.kind === 'resolve'
    && match.attentionPolicy === 'acknowledgement_required'
  ) {
    records.set(match.source, { ...match, active: false, dismissSource: undefined });
  } else {
    records.delete(match.source);
  }
  return {
    records,
    ...withHistory(
      state,
      match,
      action.kind === 'resolve'
        ? 'composer_client_error_resolved'
        : match.attentionPolicy === 'acknowledgement_required'
          ? 'composer_client_error_acknowledged'
          : 'composer_client_error_dismissed',
      action.observedAtMs,
    ),
  };
}

const OperationalErrorContext = createContext<OperationalErrorRegistry>(EMPTY_REGISTRY);

/**
 * One session-local registry for actionable Composer failures. It is only a
 * presentation source: backend alarms and runtime authority stay with their
 * owners, while local response/transport failures become dismissible in the
 * same Runtime/history surface instead of creating private toast systems.
 */
export function OperationalErrorProvider({ children }: { children: ReactNode }) {
  const [state, setState] = useState<OperationalErrorState>(
    initialOperationalErrorState,
  );
  const report = useCallback<OperationalErrorRegistry['report']>((
    source,
    title,
    message,
    attentionPolicy,
    dismissSource,
  ) => {
    if (!message.trim()) return;
    setState((current) => reduceOperationalErrors(current, {
      kind: 'report',
      source,
      title,
      message,
      attentionPolicy,
      dismissSource,
      observedAtMs: Date.now(),
    }));
  }, []);
  const resolve = useCallback<OperationalErrorRegistry['resolve']>((source) => {
    setState((current) => reduceOperationalErrors(current, {
      kind: 'resolve', source, observedAtMs: Date.now(),
    }));
  }, []);
  const dismiss = useCallback((id: string) => {
    const callback = [...state.records.values()].find((record) => record.id === id)
      ?.dismissSource;
    setState((current) => reduceOperationalErrors(current, {
      kind: 'dismiss', id, observedAtMs: Date.now(),
    }));
    callback?.();
  }, [state.records]);
  const value = useMemo<OperationalErrorRegistry>(() => ({
    errors: [...state.records.values()].reverse(),
    history: state.history,
    report,
    resolve,
    dismiss,
  }), [dismiss, report, resolve, state.history, state.records]);
  return (
    <OperationalErrorContext.Provider value={value}>
      {children}
    </OperationalErrorContext.Provider>
  );
}

export function useOperationalErrors(): OperationalErrorRegistry {
  return useContext(OperationalErrorContext);
}

export function useOperationalErrorMirror({
  source,
  title,
  message,
  onDismiss,
  attentionPolicy = 'transient',
}: {
  source: string;
  title: string;
  message: string | null;
  onDismiss?: () => void;
  attentionPolicy?: OperationalErrorAttentionPolicy;
}) {
  const { report, resolve } = useOperationalErrors();
  const dismissRef = useRef(onDismiss);
  useEffect(() => {
    // Once its source recovers, acknowledging the retained record must not
    // restart or otherwise perturb that healthy source.
    dismissRef.current = message === null ? undefined : onDismiss;
  }, [message, onDismiss]);
  const dismiss = useCallback(() => dismissRef.current?.(), []);
  useEffect(() => {
    if (message === null) resolve(source);
    else report(source, title, message, attentionPolicy, dismiss);
  }, [attentionPolicy, dismiss, message, report, resolve, source, title]);
}
