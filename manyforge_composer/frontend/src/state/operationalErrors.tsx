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

interface OperationalErrorRecord extends ClientOperationalError {
  source: string;
  dismissSource?: () => void;
}

interface OperationalErrorRegistry {
  errors: ClientOperationalError[];
  report: (
    source: string,
    title: string,
    message: string,
    dismissSource?: () => void,
  ) => void;
  clear: (source: string) => void;
  dismiss: (id: string) => void;
}

const EMPTY_REGISTRY: OperationalErrorRegistry = {
  errors: [],
  report: () => undefined,
  clear: () => undefined,
  dismiss: () => undefined,
};

const OperationalErrorContext = createContext<OperationalErrorRegistry>(EMPTY_REGISTRY);

/**
 * One session-local registry for actionable Composer failures. It is only a
 * presentation source: backend alarms and runtime authority stay with their
 * owners, while local response/transport failures become dismissible in the
 * same Runtime/history surface instead of creating private toast systems.
 */
export function OperationalErrorProvider({ children }: { children: ReactNode }) {
  const [records, setRecords] = useState<Map<string, OperationalErrorRecord>>(
    () => new Map(),
  );
  const sequence = useRef(0);
  const report = useCallback<OperationalErrorRegistry['report']>((
    source,
    title,
    message,
    dismissSource,
  ) => {
    if (!message.trim()) return;
    setRecords((current) => {
      const existing = current.get(source);
      if (existing?.message === message && existing.title === title) {
        return current;
      }
      sequence.current += 1;
      const next = new Map(current);
      next.set(source, {
        id: `${source}:${sequence.current}`,
        source,
        title,
        message,
        dismissSource,
      });
      return next;
    });
  }, []);
  const clear = useCallback((source: string) => {
    setRecords((current) => {
      if (!current.has(source)) return current;
      const next = new Map(current);
      next.delete(source);
      return next;
    });
  }, []);
  const dismiss = useCallback((id: string) => {
    const callback = [...records.values()].find((record) => record.id === id)
      ?.dismissSource;
    setRecords((current) => {
      const match = [...current.entries()].find(([, record]) => record.id === id);
      if (match === undefined) return current;
      const next = new Map(current);
      next.delete(match[0]);
      return next;
    });
    callback?.();
  }, [records]);
  const value = useMemo<OperationalErrorRegistry>(() => ({
    errors: [...records.values()], report, clear, dismiss,
  }), [clear, dismiss, records, report]);
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
}: {
  source: string;
  title: string;
  message: string | null;
  onDismiss?: () => void;
}) {
  const { report, clear } = useOperationalErrors();
  const dismissRef = useRef(onDismiss);
  useEffect(() => {
    dismissRef.current = onDismiss;
  }, [onDismiss]);
  const dismiss = useCallback(() => dismissRef.current?.(), []);
  useEffect(() => {
    if (message === null) clear(source);
    else report(source, title, message, dismiss);
    return () => clear(source);
  }, [clear, dismiss, message, report, source, title]);
}
