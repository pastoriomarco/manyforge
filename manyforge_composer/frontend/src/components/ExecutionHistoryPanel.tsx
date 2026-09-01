import { useMemo, useState } from 'react';

import type { HistorianListQuery } from '../api/client';
import type { HistorianDetail } from '../types/headless';

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
  sequence?: number;
  timestampNs: number;
  timestamp?: string | null;
  severity?: string | null;
  channel: string;
  message: string;
  executionId?: number | null;
  programRunId?: string | null;
  operationId?: string | null;
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
    startNs?: number;
    endNs?: number;
    programRunId?: string;
    operationId?: string;
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
    if (options.startNs !== undefined && event.timestampNs < options.startNs) return false;
    if (options.endNs !== undefined && event.timestampNs > options.endNs) return false;
    if (options.programRunId && event.programRunId !== options.programRunId) return false;
    if (options.operationId && event.operationId !== options.operationId) return false;
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
  embedded?: boolean;
  onReload?: (query: HistorianListQuery) => void;
  onLoadOlder?: () => void;
  hasOlder?: boolean;
  selectedDetail?: HistorianDetail | null;
  detailLoading?: boolean;
  detailError?: string | null;
  onSelectEvent?: (event: ExecutionHistoryEventView) => void;
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
  embedded = false,
  onReload,
  onLoadOlder,
  hasOlder = false,
  selectedDetail = null,
  detailLoading = false,
  detailError = null,
  onSelectEvent,
}: ExecutionHistoryPanelProps) {
  const [severity, setSeverity] = useState<HistorySeverityFilter>('all');
  const [alarmView, setAlarmView] = useState<HistoryAlarmFilter>('all');
  const [search, setSearch] = useState('');
  const [timeWindow, setTimeWindow] = useState<'all' | 'minute' | 'five_minutes' | 'hour'>('all');
  const [component, setComponent] = useState('');
  const [eventType, setEventType] = useState('');
  const [programRunId, setProgramRunId] = useState('');
  const [operationId, setOperationId] = useState('');
  const [currentOnly, setCurrentOnly] = useState(true);
  const newestTimestampNs = events.reduce(
    (latest, event) => Math.max(latest, event.timestampNs),
    0,
  );
  const windowDurationNs = timeWindow === 'minute'
    ? 60e9
    : timeWindow === 'five_minutes'
      ? 300e9
      : timeWindow === 'hour'
        ? 3_600e9
        : null;
  const startNs = windowDurationNs === null || newestTimestampNs === 0
    ? undefined
    : Math.max(0, newestTimestampNs - windowDurationNs);
  const query = (): HistorianListQuery => ({
    ...(startNs === undefined ? {} : { startNs }),
    ...(component.trim() ? { components: [component.trim()] } : {}),
    ...(eventType.trim() ? { eventTypes: [eventType.trim()] } : {}),
    limit: 64,
  });
  const filtered = useMemo(
    () => filterExecutionHistoryEvents(events, {
      severity,
      alarmView,
      eventType: eventType.trim(),
      channel: component.trim(),
      search,
      executionId: currentOnly ? currentExecutionId : undefined,
      startNs,
      programRunId: programRunId.trim(),
      operationId: operationId.trim(),
    }),
    [
      alarmView,
      component,
      currentExecutionId,
      currentOnly,
      eventType,
      events,
      operationId,
      programRunId,
      search,
      severity,
      startNs,
    ],
  );
  if (!open) return null;

  return (
    <section
      className={`history-panel${embedded ? ' history-panel-embedded' : ''}`}
      aria-labelledby="history-title"
    >
      <header>
        <div>
          <p className="eyebrow">Authoritative event projection</p>
          <h2 id="history-title">Runtime history</h2>
        </div>
        <div className="button-row">
          {onReload && <button type="button" onClick={() => onReload(query())}>Refresh</button>}
          {!embedded && <button type="button" onClick={onClose}>Close</button>}
        </div>
      </header>
      <div className="history-filters">
        <label>
          <span>Time</span>
          <select
            value={timeWindow}
            onChange={(event) => setTimeWindow(event.target.value as typeof timeWindow)}
          >
            <option value="all">All loaded time</option>
            <option value="minute">Latest minute</option>
            <option value="five_minutes">Latest 5 minutes</option>
            <option value="hour">Latest hour</option>
          </select>
        </label>
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
          <span>Component</span>
          <input
            value={component}
            onChange={(event) => setComponent(event.target.value)}
            placeholder="manyforge.runtime"
          />
        </label>
        <label>
          <span>Event type</span>
          <input
            value={eventType}
            onChange={(event) => setEventType(event.target.value)}
            placeholder="operation_completed"
          />
        </label>
        <label>
          <span>Program run</span>
          <input
            value={programRunId}
            onChange={(event) => setProgramRunId(event.target.value)}
            placeholder="program-run…"
          />
        </label>
        <label>
          <span>Operation</span>
          <input
            value={operationId}
            onChange={(event) => setOperationId(event.target.value)}
            placeholder="operation…"
          />
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
        {onReload && (
          <button type="button" onClick={() => onReload(query())}>Apply filters</button>
        )}
      </div>
      <div className="history-events">
        {loading && <p className="muted">Loading history…</p>}
        {error && <p className="error-state" role="alert">{error}</p>}
        {!loading && !error && filtered.length === 0 && (
          <p className="muted">No events match these filters.</p>
        )}
        {filtered.map((event) => (
          <button
            type="button"
            key={event.id}
            className={`history-event severity-${historySeverityGroup(event.severity)}${
              selectedDetail?.sequence === event.sequence ? ' selected' : ''
            }`}
            onClick={() => onSelectEvent?.(event)}
          >
            <header>
              <strong>{event.message || event.type}</strong>
              <time>{eventTime(event)}</time>
            </header>
            <small>{event.channel} · {event.type}</small>
          </button>
        ))}
        {hasOlder && onLoadOlder && (
          <button type="button" disabled={loading} onClick={onLoadOlder}>
            Load older events
          </button>
        )}
      </div>
      {(detailLoading || detailError !== null || selectedDetail !== null) && (
        <section className="history-detail" aria-label="Selected history event detail">
          {detailLoading && <p className="muted">Loading exact event detail…</p>}
          {detailError && <p className="error-state" role="alert">{detailError}</p>}
          {selectedDetail && (
            <>
              <header>
                <strong>{selectedDetail.event.eventType.replaceAll('_', ' ')}</strong>
                <code>sequence {selectedDetail.sequence}</code>
              </header>
              <div className="history-detail-sections">
                {Object.entries(selectedDetail.sections).map(([name, section]) => (
                  <details key={name} open={section.available}>
                    <summary>{name.replaceAll(/([A-Z])/g, ' $1')}</summary>
                    {section.available
                      ? <pre>{JSON.stringify(section.value, null, 2)}</pre>
                      : <p className="muted">{section.reason}</p>}
                  </details>
                ))}
              </div>
            </>
          )}
        </section>
      )}
    </section>
  );
}
