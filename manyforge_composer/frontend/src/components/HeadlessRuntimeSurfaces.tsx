import { useEffect, useMemo, useRef, useState } from 'react';
import { api, HISTORIAN_PAGE_LIMIT } from '../api/client';
import type { HistorianListQuery } from '../api/client';
import { useShellHeadlessState } from '../hooks/useHeadlessState';
import type {
  HeadlessStateResult,
  HeadlessStateSource,
} from '../hooks/useHeadlessState';
import type {
  AlarmLifecycleState,
  AlarmPriority,
  HistorianStoredEvent,
  HistorianDetail,
  OperatorAlarmRecord,
} from '../types/headless';
import {
  acknowledgementFence,
  acknowledgementFenceMatches,
} from '../utils/alarmFences';
import type { AlarmAcknowledgementFence } from '../utils/alarmFences';
import { OperatorAlarmPanel } from './OperatorAlarmPanel';
import type { OperatorAlarmView } from './OperatorAlarmPanel';
import { ExecutionHistoryPanel } from './ExecutionHistoryPanel';
import type {
  ExecutionHistoryEventView,
  HistoryAlarmPhase,
} from './ExecutionHistoryPanel';
import { KernelErrorPanel } from './KernelErrorPanel';
import { useOperationalErrorMirror } from '../state/operationalErrors';
import { OverlayPanel } from './OverlayPanel';

export interface ClientOperationalError {
  id: string;
  title: string;
  message: string;
  active: boolean;
  acknowledgementRequired: boolean;
}


function lifecycleLabel(state: AlarmLifecycleState): string {
  switch (state) {
    case 'active_unacknowledged':
      return 'ACTIVE — ACKNOWLEDGEMENT REQUIRED';
    case 'active_acknowledged':
      return 'ACTIVE — ACKNOWLEDGED';
    case 'returned_unacknowledged':
      return 'RESOLVED — ACKNOWLEDGEMENT REQUIRED';
    case 'returned_acknowledged':
      return 'RESOLVED — ACKNOWLEDGED';
  }
}

function alarmSeverity(priority: AlarmPriority): OperatorAlarmView['severity'] {
  if (priority === 'critical') return 'critical';
  if (priority === 'high') return 'error';
  return 'warning';
}

export function attentionAlarmsNewestFirst(
  alarms: OperatorAlarmRecord[],
): OperatorAlarmRecord[] {
  return alarms
    .filter((alarm) =>
      alarm.lifecycleState === 'active_unacknowledged'
      || alarm.lifecycleState === 'returned_unacknowledged')
    .sort((left, right) =>
      right.activatedAtNs - left.activatedAtNs
      || left.conditionId.localeCompare(right.conditionId));
}

export function operatorAlarmView(alarm: OperatorAlarmRecord): OperatorAlarmView {
  return {
    conditionId: alarm.conditionId,
    activationId: alarm.activationId,
    recordRevision: alarm.recordRevision,
    definitionId: alarm.definitionId,
    lifecycleLabel: lifecycleLabel(alarm.lifecycleState),
    priorityLabel: `${alarm.priority.toUpperCase()} PRIORITY`,
    severity: alarmSeverity(alarm.priority),
    title: alarm.title,
    message: alarm.message,
    operatorResponse: alarm.operatorResponse,
    details: {
      ...alarm.details,
      alarm_class: alarm.alarmClass,
      occurrence_count: alarm.occurrenceCount,
      consequence: alarm.consequence,
      subject: alarm.subject,
      requires_motion_attention: alarm.requiresMotionAttention,
    },
  };
}

function alarmPhase(eventType: string): HistoryAlarmPhase {
  if (eventType === 'operator_alarm_activated') return 'activation';
  if (eventType === 'operator_alarm_returned') return 'return';
  if (eventType === 'operator_alarm_acknowledged') return 'acknowledgement';
  return null;
}

const PROGRAM_RUN_ID_FIELDS = new Set(['program_run_id', 'programRunId']);
const OPERATION_ID_FIELDS = new Set(['operation_id', 'operationId']);

export function nestedHistorianIdentity(
  value: unknown,
  names: ReadonlySet<string>,
): string | null {
  const pending: Array<{ value: unknown; depth: number }> = [{ value, depth: 0 }];
  let visited = 0;
  while (pending.length > 0 && visited < 2_048) {
    const current = pending.pop();
    if (current === undefined) break;
    visited += 1;
    if (current.depth > 12 || current.value === null || typeof current.value !== 'object') {
      continue;
    }
    if (Array.isArray(current.value)) {
      for (const child of current.value) {
        pending.push({ value: child, depth: current.depth + 1 });
      }
      continue;
    }
    for (const [name, child] of Object.entries(current.value as Record<string, unknown>)) {
      if (names.has(name) && typeof child === 'string' && child.trim()) return child;
      pending.push({ value: child, depth: current.depth + 1 });
    }
  }
  return null;
}

export function historianEventView(
  stored: HistorianStoredEvent,
): ExecutionHistoryEventView {
  const readableType = stored.event.eventType.replaceAll('_', ' ');
  const conditionId = stored.event.payload.condition_id;
  return {
    id: `${stored.sequence}:${stored.event.eventId}`,
    sequence: stored.sequence,
    timestampNs: stored.event.occurredAtNs,
    timestamp: stored.event.occurredAtWall,
    severity: stored.event.severity,
    channel: stored.event.component,
    message: typeof conditionId === 'string'
      ? `${readableType}: ${conditionId}`
      : readableType,
    type: stored.event.eventType,
    alarmPhase: alarmPhase(stored.event.eventType),
    programRunId: nestedHistorianIdentity(
      stored.event.payload,
      PROGRAM_RUN_ID_FIELDS,
    ),
    operationId: nestedHistorianIdentity(
      stored.event.payload,
      OPERATION_ID_FIELDS,
    ),
    details: stored.event.payload,
  };
}

function actionErrorMessage(reason: unknown): string {
  return reason instanceof Error && reason.message.trim()
    ? reason.message
    : 'Headless request failed.';
}

function requestId(): string {
  return globalThis.crypto?.randomUUID?.()
    ?? `composer-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

interface HeadlessRuntimeSurfacesProps {
  enabled: boolean;
  headlessStateSource: HeadlessStateSource;
  clientErrors?: ClientOperationalError[];
  clientHistory?: ExecutionHistoryEventView[];
  onAcknowledgeClientError?: (id: string) => void;
  attentionRequest?: string | null;
  expanded?: boolean;
  onExpandedChange?: (expanded: boolean) => void;
  onClose?: () => void;
}

interface HeadlessRuntimeSurfacesViewProps {
  enabled: boolean;
  headlessState: HeadlessStateResult;
  clientErrors?: ClientOperationalError[];
  clientHistory?: ExecutionHistoryEventView[];
  onAcknowledgeClientError?: (id: string) => void;
  attentionRequest?: string | null;
  expanded?: boolean;
  onExpandedChange?: (expanded: boolean) => void;
  onClose?: () => void;
}

interface RuntimeAttentionFence {
  alarmFence: AlarmAcknowledgementFence | null;
  clientErrorIds: string[];
}

function sameIdentities(left: string[], right: string[]): boolean {
  if (left.length !== right.length) return false;
  const expected = [...left].sort();
  const current = [...right].sort();
  return expected.every((value, index) => value === current[index]);
}

export function runtimeAttentionSummary(
  alarms: OperatorAlarmRecord[],
  clientErrors: ClientOperationalError[],
): string {
  const count = alarms.length + clientErrors.length;
  if (count === 0) return 'No messages require attention';
  if (count > 1) return `${count} messages require attention`;
  return clientErrors[0]?.title ?? alarms[0]?.title ?? 'Attention required';
}

export function HeadlessRuntimeSurfacesView({
  enabled,
  headlessState,
  clientErrors = [],
  clientHistory = [],
  onAcknowledgeClientError,
  attentionRequest = null,
  expanded = true,
  onExpandedChange,
  onClose,
}: HeadlessRuntimeSurfacesViewProps) {
  const { snapshot, error, reconnect } = headlessState;
  const [busyActivation, setBusyActivation] = useState<string | null>(null);
  const [bulkBusy, setBulkBusy] = useState(false);
  const [bulkFence, setBulkFence] = useState<RuntimeAttentionFence | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [mode, setMode] = useState<'attention' | 'active' | 'history'>('attention');
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState<string | null>(null);
  const [historyEvents, setHistoryEvents] = useState<ExecutionHistoryEventView[]>([]);
  const [historyQuery, setHistoryQuery] = useState<HistorianListQuery>({
    limit: HISTORIAN_PAGE_LIMIT,
  });
  const [historyNextBefore, setHistoryNextBefore] = useState<number | null>(null);
  const [historyHasOlder, setHistoryHasOlder] = useState(false);
  const [historyDetail, setHistoryDetail] = useState<HistorianDetail | null>(null);
  const [historyDetailLoading, setHistoryDetailLoading] = useState(false);
  const [historyDetailError, setHistoryDetailError] = useState<string | null>(null);
  const acknowledgeAllButton = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (attentionRequest !== null) setMode('attention');
  }, [attentionRequest]);
  useOperationalErrorMirror({
    source: 'composer.runtime_history',
    title: 'Runtime history request failed',
    message: historyError,
    onDismiss: () => setHistoryError(null),
  });
  useOperationalErrorMirror({
    source: 'composer.runtime_history_detail',
    title: 'Runtime history detail request failed',
    message: historyDetailError,
    onDismiss: () => setHistoryDetailError(null),
  });
  useOperationalErrorMirror({
    source: 'composer.alarm_acknowledgement',
    title: 'Alarm acknowledgement failed',
    message: actionError,
    onDismiss: () => setActionError(null),
  });
  const attention = useMemo(
    () => attentionAlarmsNewestFirst(snapshot?.alarms.alarms ?? []),
    [snapshot],
  );
  const standing = snapshot?.alarms.alarms.filter((alarm) =>
    alarm.lifecycleState === 'active_acknowledged') ?? [];
  const attentionCount = attention.length + clientErrors.length;
  const attentionSummary = runtimeAttentionSummary(attention, clientErrors);
  const singleClientError = attentionCount === 1 ? clientErrors[0] : undefined;
  const singleAlarm = attentionCount === 1 ? attention[0] : undefined;
  const attentionSeverity = clientErrors.length > 0 || attention.some(
    (alarm) => alarm.priority === 'critical' || alarm.priority === 'high',
  ) ? 'error' : 'warning';
  const combinedHistory = useMemo(() => (
    [...clientHistory, ...historyEvents].sort((left, right) => (
      right.timestampNs - left.timestampNs || left.id.localeCompare(right.id)
    ))
  ), [clientHistory, historyEvents]);
  useEffect(() => {
    if (attentionCount < 2) setBulkFence(null);
  }, [attentionCount]);

  const loadHistory = async (
    query: HistorianListQuery = historyQuery,
    append = false,
  ) => {
    setHistoryLoading(true);
    setHistoryError(null);
    try {
      const effectiveQuery = {
        ...query,
        ...(append && historyNextBefore !== null
          ? { beforeSequence: historyNextBefore }
          : { beforeSequence: undefined }),
      };
      const page = await api.history.list(effectiveQuery);
      const incoming = page.events.map(historianEventView);
      setHistoryEvents((current) => append
        ? [...current, ...incoming.filter((event) => (
            !current.some((existing) => existing.id === event.id)
          ))]
        : incoming);
      setHistoryQuery(query);
      setHistoryNextBefore(page.nextBeforeSequence);
      setHistoryHasOlder(page.truncated && page.nextBeforeSequence !== null);
    } catch (reason) {
      setHistoryError(actionErrorMessage(reason));
    } finally {
      setHistoryLoading(false);
    }
  };

  const loadHistoryDetail = async (event: ExecutionHistoryEventView) => {
    if (event.sequence === undefined) {
      setHistoryDetail(null);
      setHistoryDetailError(null);
      return;
    }
    setHistoryDetailLoading(true);
    setHistoryDetailError(null);
    try {
      setHistoryDetail(await api.history.detail(event.sequence));
    } catch (reason) {
      setHistoryDetail(null);
      setHistoryDetailError(actionErrorMessage(reason));
    } finally {
      setHistoryDetailLoading(false);
    }
  };

  const acknowledge = async (view: OperatorAlarmView) => {
    const alarm = snapshot?.alarms.alarms.find(
      (candidate) => candidate.conditionId === view.conditionId
        && candidate.activationId === view.activationId
        && candidate.recordRevision === view.recordRevision,
    );
    if (snapshot === null || alarm === undefined) return;
    setBusyActivation(alarm.activationId);
    setActionError(null);
    try {
      await api.alarms.acknowledge(
        alarm,
        snapshot.alarms.ownerEpoch,
        requestId(),
      );
    } catch (reason) {
      setActionError(actionErrorMessage(reason));
    } finally {
      setBusyActivation(null);
    }
  };

  const requestBulkConfirmation = () => {
    const members = snapshot?.alarms.unacknowledgedMembership ?? [];
    const clientErrorIds = clientErrors.map((error) => error.id);
    setBulkFence({
      alarmFence: snapshot === null || members.length === 0
        ? null
        : acknowledgementFence(
            snapshot.alarms.ownerEpoch,
            snapshot.alarms.generation,
            members,
          ),
      clientErrorIds,
    });
    setActionError(null);
  };

  const confirmBulk = async () => {
    if (bulkFence === null) return;
    const members = snapshot?.alarms.unacknowledgedMembership ?? [];
    const clientErrorIds = clientErrors.map((error) => error.id);
    const alarmSetChanged = bulkFence.alarmFence !== null && (
      snapshot === null
      || !acknowledgementFenceMatches(
        bulkFence.alarmFence,
        snapshot.alarms.ownerEpoch,
        snapshot.alarms.generation,
        members,
      )
    );
    if (alarmSetChanged || !sameIdentities(bulkFence.clientErrorIds, clientErrorIds)) {
      setBulkFence(null);
      setActionError('The attention set changed. Review it before confirming again.');
      onExpandedChange?.(true);
      return;
    }
    setBulkBusy(true);
    setActionError(null);
    try {
      if (bulkFence.alarmFence !== null && snapshot !== null) {
        await api.alarms.acknowledgeAll(
          snapshot.alarms.ownerEpoch,
          snapshot.alarms.generation,
          members,
          requestId(),
        );
      }
      bulkFence.clientErrorIds.forEach((id) => onAcknowledgeClientError?.(id));
      setBulkFence(null);
    } catch (reason) {
      setActionError(actionErrorMessage(reason));
      onExpandedChange?.(true);
    } finally {
      setBulkBusy(false);
    }
  };

  if (!enabled) return null;
  return (
    <section
      className={`runtime-observability-surface ${expanded ? 'expanded' : 'collapsed'}${
        attentionCount > 0 ? ` has-attention attention-${attentionSeverity}` : ''
      }`}
      role="dialog"
      aria-modal="false"
      aria-label="Runtime, alarms, and history"
    >
      <header className="runtime-observability-rail">
        <strong>Runtime / history</strong>
        <span className="runtime-observability-rail-summary" aria-live="polite">
          {attentionSummary}
        </span>
        <div className="runtime-observability-rail-actions">
          {singleClientError !== undefined && onAcknowledgeClientError !== undefined && (
            <button
              type="button"
              disabled={bulkBusy || busyActivation !== null}
              title={`${singleClientError.acknowledgementRequired
                ? 'Acknowledge'
                : 'Dismiss'} this Composer message. This does not change execution.`}
              onClick={() => onAcknowledgeClientError(singleClientError.id)}
            >
              {singleClientError.acknowledgementRequired ? 'Acknowledge' : 'Dismiss'}
            </button>
          )}
          {singleAlarm !== undefined && (
            <button
              type="button"
              disabled={bulkBusy || busyActivation === singleAlarm.activationId}
              title="Acknowledge this exact alarm activation and revision. This does not clear the condition or resume motion."
              onClick={() => { void acknowledge(operatorAlarmView(singleAlarm)); }}
            >
              Acknowledge
            </button>
          )}
          {attentionCount > 1 && (
            <button
              ref={acknowledgeAllButton}
              type="button"
              disabled={bulkBusy || busyActivation !== null}
              title="Acknowledge the exact current alarm set and dismiss the exact current Composer messages. This does not change execution."
              onClick={requestBulkConfirmation}
            >
              Acknowledge all
            </button>
          )}
          <button
            type="button"
            className="icon-button"
            aria-label={expanded ? 'Collapse runtime details' : 'Expand runtime details'}
            aria-expanded={expanded}
            title={expanded ? 'Collapse runtime details' : 'Expand runtime details'}
            onClick={() => onExpandedChange?.(!expanded)}
          >
            {expanded ? '⌃' : '⌄'}
          </button>
          {onClose && (
            <button
              type="button"
              className="icon-button"
              aria-label="Close runtime messages"
              title="Close runtime messages"
              onClick={onClose}
            >×</button>
          )}
        </div>
      </header>
      {expanded && (
      <div className="runtime-observability-details">
      <nav className="runtime-observability-tabs" aria-label="Runtime observability sections">
        <button
          type="button"
          className={mode === 'attention' ? 'active' : ''}
          aria-pressed={mode === 'attention'}
          onClick={() => setMode('attention')}
        >
          Attention{attentionCount > 0 ? ` ${attentionCount}` : ''}
        </button>
        <button
          type="button"
          className={mode === 'active' ? 'active' : ''}
          aria-pressed={mode === 'active'}
          onClick={() => setMode('active')}
        >
          Active{standing.length > 0 ? ` ${standing.length}` : ''}
        </button>
        <button
          type="button"
          className={mode === 'history' ? 'active' : ''}
          aria-pressed={mode === 'history'}
          onClick={() => {
            setMode('history');
            if (historyEvents.length === 0) void loadHistory();
          }}
        >History</button>
      </nav>

      {mode === 'attention' && (
        <div className="runtime-observability-section" aria-label="Attention required">
          <header className="runtime-observability-section-header">
            <div>
              <strong>Attention required</strong>
              <small>Exact backend alarms and current Composer-session errors.</small>
            </div>
          </header>
          {attentionCount === 0 && <p className="muted">Nothing currently requires acknowledgement.</p>}
          {clientErrors.map((clientError) => (
          <div className="operator-alarm-panel" key={clientError.id}>
            <details>
              <summary>
                COMPOSER ERROR{clientError.acknowledgementRequired
                  ? ` — ${clientError.active ? '' : 'RESOLVED · '}DISMISSAL REQUIRED`
                  : ''} · {clientError.title}
              </summary>
              <KernelErrorPanel
                error={{ code: clientError.id, message: clientError.message }}
                presentation={{
                  headline: clientError.title,
                  hint: clientError.message,
                  severity: 'error',
                }}
                presentationKey={clientError.id}
                eyebrow="Composer client error"
              />
            </details>
            <div className="operator-alarm-actions">
              <button
                type="button"
                onClick={() => onAcknowledgeClientError?.(clientError.id)}
              >
                {clientError.acknowledgementRequired
                  ? 'Acknowledge message'
                  : 'Dismiss message'}
              </button>
            </div>
          </div>
        ))}
          {attention.map((alarm) => (
          <OperatorAlarmPanel
            key={alarm.activationId}
            alarm={operatorAlarmView(alarm)}
            busy={busyActivation === alarm.activationId}
            onAcknowledge={(value) => { void acknowledge(value); }}
          />
          ))}
          {actionError !== null && (
          <p className="error-state" role="alert">{actionError}</p>
          )}
          {error !== null && (
          <button type="button" className="text-button" onClick={reconnect}>
            Reconnect headless projection
          </button>
          )}
        </div>
      )}

      {mode === 'active' && (
        <div className="runtime-observability-section" aria-label="Active alarms">
          <header className="runtime-observability-section-header">
            <div>
              <strong>Active conditions</strong>
              <small>Standing owner-issued conditions already acknowledged by the operator.</small>
            </div>
          </header>
          {standing.length === 0 && <p className="muted">No acknowledged alarm remains active.</p>}
          {standing.map((alarm) => (
            <OperatorAlarmPanel
              key={alarm.activationId}
              alarm={operatorAlarmView(alarm)}
              busy={false}
            />
          ))}
        </div>
      )}

      <ExecutionHistoryPanel
        open={mode === 'history'}
        embedded
        onClose={() => setMode('attention')}
        onReload={(query) => { void loadHistory(query); }}
        onLoadOlder={() => { void loadHistory(historyQuery, true); }}
        hasOlder={historyHasOlder}
        events={combinedHistory}
        eyebrow="Backend history and bounded Composer-session trace"
        loading={historyLoading}
        error={historyError}
        selectedDetail={historyDetail}
        detailLoading={historyDetailLoading}
        detailError={historyDetailError}
        onSelectEvent={(event) => { void loadHistoryDetail(event); }}
      />
      </div>
      )}
      <OverlayPanel
        open={bulkFence !== null}
        title="Confirm acknowledge all"
        titleId="runtime-acknowledge-all-confirmation-title"
        compact
        onClose={() => setBulkFence(null)}
        returnFocusRef={acknowledgeAllButton}
      >
        <div className="program-dialog-copy runtime-bulk-confirmation">
          <p>
            Acknowledge the exact current set of {attentionCount} attention messages?
          </p>
          <p>
            Active backend conditions remain active. Composer messages are dismissed.
            This does not resume motion or otherwise change execution.
          </p>
          <div className="program-dialog-actions">
            <button type="button" disabled={bulkBusy} onClick={() => setBulkFence(null)}>
              Cancel
            </button>
            <button
              type="button"
              className="primary-action"
              disabled={bulkBusy}
              onClick={() => { void confirmBulk(); }}
            >
              {bulkBusy ? 'Acknowledging' : 'Acknowledge all'}
            </button>
          </div>
        </div>
      </OverlayPanel>
    </section>
  );
}

export function HeadlessRuntimeSurfaces({
  enabled,
  headlessStateSource,
  clientErrors = [],
  clientHistory = [],
  onAcknowledgeClientError,
  attentionRequest = null,
  expanded = true,
  onExpandedChange,
  onClose,
}: HeadlessRuntimeSurfacesProps) {
  const headlessState = useShellHeadlessState(headlessStateSource);
  useEffect(() => {
    if (onClose === undefined) return undefined;
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      onClose();
    };
    document.addEventListener('keydown', handleKeyDown);
    return () => document.removeEventListener('keydown', handleKeyDown);
  }, [onClose]);
  return (
    <HeadlessRuntimeSurfacesView
      enabled={enabled}
      headlessState={headlessState}
      clientErrors={clientErrors}
      clientHistory={clientHistory}
      onAcknowledgeClientError={onAcknowledgeClientError}
      attentionRequest={attentionRequest}
      expanded={expanded}
      onExpandedChange={onExpandedChange}
      onClose={onClose}
    />
  );
}
