import { useEffect, useRef, useState } from 'react';

import { api, HISTORIAN_PAGE_LIMIT } from '../api/client';
import { HistoryDiscovery } from './HistoryDiscovery';
import type { HistorianListQuery } from '../api/client';
import {
  fullId, isApplicationStartId, parseIdFilter, type ApplicationStartId, type DisplayIdKind,
} from '../api/applicationIdentity';
import type { HistorianDetail, HistorianRetention } from '../types/headless';
import { parseProgramRunEnd } from '../api/parsers/components';
import { runEndPresentation, utcText, type RunEndPresentation } from '../state/componentPresentation';
import { copyToClipboard } from '../utils/clipboard';
import type { NoticeHistoryRef } from '../state/workbenchNotices';

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
/** Follow reads the newest page this long after the previous read ended. */
export const HISTORY_FOLLOW_INTERVAL_MS = 2_000;

/** A recorded UTC time with date and milliseconds: `2026-11-24 14:02:03.123 UTC`. */
export function historyUtcText(utcNs: string): string {
  return utcText(utcNs);
}

/** Ids an event names, by payload key, that a History filter takes. */
export type HistoryIdKey =
  | 'application_start_id' | 'program_run_id' | 'operation_id' | 'node_attempt_id'
  | 'stop_event_id' | 'node_instance_id' | 'action_span_id' | 'intervention_session_id'
  | 'rule_id' | 'incident_id' | 'condition_id';

const HISTORY_ID_LABELS: Readonly<Record<HistoryIdKey, string>> = {
  application_start_id: 'Start',
  program_run_id: 'Program run',
  operation_id: 'Operation',
  node_attempt_id: 'Node attempt',
  stop_event_id: 'Stop',
  node_instance_id: 'Node instance',
  action_span_id: 'Action span',
  intervention_session_id: 'Intervention',
  rule_id: 'Rule',
  incident_id: 'Incident',
  condition_id: 'Condition',
};

const NUMBERED_HISTORY_IDS: ReadonlySet<string> = new Set(
  ['program_run_id', 'operation_id', 'node_attempt_id', 'stop_event_id'],
);

/** The correlation filter each remaining id sets. */
const CORRELATION_ID_FILTERS = {
  stop_event_id: 'stopEventId',
  node_instance_id: 'nodeInstanceId',
  action_span_id: 'actionSpanId',
  intervention_session_id: 'interventionSessionId',
  rule_id: 'ruleId',
  incident_id: 'incidentId',
  condition_id: 'conditionId',
} as const;

export interface HistoryIdReference {
  key: HistoryIdKey;
  value: string;
}

function historyIdText(key: string, value: unknown): string | null {
  if (!Object.hasOwn(HISTORY_ID_LABELS, key)) return null;
  if (NUMBERED_HISTORY_IDS.has(key)) {
    return Number.isSafeInteger(value) && (value as number) > 0 ? String(value) : null;
  }
  if (key === 'application_start_id') return isApplicationStartId(value) ? value : null;
  return typeof value === 'string' && value !== '' && value.length <= 1024 ? value : null;
}

/** Every distinct filterable id anywhere in an event payload, in payload order. */
export function historyIdReferences(payload: unknown): HistoryIdReference[] {
  const found = new Map<string, HistoryIdReference>();
  const visit = (value: unknown) => {
    if (value === null || typeof value !== 'object') return;
    if (Array.isArray(value)) {
      value.forEach(visit);
      return;
    }
    for (const [key, item] of Object.entries(value)) {
      const text = historyIdText(key, item);
      if (text === null) visit(item);
      else found.set(`${key}\n${text}`, { key: key as HistoryIdKey, value: text });
    }
  };
  visit(payload);
  return [...found.values()];
}

/**
 * The selected event as the backend recorded it, for a developer: the detail's
 * event in its wire field names, with the row's sequence, start and recorded UTC.
 */
export function historyRawEvent(
  detail: HistorianDetail,
  row: ExecutionHistoryEventView | undefined,
): Record<string, unknown> {
  const { event } = detail;
  return {
    sequence: detail.sequence,
    ...(row?.applicationStartId === undefined ? {} : { application_start_id: row.applicationStartId }),
    ...(row?.timestampUtcNs == null ? {} : { occurred_at_utc_ns: row.timestampUtcNs }),
    ...(row?.clockOwner == null ? {} : { clock_owner: row.clockOwner }),
    event: {
      schema_version: event.schemaVersion,
      event_id: event.eventId,
      component: event.component,
      event_type: event.eventType,
      severity: event.severity,
      occurred_at_ns: event.occurredAtNs,
      occurred_at_wall: event.occurredAtWall,
      payload: event.payload,
    },
  };
}

/**
 * Follow: `read` now, then again HISTORY_FOLLOW_INTERVAL_MS after each read
 * ends (never two at once), while `active` and the browser page is visible.
 */
export function useHistoryFollow(active: boolean, read: () => Promise<void> | undefined): void {
  const latest = useRef(read);
  useEffect(() => {
    latest.current = read;
  });
  useEffect(() => {
    if (!active) return undefined;
    let stopped = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const tick = async () => {
      if (!document.hidden) await latest.current();
      if (!stopped) timer = setTimeout(() => { void tick(); }, HISTORY_FOLLOW_INTERVAL_MS);
    };
    void tick();
    return () => {
      stopped = true;
      clearTimeout(timer);
    };
  }, [active]);
}

/**
 * A run's rows in recorded-time order (sequence order is ingest order, and a
 * runner's batched records arrive late), each with the time since the row
 * before it. Rows without a recorded UTC time keep their place at the end.
 */
export function historyTimeline(
  events: readonly ExecutionHistoryEventView[],
): { event: ExecutionHistoryEventView; sincePreviousMs: number | null }[] {
  const timed = events.filter((event) => event.timestampUtcNs != null)
    .map((event) => ({ event, utcNs: BigInt(event.timestampUtcNs as string) }))
    .sort((left, right) => (left.utcNs < right.utcNs ? -1 : left.utcNs > right.utcNs ? 1
      : (left.event.sequence ?? 0) - (right.event.sequence ?? 0)));
  return [
    ...timed.map(({ event, utcNs }, index) => ({
      event,
      sincePreviousMs: index === 0 ? null : Number((utcNs - timed[index - 1].utcNs) / 1_000n) / 1_000,
    })),
    ...events.filter((event) => event.timestampUtcNs == null)
      .map((event) => ({ event, sincePreviousMs: null })),
  ];
}

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
  /** A node attempt number; it names an attempt of the selected (else the current) start. */
  nodeAttemptId?: string;
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
  const attemptText = (options.nodeAttemptId ?? '').trim();
  const attempt = attemptText === '' ? undefined
    : /^[1-9]\d{0,15}$/.test(attemptText) && Number.isSafeInteger(Number(attemptText)) ? Number(attemptText) : null;
  if (run === null || operation === null || attempt === null
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
    nodeAttemptId: attempt,
    applicationStartId: (run ?? operation)?.applicationStartId ?? startFilter ?? undefined,
    search: options.search.trim() || undefined,
  };
}

function eventTime(event: ExecutionHistoryEventView): string {
  if (event.timestampUtcNs != null) return historyUtcText(event.timestampUtcNs);
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
  /** Reads the newest page for Follow without replacing the open detail; settles when the read ends. */
  onFollow?: (query: HistorianListQuery) => Promise<void>;
  /** The Program run the shown page is filtered to, if one: the page also shows as its timeline. */
  timelineRunId?: number | null;
  onLoadOlder?: () => void;
  onLoadNewer?: () => void;
  hasOlder?: boolean;
  hasNewer?: boolean;
  selectedDetail?: HistorianDetail | null;
  detailLoading?: boolean;
  detailError?: string | null;
  onSelectEvent?: (event: ExecutionHistoryEventView) => void;
  /**
   * The run or operation filter the panel starts with (a Recent notice's
   * "see History"); its page is read once when shown. Remount to apply another.
   */
  preset?: NoticeHistoryRef;
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
  onFollow,
  timelineRunId = null,
  onLoadOlder,
  onLoadNewer,
  hasOlder = false,
  hasNewer = false,
  selectedDetail = null,
  detailLoading = false,
  detailError = null,
  onSelectEvent,
  preset,
}: ExecutionHistoryPanelProps) {
  const [severity, setSeverity] = useState<HistorySeverityFilter>('all');
  const [alarmView, setAlarmView] = useState<HistoryAlarmFilter>('all');
  const [search, setSearch] = useState('');
  const [timeWindow, setTimeWindow] = useState<'all' | 'minute' | 'five_minutes' | 'hour'>('all');
  const [component, setComponent] = useState('');
  const [eventType, setEventType] = useState('');
  const [programRunId, setProgramRunId] = useState(preset?.programRunId === undefined ? '' : String(preset.programRunId));
  const [operationId, setOperationId] = useState(preset?.operationId === undefined ? '' : String(preset.operationId));
  const [nodeAttemptId, setNodeAttemptId] = useState('');
  const [follow, setFollow] = useState(false);
  const [copied, setCopied] = useState<{ sequence: number; ok: boolean } | null>(null);
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
    severity, alarmView, timeWindow, component, eventType, search, operationId, nodeAttemptId,
    programRunId: currentOnly && currentProgramRunId ? String(currentProgramRunId) : programRunId,
    applicationStartId,
    startFilter,
    correlations,
  });
  const validSearch = search.trim() === '' || Array.from(search.trim()).length >= 3;
  // The start a run or operation filter names, else the current start: run
  // numbers, the run list and rows are read against it.
  const selectedStart = query?.applicationStartId ?? applicationStartId;
  useHistoryFollow(open && follow, () => (
    !loading && validSearch && query !== null ? onFollow?.(query) : undefined));
  const presetRead = useRef(preset === undefined);
  useEffect(() => {
    if (!open || presetRead.current || query === null) return;
    presetRead.current = true;
    onReload?.(query);
  });
  if (!open) return null;
  const selectedRow = selectedDetail === null ? undefined
    : events.find((event) => event.sequence === selectedDetail.sequence);
  const recordedEvent = selectedDetail === null ? ''
    : JSON.stringify(historyRawEvent(selectedDetail, selectedRow), null, 2);
  // An id of the selected row sets its filter, under the row's own start.
  const applyReference = ({ key, value }: HistoryIdReference) => {
    const rowStart = selectedRow?.applicationStartId ?? selectedStart;
    const otherStart = rowStart !== null && rowStart !== applicationStartId ? rowStart : null;
    const numbered = (kind: DisplayIdKind) => (
      otherStart === null ? value : fullId(otherStart, kind, Number(value)));
    if (key === 'application_start_id') {
      setStartFilter(value === applicationStartId ? null : value);
      return;
    }
    setStartFilter(otherStart);
    if (key === 'program_run_id') {
      setCurrentOnly(false);
      setProgramRunId(numbered('run'));
    } else if (key === 'operation_id') setOperationId(numbered('op'));
    else if (key === 'node_attempt_id') setNodeAttemptId(value);
    else {
      setCorrelations({
        ...correlations,
        [CORRELATION_ID_FILTERS[key]]: key === 'stop_event_id' ? numbered('stop') : value,
      });
    }
  };

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
          {onFollow && (
            <label className="history-follow-toggle" title="Read the newest page every 2 s while this tab is open.">
              <input type="checkbox" checked={follow} onChange={(event) => setFollow(event.target.checked)} />
              Follow
            </label>
          )}
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
          <span>Node attempt</span>
          <input
            value={nodeAttemptId}
            onChange={(event) => setNodeAttemptId(event.target.value)}
            placeholder="Attempt number"
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
            ['actionSpanId', 'Action span'], ['nodeInstanceId', 'Node instance'], ['stopEventId', 'Stop'],
            ['interventionSessionId', 'Intervention'], ['ruleId', 'Rule'],
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
        <p className="muted">Bounded local history, not a permanent archive. Important events, routine progress and Program node attempts have independent limits. Export evidence before it is pruned.</p>
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
        {timelineRunId !== null && events.length > 0 && (
          <details className="history-timeline">
            <summary>Run {timelineRunId} timeline · this page, oldest first</summary>
            <ol>
              {historyTimeline(events.slice(0, MAXIMUM_MOUNTED_HISTORY_ROWS)).map(({ event, sincePreviousMs }) => (
                <li key={event.id} className={`severity-${historySeverityGroup(event.severity)}`}>
                  <time title={event.timestampUtcNs == null ? undefined : historyUtcText(event.timestampUtcNs)}>
                    {event.timestampUtcNs == null ? 'time unknown' : historyUtcText(event.timestampUtcNs).slice(11, 23)}
                  </time>
                  <small title="Time since the row before">{sincePreviousMs === null ? '' : `+${(sincePreviousMs / 1000).toFixed(3)} s`}</small>
                  <small>{event.type.replaceAll('_', ' ')}</small>
                  <span>{event.message || event.type}</span>
                </li>
              ))}
            </ol>
          </details>
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
              onClick={() => {
                // An older page is not the newest one: reading it ends Follow.
                setFollow(false);
                onLoadOlder?.();
              }}
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
                <span className="button-row">
                  <code>sequence {selectedDetail.sequence}</code>
                  <button
                    type="button"
                    title="Copy the whole recorded event, with its ids, as JSON."
                    onClick={() => setCopied({
                      sequence: selectedDetail.sequence,
                      ok: copyToClipboard(recordedEvent),
                    })}
                  >{copied?.sequence !== selectedDetail.sequence ? 'Copy' : copied.ok ? 'Copied' : 'Copy failed'}</button>
                </span>
              </header>
              <HistoryRunEnd payload={selectedDetail.event.payload} />
              <HistoryReferences payload={selectedDetail.event.payload} onUse={applyReference} />
              <div className="history-detail-sections">
                <details open>
                  <summary>Recorded event</summary>
                  <pre>{recordedEvent}</pre>
                </details>
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

function HistoryReferences({ payload, onUse }: {
  payload: Record<string, unknown>;
  onUse: (reference: HistoryIdReference) => void;
}) {
  const references = historyIdReferences(payload);
  if (references.length === 0) return null;
  return (
    <p className="history-detail-references">
      <span className="muted">Set filter:</span>
      {references.map((reference) => {
        const label = HISTORY_ID_LABELS[reference.key];
        return (
          <button
            type="button"
            className="text-button"
            key={`${reference.key}\n${reference.value}`}
            title={`Set the ${label} filter to ${reference.value}; Apply filters reads it.`}
            onClick={() => onUse(reference)}
          >{label} {reference.key === 'application_start_id' ? reference.value.slice(0, 4) : reference.value}</button>
        );
      })}
    </p>
  );
}

function HistoryRunEnd({ payload }: { payload: Record<string, unknown> }) {
  const end = historyRunEnd(payload);
  if (end === null) return null;
  return <p className={`run-end tone-${end.tone}`} role="status"><strong>{end.label}</strong> · {end.text}</p>;
}
