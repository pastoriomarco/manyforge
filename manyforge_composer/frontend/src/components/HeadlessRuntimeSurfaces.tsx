import { useEffect, useMemo, useRef, useState } from 'react';
import { api, HISTORIAN_PAGE_LIMIT } from '../api/client';
import type { HistorianListQuery } from '../api/client';
import { useShellHeadlessState } from '../hooks/useHeadlessState';
import type {
  ControlClientIncidentKind,
  HeadlessStateResult,
  HeadlessStateSource,
} from '../hooks/useHeadlessState';
import type {
  AlarmLifecycleState,
  AlarmPriority,
  HistorianStoredEvent,
  HistorianDetail,
  HistorianRetention,
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
import { FailureCapturePanel } from './FailureCapturePanel';
import { useOperationalErrorMirror } from '../state/operationalErrors';
import type { WorkbenchNotice } from '../state/workbenchNotices';
import { OverlayPanel } from './OverlayPanel';
import { NoticeActions, NoticeItem, NotificationRail } from './WorkbenchNotifications';

export interface ClientOperationalError {
  id: string;
  title: string;
  message: string;
  active: boolean;
  acknowledgementRequired: boolean;
  /** Composer messages in the registry are errors; acknowledgement never
   * applies to warnings or information. */
  severity: 'error';
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
      Number(BigInt(right.activatedAtNs) - BigInt(left.activatedAtNs))
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
  if (eventType === 'operator_alarm_acknowledged'
    || eventType === 'operator_alarm_bulk_acknowledged') return 'acknowledgement';
  return null;
}

export function historianEventView(
  stored: HistorianStoredEvent,
): ExecutionHistoryEventView {
  return {
    id: `${stored.sequence}:${stored.event.eventId}`,
    sequence: stored.sequence,
    timestampNs: stored.event.occurredAtNs,
    timestamp: stored.event.occurredAtWall,
    timestampUtcNs: stored.event.occurredAtUtcNs,
    severity: stored.event.conditionSeverity,
    clockOwner: stored.event.clockOwner,
    wallClockRegressed: stored.event.wallClockRegressed,
    channel: stored.event.component,
    message: stored.event.conditionId === null
      ? stored.event.message
      : `${stored.event.message}: ${stored.event.conditionId}`,
    type: stored.event.eventType,
    alarmPhase: alarmPhase(stored.event.eventType),
    programRunId: stored.event.programRunId,
    operationId: stored.event.operationId,
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
  conditions?: WorkbenchNotice[];
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
  /** Current unresolved error conditions. They have source actions but no
   * acknowledgement or dismissal and stay until their source resolves. */
  conditions?: WorkbenchNotice[];
  onAcknowledgeClientError?: (id: string) => void;
  attentionRequest?: string | null;
  expanded?: boolean;
  onExpandedChange?: (expanded: boolean) => void;
  onClose?: () => void;
  reportClientIncident?: (
    kind: ControlClientIncidentKind,
    active: boolean,
    reasonCode: string,
  ) => void;
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
  conditions: readonly WorkbenchNotice[] = [],
): string {
  const count = alarms.length + clientErrors.length + conditions.length;
  if (count === 0) return 'No messages require attention';
  if (count > 1) return `${count} errors require attention · ${
    conditions[0]?.title ?? clientErrors[0]?.title ?? alarms[0]?.title}`;
  const condition = conditions[0];
  if (condition !== undefined) {
    return condition.message ? `${condition.title} — ${condition.message}` : condition.title;
  }
  const clientError = clientErrors[0];
  if (clientError !== undefined) return `${clientError.title} — ${clientError.message}`;
  return alarms[0]?.title ?? 'Attention required';
}

export function HeadlessRuntimeSurfacesView({
  enabled,
  headlessState,
  clientErrors = [],
  conditions = [],
  onAcknowledgeClientError,
  attentionRequest = null,
  expanded = true,
  onExpandedChange,
  onClose,
  reportClientIncident,
}: HeadlessRuntimeSurfacesViewProps) {
  const { snapshot, error, reconnect } = headlessState;
  const [busyActivation, setBusyActivation] = useState<string | null>(null);
  const [bulkBusy, setBulkBusy] = useState(false);
  const [bulkFence, setBulkFence] = useState<RuntimeAttentionFence | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [mode, setMode] = useState<'attention' | 'active' | 'history' | 'captures'>('attention');
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState<string | null>(null);
  const [historyEvents, setHistoryEvents] = useState<ExecutionHistoryEventView[]>([]);
  const [historyRetention, setHistoryRetention] = useState<HistorianRetention | null>(null);
  const [historyQuery, setHistoryQuery] = useState<HistorianListQuery>({
    limit: HISTORIAN_PAGE_LIMIT,
  });
  const [historyBefore, setHistoryBefore] = useState<number | null>(null);
  const [historyNewerCursors, setHistoryNewerCursors] = useState<
    Array<number | null>
  >([]);
  const [historyNextBefore, setHistoryNextBefore] = useState<number | null>(null);
  const [historyHasOlder, setHistoryHasOlder] = useState(false);
  const [historyDetail, setHistoryDetail] = useState<HistorianDetail | null>(null);
  const [historyDetailLoading, setHistoryDetailLoading] = useState(false);
  const [historyDetailError, setHistoryDetailError] = useState<string | null>(null);
  const historyRequest = useRef<AbortController | null>(null);
  const historyDetailRequest = useRef<AbortController | null>(null);
  useEffect(() => () => {
    historyRequest.current?.abort();
    historyDetailRequest.current?.abort();
  }, []);
  useEffect(() => {
    if (!enabled || !expanded || mode !== 'history') {
      historyRequest.current?.abort();
      historyDetailRequest.current?.abort();
      setHistoryLoading(false);
      setHistoryDetailLoading(false);
    }
  }, [enabled, expanded, mode]);
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
  useEffect(() => {
    reportClientIncident?.(
      'history_request_failed',
      historyError !== null,
      'history_request_failed',
    );
  }, [historyError, reportClientIncident]);
  useEffect(() => {
    reportClientIncident?.(
      'history_detail_request_failed',
      historyDetailError !== null,
      'history_detail_request_failed',
    );
  }, [historyDetailError, reportClientIncident]);
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
  // Acknowledgeable/dismissible items keep their exact fence; conditions are
  // counted and shown but never enter acknowledgement.
  const attentionCount = attention.length + clientErrors.length;
  const itemCount = attentionCount + conditions.length;
  const attentionSummary = runtimeAttentionSummary(attention, clientErrors, conditions);
  const singleClientError = itemCount === 1 ? clientErrors[0] : undefined;
  const singleAlarm = itemCount === 1 ? attention[0] : undefined;
  const singleCondition = itemCount === 1 ? conditions[0] : undefined;
  const attentionSeverity = conditions.length > 0
    || clientErrors.length > 0
    || attention.some(
    (alarm) => alarm.priority === 'critical' || alarm.priority === 'high',
  ) ? 'error' : 'warning';
  useEffect(() => {
    if (attentionCount < 2) setBulkFence(null);
  }, [attentionCount]);

  const loadHistory = async (
    query: HistorianListQuery = historyQuery,
    beforeSequence: number | null = null,
    newerCursors: Array<number | null> = [],
  ) => {
    historyRequest.current?.abort();
    historyDetailRequest.current?.abort();
    const request = new AbortController();
    historyRequest.current = request;
    setHistoryDetailLoading(false);
    setHistoryLoading(true);
    setHistoryError(null);
    try {
      const effectiveQuery = {
        ...query,
        ...(beforeSequence !== null
          ? { beforeSequence }
          : { beforeSequence: undefined }),
      };
      const page = await api.history.list(effectiveQuery, request.signal);
      if (request.signal.aborted) return;
      historyDetailRequest.current?.abort();
      setHistoryDetailLoading(false);
      setHistoryEvents(page.events.map(historianEventView));
      setHistoryRetention(page.retention);
      // Freeze the server's UTC window for navigation; Refresh requests a new
      // relative window. Client time and owner-monotonic samples are irrelevant.
      setHistoryQuery({ ...query, recentSeconds: undefined,
        startUtcNs: page.timeWindow.startUtcNs ?? undefined,
        endUtcNs: page.timeWindow.endUtcNs ?? undefined });
      setHistoryBefore(beforeSequence);
      setHistoryNewerCursors(newerCursors.slice(-32));
      setHistoryNextBefore(page.nextBeforeSequence);
      setHistoryHasOlder(page.truncated && page.nextBeforeSequence !== null);
      setHistoryDetail(null);
      setHistoryDetailError(null);
    } catch (reason) {
      if (!request.signal.aborted) setHistoryError(actionErrorMessage(reason));
    } finally {
      if (!request.signal.aborted) setHistoryLoading(false);
    }
  };

  const loadHistoryDetail = async (event: ExecutionHistoryEventView) => {
    if (historyLoading) return;
    historyDetailRequest.current?.abort();
    if (event.sequence === undefined) {
      setHistoryDetail(null);
      setHistoryDetailError(null);
      return;
    }
    const request = new AbortController();
    historyDetailRequest.current = request;
    setHistoryDetailLoading(true);
    setHistoryDetailError(null);
    try {
      const detail = await api.history.detail(event.sequence, request.signal);
      if (!request.signal.aborted) setHistoryDetail(detail);
    } catch (reason) {
      if (!request.signal.aborted) {
        setHistoryDetail(null);
        setHistoryDetailError(actionErrorMessage(reason));
      }
    } finally {
      if (!request.signal.aborted) setHistoryDetailLoading(false);
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
        itemCount > 0 ? ` has-attention attention-${attentionSeverity}` : ''
      }`}
      role="dialog"
      aria-modal="false"
      aria-label="Errors, alarms, and runtime history"
    >
      <NotificationRail
        className="runtime-observability-rail"
        label={itemCount > 0 ? 'Errors' : 'Runtime / history'}
        summary={attentionSummary}
        expanded={expanded}
        onToggle={() => onExpandedChange?.(!expanded)}
        expandLabel="runtime details"
        onClose={onClose}
        closeLabel="Close runtime messages"
        actions={<>
          {singleCondition !== undefined && !expanded && (
            <NoticeActions notice={singleCondition} />
          )}
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
              title="Acknowledge the exact current alarm set and dismiss the exact current Composer messages. Unresolved error conditions remain. This does not change execution."
              onClick={requestBulkConfirmation}
            >
              Acknowledge all
            </button>
          )}
        </>}
      />
      {expanded && (
      <div className="runtime-observability-details">
      <nav className="runtime-observability-tabs" aria-label="Runtime observability sections">
        <button
          type="button"
          className={mode === 'attention' ? 'active' : ''}
          aria-pressed={mode === 'attention'}
          onClick={() => setMode('attention')}
        >
          Attention{itemCount > 0 ? ` ${itemCount}` : ''}
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
            if (historyEvents.length === 0 && !historyLoading) void loadHistory();
          }}
        >History</button>
        <button
          type="button"
          className={mode === 'captures' ? 'active' : ''}
          aria-pressed={mode === 'captures'}
          onClick={() => setMode('captures')}
        >Captures</button>
      </nav>

      <FailureCapturePanel open={enabled && expanded && mode === 'captures'} />

      {mode === 'attention' && (
        <div className="runtime-observability-section" aria-label="Attention required">
          <header className="runtime-observability-section-header">
            <div>
              <strong>Attention required</strong>
              <small>Unresolved error conditions, exact backend alarms and current Composer-session errors.</small>
            </div>
          </header>
          {itemCount === 0 && <p className="muted">Nothing currently requires attention.</p>}
          {conditions.map((condition) => (
            <NoticeItem key={condition.id} notice={condition} />
          ))}
          {clientErrors.map((clientError) => (
          <div
            className={`operator-alarm-panel severity-${clientError.severity}`}
            key={clientError.id}
          >
            <details>
              <summary>
                COMPOSER {clientError.severity.toUpperCase()}{
                  clientError.acknowledgementRequired
                  ? ` — ${clientError.active ? '' : 'RESOLVED · '}DISMISSAL REQUIRED`
                  : ''} · {clientError.title}
              </summary>
              <KernelErrorPanel
                error={{ code: clientError.id, message: clientError.message }}
                presentation={{
                  headline: clientError.title,
                  hint: clientError.message,
                  severity: clientError.severity,
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
          <p className="error-state notice severity-error" role="alert">{actionError}</p>
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
        onLoadOlder={() => {
          if (historyNextBefore !== null) {
            void loadHistory(
              historyQuery,
              historyNextBefore,
              [...historyNewerCursors, historyBefore],
            );
          }
        }}
        onLoadNewer={() => {
          const before = historyNewerCursors.at(-1);
          if (before !== undefined) {
            void loadHistory(
              historyQuery,
              before,
              historyNewerCursors.slice(0, -1),
            );
          }
        }}
        hasOlder={historyHasOlder}
        hasNewer={historyNewerCursors.length > 0}
        events={historyEvents}
        retention={historyRetention}
        currentProgramRunId={snapshot?.programCycle.programRunId}
        eyebrow="Backend history"
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
        <div className="dialog-copy runtime-bulk-confirmation">
          <p>
            Acknowledge the exact current set of {attentionCount} attention messages?
          </p>
          <p>
            Active backend conditions remain active. Composer messages are dismissed.
            This does not resume motion or otherwise change execution.
          </p>
          <div className="dialog-actions">
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
  conditions = [],
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
      conditions={conditions}
      onAcknowledgeClientError={onAcknowledgeClientError}
      attentionRequest={attentionRequest}
      expanded={expanded}
      onExpandedChange={onExpandedChange}
      onClose={onClose}
      reportClientIncident={headlessStateSource.reportClientIncident}
    />
  );
}
