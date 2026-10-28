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

/** One Composer request failure. It blocks nothing, so it is a warning. */
export interface ClientOperationalError {
  id: string;
  title: string;
  message: string;
  active: boolean;
  acknowledgementRequired: boolean;
  severity: 'warning';
}

export type OperationalErrorAttentionPolicy =
  | 'transient'
  | 'acknowledgement_required';

export interface OperationalErrorRecord extends ClientOperationalError {
  source: string;
  attentionPolicy: OperationalErrorAttentionPolicy;
  dismissSource?: () => void;
}

interface OperationalErrorRegistry {
  errors: ClientOperationalError[];
  report: (
    source: string,
    title: string,
    message: string,
    attentionPolicy: OperationalErrorAttentionPolicy,
    dismissSource?: () => void,
  ) => void;
  resolve: (source: string) => void;
  dismiss: (id: string) => void;
  /** Reset and Scenario Close clear every message. */
  clear: () => void;
}

const EMPTY_REGISTRY: OperationalErrorRegistry = {
  errors: [],
  report: () => undefined,
  resolve: () => undefined,
  dismiss: () => undefined,
  clear: () => undefined,
};

export interface OperationalErrorState {
  records: Map<string, OperationalErrorRecord>;
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
    }
  | { kind: 'resolve'; source: string }
  | { kind: 'dismiss'; id: string }
  | { kind: 'clear' };

export function initialOperationalErrorState(): OperationalErrorState {
  return { records: new Map(), sequence: 0 };
}

export function reduceOperationalErrors(
  state: OperationalErrorState,
  action: OperationalErrorAction,
): OperationalErrorState {
  if (action.kind === 'clear') {
    return state.records.size === 0
      ? state
      : { records: new Map(), sequence: state.sequence + 1 };
  }
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
      severity: 'warning',
      attentionPolicy: action.attentionPolicy,
      dismissSource: action.dismissSource,
    };
    const records = new Map(state.records);
    records.delete(action.source);
    records.set(action.source, record);
    return {
      records,
      sequence: state.sequence + 1,
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
    sequence: state.sequence + 1,
  };
}

const OperationalErrorContext = createContext<OperationalErrorRegistry>(EMPTY_REGISTRY);

/**
 * One session-local registry for Composer request failures (warning row).
 * It is only a presentation source: backend alarms and runtime authority stay
 * with their owners. A failure blocks nothing, so every record is a warning
 * that stays until acknowledged, or until Reset or Scenario Close clears it.
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
    }));
  }, []);
  const resolve = useCallback<OperationalErrorRegistry['resolve']>((source) => {
    setState((current) => reduceOperationalErrors(current, {
      kind: 'resolve', source,
    }));
  }, []);
  const dismiss = useCallback((id: string) => {
    const callback = [...state.records.values()].find((record) => record.id === id)
      ?.dismissSource;
    setState((current) => reduceOperationalErrors(current, {
      kind: 'dismiss', id,
    }));
    callback?.();
  }, [state.records]);
  const clear = useCallback(() => {
    setState((current) => reduceOperationalErrors(current, { kind: 'clear' }));
  }, []);
  const value = useMemo<OperationalErrorRegistry>(() => ({
    errors: [...state.records.values()].reverse(),
    report,
    resolve,
    dismiss,
    clear,
  }), [clear, dismiss, report, resolve, state.records]);
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
  // A newer command does not clear a failure: it stays until acknowledged.
  attentionPolicy = 'acknowledgement_required',
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
