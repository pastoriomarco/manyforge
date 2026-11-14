import { useEffect, useState } from 'react';

import { api, HISTORIAN_PAGE_LIMIT } from '../api/client';
import { HistoryDiscovery } from './HistoryDiscovery';
import type { HistorianListQuery } from '../api/client';
import { fullId, parseIdFilter, type ApplicationStartId, type DisplayIdKind } from '../api/applicationIdentity';
import type { HistorianDetail, HistorianRetention } from '../types/headless';
import { parseProgramRunEnd } from '../api/parsers/components';
import { runEndPresentation, type RunEndPresentation } from '../state/componentPresentation';

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
  programRunId?: number | null;
  operationId?: number | null;
  /** The application start the row belongs to. */
  applicationStartId?: ApplicationStartId;
  type: string;
  alarmPhase?: HistoryAlarmPhase;
  details?: unknown;
}

const MAXIMUM_MOUNTED_HISTORY_ROWS = 32;

/** The run end a Program run's end event carries, if this is one. */
export function historyRunEnd(payload: Record<string, unknown>): RunEndPresentation | null {
  if (payload.run_end === undefined || payload.run_end === null) return null;
  return runEndPresentation(parseProgramRunEnd(payload.run_end, 'history.run_end'));
}

export function historySeverityGroup(
  severity: string | null | undefined,
): Exclude<HistorySeverityFilter, 'all'> {
  const normalized = (severity ?? 'INFO').toUpperCase();
  if (normalized === 'ERROR' || normalized === 'CRITICAL') return 'error';
  if (normalized === 'WARN' || normalized === 'WARNING') return 'warning';
  return 'info';
}

/**
 * A typed run or operation filter: the display form (`3f9a/op7`), the full
 * form (`<16 hex>/op7`) or the bare number. The full form names its own
 * start; the other two name the current start, so a display prefix of
 * another start names nothing. Null for text that names nothing.
 */
function historyIdFilter(
  text: string,
  kind: DisplayIdKind,
  current: ApplicationStartId | null,
): { value: number; applicationStartId: ApplicationStartId } | null {
  const parsed = parseIdFilter(text, kind);
  if (parsed === null) return null;
  if (parsed.applicationStartId !== null) {
    return { value: parsed.value, applicationStartId: parsed.applicationStartId };
  }
  if (current === null
    || (parsed.applicationStartPrefix !== null && !current.startsWith(parsed.applicationStartPrefix))) return null;
  return { value: parsed.value, applicationStartId: current };
}

/**
 * The start selector's entries: the current start first, then the earlier
 * starts that have History rows, newest first (as discovery lists them).
 * An earlier start is labelled with the recorded UTC time of its last event
 * once that row is loaded (`lastEventUtcNs`), else with its 4-hex prefix.
 */
export function historyStartOptions(
  current: ApplicationStartId | null,
  discovered: readonly string[],
  lastEventUtcNs: Readonly<Record<string, string | null>> = {},
): { value: ApplicationStartId | ''; label: string; title?: string }[] {
  const earlier = discovered.filter((start) => start !== current);
  return [
    { value: '', label: 'Current start', title: current ?? undefined },
    ...earlier.map((start, index) => {
      const last = lastEventUtcNs[start];
      const label = last == null
        ? `Earlier start ${start.slice(0, 4)}`
        : `Earlier start, last event ${new Date(Number(BigInt(last) / 1_000_000n)).toLocaleString()}`;
      return { value: start, label: `${label}${index === 0 ? ' (previous start)' : ''}`, title: start };
    }),
  ];
}

/** The backend query, or null when a run or operation filter names nothing. */
export function historyFilterQuery(options: {
  severity: HistorySeverityFilter;
  alarmView: HistoryAlarmFilter;
  timeWindow: 'all' | 'minute' | 'five_minutes' | 'hour';
  component: string;
  eventType: string;
  search: string;
  programRunId: string;
  operationId: string;
  /** The current start, for the display form and the bare number. */
  applicationStartId: ApplicationStartId | null;
  /** The start chosen in the start selector; null is the current start. */
  startFilter?: ApplicationStartId | null;
  correlations?: Partial<HistorianListQuery>;
}): HistorianListQuery | null {
  const startFilter = options.startFilter ?? null;
  const base = startFilter ?? options.applicationStartId;
  const run = options.programRunId.trim() === ''
    ? undefined : historyIdFilter(options.programRunId, 'run', base);
  const operation = options.operationId.trim() === ''
    ? undefined : historyIdFilter(options.operationId, 'op', base);
  if (run === null || operation === null
    || (run !== undefined && operation !== undefined
      && run.applicationStartId !== operation.applicationStartId)
    || (startFilter !== null && (run ?? operation) !== undefined
      && (run ?? operation)!.applicationStartId !== startFilter)) return null;
  return {
    ...options.correlations,
    limit: HISTORIAN_PAGE_LIMIT,
    recentSeconds: { all: undefined, minute: 60, five_minutes: 300, hour: 3600 }[options.timeWindow],
    severities: { all: undefined, info: ['INFO', 'DEBUG'], warning: ['WARN'], error: ['ERROR', 'CRITICAL'] }[options.severity],
    alarmPhase: { all: undefined, lifecycle: 'all', activation: 'activated', return: 'returned', acknowledgement: 'acknowledged', raw: 'raw' }[options.alarmView] as HistorianListQuery['alarmPhase'],
    components: options.component.trim() ? [options.component.trim()] : undefined,
    eventTypes: options.eventType.trim() ? [options.eventType.trim()] : undefined,
    programRunId: run?.value,
    operationId: operation?.value,
    applicationStartId: (run ?? operation)?.applicationStartId ?? startFilter ?? undefined,
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
  currentProgramRunId?: number | null;
  /** The current application-start id (`A`); run and operation filters need it. */
  applicationStartId?: ApplicationStartId | null;
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
  applicationStartId = null,
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
  const [startFilter, setStartFilter] = useState<ApplicationStartId | null>(null);
  const [earlierStarts, setEarlierStarts] = useState<string[]>([]);
  const [startsError, setStartsError] = useState<string | null>(null);
  const [lastEventUtcNs, setLastEventUtcNs] = useState<Record<string, string | null>>({});
  // The starts with History rows, one bounded page, read when the page opens;
  // then each listed start's last event (one row), one query at a time. A
  // failed label read leaves that start's fallback label.
  useEffect(() => {
    if (!open) return undefined;
    const request = new AbortController();
    void (async () => {
      let starts: string[];
      try {
        starts = (await api.history.discovery('application_start_id', '', request.signal)).values;
      } catch (reason) {
        if (!request.signal.aborted) setStartsError(reason instanceof Error ? reason.message : String(reason));
        return;
      }
      if (request.signal.aborted) return;
      setEarlierStarts(starts);
      setStartsError(null);
      for (const start of starts) {
        if (start === applicationStartId) continue;
        try {
          const page = await api.history.list({ applicationStartId: start, limit: 1 }, request.signal);
          if (request.signal.aborted) return;
          const last = page.events[0]?.event.occurredAtUtcNs ?? null;
          setLastEventUtcNs((known) => ({ ...known, [start]: last }));
        } catch {
          if (request.signal.aborted) return;
        }
      }
    })();
    return () => request.abort();
  }, [open, applicationStartId]);
  const query = historyFilterQuery({
    severity, alarmView, timeWindow, component, eventType, search, operationId,
    programRunId: currentOnly && currentProgramRunId ? String(currentProgramRunId) : programRunId,
    applicationStartId,
    startFilter,
    correlations,
  });
  const validSearch = search.trim() === '' || Array.from(search.trim()).length >= 3;
  // The start a run or operation filter names, else the current start: run
  // numbers, the run list and rows are read against it.
  const selectedStart = query?.applicationStartId ?? applicationStartId;
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
          {onReload && <button type="button" disabled={loading || !validSearch || query === null} onClick={() => { if (query !== null) onReload(query); }}>Refresh</button>}
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
          <span>Start</span>
          <select
            value={startFilter ?? ''}
            title={startFilter ?? applicationStartId ?? undefined}
            onChange={(event) => setStartFilter(event.target.value === '' ? null : event.target.value)}
          >
            {historyStartOptions(applicationStartId, earlierStarts, lastEventUtcNs).map((option) => (
              <option key={option.value} value={option.value} title={option.title}>{option.label}</option>
            ))}
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
            placeholder="Run number"
          />
        </label>
        <label>
          <span>Operation</span>
          <input
            value={operationId}
            onChange={(event) => setOperationId(event.target.value)}
            placeholder="Operation number"
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
          <button type="button" disabled={loading || !validSearch || query === null} onClick={() => { if (query !== null) onReload(query); }}>Apply filters</button>
        )}
      </div>
      <HistoryDiscovery key={selectedStart ?? ''} applicationStartId={selectedStart} onSelect={(kind, value) => {
        if (kind === 'program_run_id') {
          setCurrentOnly(false);
          setProgramRunId(selectedStart !== null && selectedStart !== (startFilter ?? applicationStartId)
            ? fullId(selectedStart, 'run', Number(value)) : value);
        }
        else if (kind === 'component') setComponent(value);
        else if (kind === 'event_type') setEventType(value);
        else setCorrelations({ ...correlations, channel: value });
      }} />
      {startsError && <p role="alert" className="error-state notice severity-error">{startsError}</p>}
      {validSearch && query !== null && <p>
        <a href={api.history.exportUrl(query)} download="manyforge-debug.json">Export filtered diagnostic bundle</a>
        {' · '}<a href={api.history.exportUrl(query, true)} download="manyforge-trace.json">Normalized trace</a>
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
        {retention.olderHistoryFiles.map((file) => <p key={file.fileName} className="muted">
          Earlier history{file.format === null ? '' : ` (format ${file.format})`} is in <code>{file.fileName}</code> and is not shown here.
        </p>)}
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
        {error && <p className="error-state notice severity-error" role="alert">{error}</p>}
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
            <small>
              {event.channel} · {event.type}
              {event.applicationStartId !== undefined && event.applicationStartId !== selectedStart
                && <> · start {event.applicationStartId.slice(0, 4)}</>}
            </small>
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
          {detailError && <p className="error-state notice severity-error" role="alert">{detailError}</p>}
          {selectedDetail && (
            <>
              <header>
                <strong>{selectedDetail.event.eventType.replaceAll('_', ' ')}</strong>
                <code>sequence {selectedDetail.sequence}</code>
              </header>
              <HistoryRunEnd payload={selectedDetail.event.payload} />
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

function HistoryRunEnd({ payload }: { payload: Record<string, unknown> }) {
  const end = historyRunEnd(payload);
  if (end === null) return null;
  return <p className={`run-end tone-${end.tone}`} role="status"><strong>{end.label}</strong> · {end.text}</p>;
}
