import { useEffect, useMemo, useState } from 'react';

import { api } from '../api/client';
import type {
  ExecutionHistoryEvent,
  ExecutionHistoryResponse,
} from '../types/api';

export type HistorySeverityFilter = 'all' | 'info' | 'warning' | 'error';

function severityGroup(severity: string | null | undefined): HistorySeverityFilter {
  const normalized = (severity ?? 'INFO').toUpperCase();
  if (normalized === 'ERROR' || normalized === 'CRITICAL') return 'error';
  if (normalized === 'WARN' || normalized === 'WARNING') return 'warning';
  return 'info';
}

export function filterExecutionHistoryEvents(
  events: ExecutionHistoryEvent[],
  options: {
    severity: HistorySeverityFilter;
    eventType: string;
    channel: string;
    search: string;
    executionId?: number;
  },
): ExecutionHistoryEvent[] {
  const needle = options.search.trim().toLowerCase();
  return events.filter((event) => {
    if (
      options.severity !== 'all'
      && severityGroup(event.severity) !== options.severity
    ) return false;
    if (options.eventType && event.type !== options.eventType) return false;
    if (options.channel && event.channel !== options.channel) return false;
    if (
      options.executionId !== undefined
      && event.execution_id !== options.executionId
    ) return false;
    if (!needle) return true;
    return JSON.stringify(event).toLowerCase().includes(needle);
  });
}

function eventTime(event: ExecutionHistoryEvent): string {
  if (event.timestamp) {
    const parsed = new Date(event.timestamp);
    if (!Number.isNaN(parsed.getTime())) return parsed.toLocaleTimeString();
  }
  return `${event.timestamp_ns} ns`;
}

function severityStyle(severity: string | null | undefined): string {
  switch (severityGroup(severity)) {
    case 'error':
      return 'border-red-700/70 bg-red-950/30 text-red-100';
    case 'warning':
      return 'border-amber-700/70 bg-amber-950/25 text-amber-100';
    default:
      return 'border-gray-700 bg-gray-900/60 text-gray-200';
  }
}

export function ExecutionHistoryPanel({
  open,
  onClose,
}: {
  open: boolean;
  onClose: () => void;
}) {
  const [limit, setLimit] = useState(100);
  const [history, setHistory] = useState<ExecutionHistoryResponse | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [severity, setSeverity] = useState<HistorySeverityFilter>('all');
  const [eventType, setEventType] = useState('');
  const [channel, setChannel] = useState('');
  const [search, setSearch] = useState('');
  const [currentExecutionOnly, setCurrentExecutionOnly] = useState(true);
  const [autoRefresh, setAutoRefresh] = useState(true);

  useEffect(() => {
    if (!open) return undefined;
    let cancelled = false;
    let inFlight = false;
    const refresh = async () => {
      if (inFlight) return;
      inFlight = true;
      setLoading(true);
      try {
        const result = await api.execution.history({ limit });
        if (!cancelled) {
          setHistory(result);
          setError(null);
        }
      } catch (reason) {
        if (!cancelled) {
          setError(reason instanceof Error ? reason.message : 'History unavailable');
        }
      } finally {
        inFlight = false;
        if (!cancelled) setLoading(false);
      }
    };
    void refresh();
    const timer = autoRefresh
      ? window.setInterval(() => void refresh(), 2000)
      : null;
    return () => {
      cancelled = true;
      if (timer !== null) window.clearInterval(timer);
    };
  }, [autoRefresh, limit, open]);

  const eventTypes = useMemo(
    () => Array.from(new Set((history?.events ?? []).map((event) => event.type))).sort(),
    [history],
  );
  const channels = useMemo(
    () => Array.from(new Set(
      (history?.events ?? [])
        .map((event) => event.channel)
        .filter((value): value is string => Boolean(value)),
    )).sort(),
    [history],
  );
  const visibleEvents = useMemo(() => filterExecutionHistoryEvents(
    [...(history?.events ?? [])].reverse(),
    {
      severity,
      eventType,
      channel,
      search,
      executionId:
        currentExecutionOnly && (history?.executionId ?? 0) > 0
          ? history!.executionId
          : undefined,
    },
  ), [channel, currentExecutionOnly, eventType, history, search, severity]);

  if (!open) return null;

  return (
    <aside
      aria-label="Messages and execution history"
      className="fixed bottom-0 right-0 top-11 z-[70] flex w-[min(44rem,calc(100vw-1rem))] flex-col border-l border-gray-600 bg-gray-950/98 text-gray-100 shadow-2xl shadow-black/70 backdrop-blur-sm"
    >
      <div className="flex items-center justify-between border-b border-gray-700 px-4 py-3">
        <div>
          <h2 className="text-sm font-semibold">Messages and history</h2>
          <div className="mt-0.5 text-[11px] text-gray-400">
            {history?.runId ? `Run ${history.runId}` : 'Current Composer session'}
            {' · '}{visibleEvents.length} shown / {history?.events.length ?? 0} loaded
          </div>
        </div>
        <button
          type="button"
          onClick={onClose}
          aria-label="Close messages history"
          className="rounded p-1.5 text-gray-400 hover:bg-gray-800 hover:text-white"
        >
          <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.6" aria-hidden>
            <path d="m3 3 10 10M13 3 3 13" strokeLinecap="round" />
          </svg>
        </button>
      </div>

      <div className="grid gap-2 border-b border-gray-700 bg-gray-900/60 p-3 sm:grid-cols-2">
        <label className="text-[11px] text-gray-400">
          Severity
          <select
            value={severity}
            onChange={(event) => setSeverity(event.target.value as HistorySeverityFilter)}
            className="mt-1 w-full rounded border border-gray-600 bg-gray-950 px-2 py-1.5 text-xs text-gray-100"
          >
            <option value="all">All severities</option>
            <option value="info">Messages (INFO)</option>
            <option value="warning">Warnings</option>
            <option value="error">Errors / critical</option>
          </select>
        </label>
        <label className="text-[11px] text-gray-400">
          Event type
          <select
            value={eventType}
            onChange={(event) => setEventType(event.target.value)}
            className="mt-1 w-full rounded border border-gray-600 bg-gray-950 px-2 py-1.5 text-xs text-gray-100"
          >
            <option value="">All event types</option>
            {eventTypes.map((value) => <option key={value} value={value}>{value}</option>)}
          </select>
        </label>
        <label className="text-[11px] text-gray-400">
          Channel
          <select
            value={channel}
            onChange={(event) => setChannel(event.target.value)}
            className="mt-1 w-full rounded border border-gray-600 bg-gray-950 px-2 py-1.5 text-xs text-gray-100"
          >
            <option value="">All channels</option>
            {channels.map((value) => <option key={value} value={value}>{value}</option>)}
          </select>
        </label>
        <label className="text-[11px] text-gray-400">
          Retained rows
          <select
            value={limit}
            onChange={(event) => setLimit(Number(event.target.value))}
            className="mt-1 w-full rounded border border-gray-600 bg-gray-950 px-2 py-1.5 text-xs text-gray-100"
          >
            {[50, 100, 250, 500, 1000].map((value) => (
              <option key={value} value={value}>{value}</option>
            ))}
          </select>
        </label>
        <label className="sm:col-span-2 text-[11px] text-gray-400">
          Search message or details
          <input
            value={search}
            onChange={(event) => setSearch(event.target.value)}
            className="mt-1 w-full rounded border border-gray-600 bg-gray-950 px-2 py-1.5 text-xs text-gray-100"
            placeholder="collision, warning, node name, error code…"
          />
        </label>
        <div className="sm:col-span-2 flex flex-wrap gap-4 text-xs text-gray-300">
          <label className="flex items-center gap-1.5">
            <input
              type="checkbox"
              checked={currentExecutionOnly}
              onChange={(event) => setCurrentExecutionOnly(event.target.checked)}
              disabled={(history?.executionId ?? 0) <= 0}
              className="accent-emerald-500"
            />
            Current execution{(history?.executionId ?? 0) > 0 ? ` #${history!.executionId}` : ''}
          </label>
          <label className="flex items-center gap-1.5">
            <input
              type="checkbox"
              checked={autoRefresh}
              onChange={(event) => setAutoRefresh(event.target.checked)}
              className="accent-emerald-500"
            />
            Auto-refresh
          </label>
          {loading && <span className="text-gray-500">Refreshing…</span>}
        </div>
      </div>

      {error && (
        <div role="alert" className="border-b border-red-800 bg-red-950/50 px-3 py-2 text-xs text-red-200">
          {error}
        </div>
      )}
      <div className="min-h-0 flex-1 overflow-y-auto p-3">
        {visibleEvents.length === 0 ? (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-4 text-center text-xs text-gray-500">
            No events match the selected filters.
          </div>
        ) : visibleEvents.map((event) => (
          <details
            key={`${event.event_index}:${event.type}`}
            className={`mb-2 rounded border px-3 py-2 text-xs ${severityStyle(event.severity)}`}
          >
            <summary className="cursor-pointer list-none">
              <div className="flex items-start gap-2">
                <span className="mt-0.5 shrink-0 rounded bg-black/25 px-1.5 py-0.5 font-mono text-[10px] uppercase">
                  {event.severity ?? 'INFO'}
                </span>
                <span className="min-w-0 flex-1 break-words">
                  {event.message || event.type.replaceAll('_', ' ')}
                </span>
                <span className="shrink-0 font-mono text-[10px] opacity-65">{eventTime(event)}</span>
              </div>
              <div className="mt-1 flex flex-wrap gap-2 font-mono text-[10px] opacity-65">
                <span>{event.type}</span>
                {event.channel && <span>{event.channel}</span>}
                {event.execution_id > 0 && <span>execution #{event.execution_id}</span>}
              </div>
            </summary>
            <pre className="mt-2 max-h-72 overflow-auto rounded bg-black/40 p-2 text-[10px] leading-4 text-gray-200">
              {JSON.stringify(event, null, 2)}
            </pre>
          </details>
        ))}
      </div>
    </aside>
  );
}
