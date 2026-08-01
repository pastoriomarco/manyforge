import { useMemo, useState } from 'react';

export type HistorySeverityFilter = 'all' | 'info' | 'warning' | 'error';
export type HistoryAlarmFilter =
  | 'all'
  | 'lifecycle'
  | 'activation'
  | 'return'
  | 'acknowledgement'
  | 'raw';
export type HistoryAlarmPhase =
  | 'activation'
  | 'return'
  | 'acknowledgement'
  | null;

export interface ExecutionHistoryEventView {
  id: string;
  timestampNs: number;
  timestamp?: string | null;
  severity?: string | null;
  channel: string;
  message: string;
  executionId?: number | null;
  type: string;
  alarmPhase?: HistoryAlarmPhase;
  details?: unknown;
}

export function historySeverityGroup(
  severity: string | null | undefined,
): Exclude<HistorySeverityFilter, 'all'> {
  const normalized = (severity ?? 'INFO').toUpperCase();
  if (normalized === 'ERROR' || normalized === 'CRITICAL') return 'error';
  if (normalized === 'WARN' || normalized === 'WARNING') return 'warning';
  return 'info';
}

function matchesAlarmView(
  event: ExecutionHistoryEventView,
  alarmView: HistoryAlarmFilter,
): boolean {
  if (alarmView === 'all') return true;
  if (alarmView === 'raw') return !event.alarmPhase;
  if (alarmView === 'lifecycle') return Boolean(event.alarmPhase);
  return event.alarmPhase === alarmView;
}

export function filterExecutionHistoryEvents(
  events: ExecutionHistoryEventView[],
  options: {
    severity: HistorySeverityFilter;
    eventType: string;
    channel: string;
    search: string;
    alarmView?: HistoryAlarmFilter;
    conditionIdentity?: string;
    executionId?: number;
  },
): ExecutionHistoryEventView[] {
  const needle = options.search.trim().toLocaleLowerCase();
  const conditionNeedle = (options.conditionIdentity ?? '').trim().toLocaleLowerCase();
  return events.filter((event) => {
    if (
      options.severity !== 'all'
      && historySeverityGroup(event.severity) !== options.severity
    ) return false;
    if (options.eventType && event.type !== options.eventType) return false;
    if (options.channel && event.channel !== options.channel) return false;
    if (!matchesAlarmView(event, options.alarmView ?? 'all')) return false;
    if (
      options.executionId !== undefined
      && event.executionId !== options.executionId
    ) return false;
    const document = JSON.stringify(event).toLocaleLowerCase();
    if (conditionNeedle && !document.includes(conditionNeedle)) return false;
    return !needle || document.includes(needle);
  });
}

function eventTime(event: ExecutionHistoryEventView): string {
  if (event.timestamp) {
    const parsed = new Date(event.timestamp);
    if (!Number.isNaN(parsed.getTime())) return parsed.toLocaleTimeString();
  }
  return `${event.timestampNs} ns`;
}

interface ExecutionHistoryPanelProps {
  open: boolean;
  onClose: () => void;
  events: ExecutionHistoryEventView[];
  loading?: boolean;
  error?: string | null;
  currentExecutionId?: number;
}

/**
 * Prop-driven history presentation. I5 owns the exact retained event DTO and
 * polling client; this panel deliberately has no endpoint or lifecycle owner.
 */
export function ExecutionHistoryPanel({
  open,
  onClose,
  events,
  loading = false,
  error = null,
  currentExecutionId,
}: ExecutionHistoryPanelProps) {
  const [severity, setSeverity] = useState<HistorySeverityFilter>('all');
  const [alarmView, setAlarmView] = useState<HistoryAlarmFilter>('all');
  const [search, setSearch] = useState('');
  const [currentOnly, setCurrentOnly] = useState(true);
  const filtered = useMemo(
    () => filterExecutionHistoryEvents(events, {
      severity,
      alarmView,
      eventType: '',
      channel: '',
      search,
      executionId: currentOnly ? currentExecutionId : undefined,
    }),
    [alarmView, currentExecutionId, currentOnly, events, search, severity],
  );
  if (!open) return null;

  return (
    <section className="history-panel" aria-labelledby="history-title">
      <header>
        <div>
          <p className="eyebrow">Authoritative event projection</p>
          <h2 id="history-title">Execution history</h2>
        </div>
        <button type="button" onClick={onClose}>Close</button>
      </header>
      <div className="history-filters">
        <label>
          <span>Severity</span>
          <select
            value={severity}
            onChange={(event) => setSeverity(event.target.value as HistorySeverityFilter)}
          >
            <option value="all">All</option>
            <option value="info">Info</option>
            <option value="warning">Warnings</option>
            <option value="error">Errors</option>
          </select>
        </label>
        <label>
          <span>Alarm events</span>
          <select
            value={alarmView}
            onChange={(event) => setAlarmView(event.target.value as HistoryAlarmFilter)}
          >
            <option value="all">All events</option>
            <option value="lifecycle">Alarm lifecycle</option>
            <option value="activation">Activations</option>
            <option value="return">Returns</option>
            <option value="acknowledgement">Acknowledgements</option>
            <option value="raw">Non-alarm events</option>
          </select>
        </label>
        <label>
          <span>Search</span>
          <input value={search} onChange={(event) => setSearch(event.target.value)} />
        </label>
        <label className="history-current-toggle">
          <input
            type="checkbox"
            checked={currentOnly}
            disabled={currentExecutionId === undefined}
            onChange={(event) => setCurrentOnly(event.target.checked)}
          />
          Current execution
        </label>
      </div>
      <div className="history-events">
        {loading && <p className="muted">Loading history…</p>}
        {error && <p className="error-state" role="alert">{error}</p>}
        {!loading && !error && filtered.length === 0 && (
          <p className="muted">No events match these filters.</p>
        )}
        {filtered.map((event) => (
          <article
            key={event.id}
            className={`history-event severity-${historySeverityGroup(event.severity)}`}
          >
            <header>
              <strong>{event.message || event.type}</strong>
              <time>{eventTime(event)}</time>
            </header>
            <small>{event.channel} · {event.type}</small>
            {event.details !== undefined && (
              <pre>{JSON.stringify(event.details, null, 2)}</pre>
            )}
          </article>
        ))}
      </div>
    </section>
  );
}
