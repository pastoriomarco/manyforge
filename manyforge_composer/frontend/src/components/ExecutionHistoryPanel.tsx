import { useState } from 'react';

import { api, HISTORIAN_PAGE_LIMIT } from '../api/client';
import { HistoryDiscovery } from './HistoryDiscovery';
import type { HistorianListQuery } from '../api/client';
import type { HistorianDetail, HistorianRetention } from '../types/headless';

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
  timestampNs: string;
  timestamp?: string | null;
  timestampUtcNs?: string | null;
  clockOwner?: string | null;
  wallClockRegressed?: boolean;
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

const MAXIMUM_MOUNTED_HISTORY_ROWS = 32;

export function historySeverityGroup(
  severity: string | null | undefined,
): Exclude<HistorySeverityFilter, 'all'> {
  const normalized = (severity ?? 'INFO').toUpperCase();
  if (normalized === 'ERROR' || normalized === 'CRITICAL') return 'error';
  if (normalized === 'WARN' || normalized === 'WARNING') return 'warning';
  return 'info';
}

export function historyFilterQuery(options: {
  severity: HistorySeverityFilter;
  alarmView: HistoryAlarmFilter;
  timeWindow: 'all' | 'minute' | 'five_minutes' | 'hour';
  component: string;
  eventType: string;
  search: string;
  programRunId: string;
  operationId: string;
  correlations?: Partial<HistorianListQuery>;
}): HistorianListQuery {
  return {
    ...options.correlations,
    limit: HISTORIAN_PAGE_LIMIT,
    recentSeconds: { all: undefined, minute: 60, five_minutes: 300, hour: 3600 }[options.timeWindow],
    severities: { all: undefined, info: ['INFO', 'DEBUG'], warning: ['WARN'], error: ['ERROR', 'CRITICAL'] }[options.severity],
    alarmPhase: { all: undefined, lifecycle: 'all', activation: 'activated', return: 'returned', acknowledgement: 'acknowledged', raw: 'raw' }[options.alarmView] as HistorianListQuery['alarmPhase'],
    components: options.component.trim() ? [options.component.trim()] : undefined,
    eventTypes: options.eventType.trim() ? [options.eventType.trim()] : undefined,
    programRunId: options.programRunId.trim() || undefined,
    operationId: options.operationId.trim() || undefined,
    search: options.search.trim() || undefined,
  };
}

function eventTime(event: ExecutionHistoryEventView): string {
  if (event.timestampUtcNs != null) {
    return new Date(Number(BigInt(event.timestampUtcNs) / 1_000_000n)).toLocaleTimeString();
  }
  return event.clockOwner
    ? `UTC unavailable · ${event.clockOwner}: ${event.timestampNs} ns`
    : 'Recorded UTC unavailable';
}

interface ExecutionHistoryPanelProps {
  open: boolean;
  onClose: () => void;
  events: ExecutionHistoryEventView[];
  eyebrow?: string;
  loading?: boolean;
  error?: string | null;
  retention?: HistorianRetention | null;
  currentProgramRunId?: string | null;
  embedded?: boolean;
  onReload?: (query: HistorianListQuery) => void;
  onLoadOlder?: () => void;
  onLoadNewer?: () => void;
  hasOlder?: boolean;
  hasNewer?: boolean;
  selectedDetail?: HistorianDetail | null;
  detailLoading?: boolean;
  detailError?: string | null;
  onSelectEvent?: (event: ExecutionHistoryEventView) => void;
}

/**
 * Only a bounded, already-filtered backend page is rendered. Filters never
 * scan the browser's current page or turn unrelated owner clocks into UTC.
 */
export function ExecutionHistoryPanel({
  open,
  onClose,
  events,
  eyebrow = 'Authoritative event projection',
  loading = false,
  error = null,
  retention = null,
  currentProgramRunId,
  embedded = false,
  onReload,
  onLoadOlder,
  onLoadNewer,
  hasOlder = false,
  hasNewer = false,
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
  const [currentOnly, setCurrentOnly] = useState(false);
  const [correlations, setCorrelations] = useState<Partial<HistorianListQuery>>({});
  const query = (): HistorianListQuery => historyFilterQuery({
    severity, alarmView, timeWindow, component, eventType, search, operationId,
    programRunId: currentOnly && currentProgramRunId ? currentProgramRunId : programRunId,
    correlations,
  });
  const validSearch = search.trim() === '' || Array.from(search.trim()).length >= 3;
  if (!open) return null;

  return (
    <section
      className={`history-panel${embedded ? ' history-panel-embedded' : ''}`}
      aria-labelledby="history-title"
    >
      <header>
        <div>
          <p className="eyebrow">{eyebrow}</p>
          <h2 id="history-title">Runtime history</h2>
        </div>
        <div className="button-row">
          {onReload && <button type="button" disabled={loading || !validSearch} onClick={() => onReload(query())}>Refresh</button>}
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
            <option value="all">All retained time</option>
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
            disabled={currentOnly && Boolean(currentProgramRunId)}
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
          <input value={search} maxLength={256} placeholder="At least 3 characters" onChange={(event) => setSearch(event.target.value)} />
        </label>
        <label className="history-current-toggle">
          <input
            type="checkbox"
            checked={currentOnly}
            disabled={!currentProgramRunId}
            onChange={(event) => setCurrentOnly(event.target.checked)}
          />
          Current Program run
        </label>
        {onReload && (
          <button type="button" disabled={loading || !validSearch} onClick={() => onReload(query())}>Apply filters</button>
        )}
      </div>
      <HistoryDiscovery onSelect={(kind, value) => {
        if (kind === 'program_run_id') { setCurrentOnly(false); setProgramRunId(value); }
        else if (kind === 'component') setComponent(value);
        else if (kind === 'event_type') setEventType(value);
        else setCorrelations({ ...correlations, channel: value });
      }} />
      {validSearch && <p>
        <a href={api.history.exportUrl(query())} download="manyforge-debug.json">Export filtered diagnostic bundle</a>
        {' · '}<a href={api.history.exportUrl(query(), true)} download="manyforge-trace.json">Normalized trace</a>
        {' '}<span className="muted">Up to 256 retained events; coverage and continuation are included.</span>
      </p>}
      <details className="history-correlation-filters">
        <summary>Exact causal references</summary>
        <div className="history-filters">
          {([
            ['channel', 'Exact component / channel'],
            ['actionSpanId', 'Action span'], ['nodeInstanceId', 'Node instance'],
            ['interventionSessionId', 'Intervention'], ['contactPolicyId', 'Contact policy'],
            ['incidentId', 'Incident'], ['conditionId', 'Condition'],
          ] as const).map(([key, label]) => (
            <label key={key}><span>{label}</span>
              <input value={correlations[key] ?? ''} maxLength={1024}
                onChange={(event) => setCorrelations({ ...correlations, [key]: event.target.value.trim() || undefined })} />
            </label>
          ))}
        </div>
      </details>
      {retention && <details className="history-retention">
        <summary>Retention and recorded-time coverage</summary>
        <p className="muted">Bounded local history, not a permanent archive. Important events and routine progress have independent limits. Export evidence before it is pruned.</p>
        {!retention.priorPruningKnown && <p className="muted">Legacy history did not retain pruning counts; coverage before this migration is unknown.</p>}
        {retention.partitions.map((part) => <p key={part.partition}>
          <strong>{part.partition}</strong>: {part.retainedCount} retained; {part.prunedCount} pruned{part.lossCounterSaturated ? ' (at least)' : ''}.
          {' '}Recorded UTC: {part.firstRecordedUtcNs === null ? 'unavailable' : new Date(Number(BigInt(part.firstRecordedUtcNs) / 1_000_000n)).toISOString()}
          {' → '}{part.lastRecordedUtcNs === null ? 'unavailable' : new Date(Number(BigInt(part.lastRecordedUtcNs) / 1_000_000n)).toISOString()}.
          {' '}{part.unknownTimeCount} unknown times; {part.regressedTimeCount} clock regressions.
        </p>)}
        <p>
          <strong>failure captures</strong>: {retention.captures.liveCount} of {retention.captures.liveCaptureLimit} retained
          {' '}protecting {retention.captures.protectedCount} events ({retention.captures.protectedBytes} of {retention.captures.protectedByteLimit} bytes);
          {' '}{retention.captures.retiredCount} retired, releasing {retention.captures.releasedRows} events to ordinary pruning.
        </p>
      </details>}
      <div className="history-events">
        {loading && <p className="muted">Loading history…</p>}
        {error && <p className="error-state" role="alert">{error}</p>}
        {!loading && !error && events.length === 0 && (
          <p className="muted">No events match these filters.</p>
        )}
        {events.slice(0, MAXIMUM_MOUNTED_HISTORY_ROWS).map((event) => (
          <button
            type="button"
            key={event.id}
            disabled={loading}
            className={`history-event severity-${historySeverityGroup(event.severity)}${
              event.sequence !== undefined
                && selectedDetail !== null
                && selectedDetail.sequence === event.sequence ? ' selected' : ''
            }`}
            onClick={() => onSelectEvent?.(event)}
          >
            <header>
              <strong>{event.message || event.type}</strong>
              <time title={event.timestamp ? `Original recorded wall time: ${event.timestamp}` : undefined}>{eventTime(event)}{event.wallClockRegressed ? ' · clock moved backward' : ''}</time>
            </header>
            <small>{event.channel} · {event.type}</small>
          </button>
        ))}
        {(hasNewer || hasOlder) && (
          <nav className="history-page-navigation" aria-label="History pages">
            <button
              type="button"
              disabled={loading || !hasNewer}
              onClick={onLoadNewer}
            >Newer</button>
            <button
              type="button"
              disabled={loading || !hasOlder}
              onClick={onLoadOlder}
            >Older</button>
          </nav>
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
