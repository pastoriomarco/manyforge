import { useEffect, useMemo, useState } from 'react';
import { api } from '../api/client';
import { useShellHeadlessState } from '../hooks/useHeadlessState';
import type {
  HeadlessStateResult,
  HeadlessStateSource,
} from '../hooks/useHeadlessState';
import type {
  AlarmLifecycleState,
  AlarmPriority,
  HeadlessRuntimeSnapshot,
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
import type { RuntimeStatusView } from './RuntimeStatusOverlay';
import { KernelErrorPanel } from './KernelErrorPanel';
import { useOperationalErrorMirror } from '../state/operationalErrors';

export interface ClientOperationalError {
  id: string;
  title: string;
  message: string;
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

export function runtimeStatusView(
  runtime: HeadlessRuntimeSnapshot,
  context: {
    unacknowledged: number;
    activeAcknowledged: number;
    programCycleState?: string;
    executionOwnerAvailable?: boolean;
    clientErrorCount?: number;
  } = {
    unacknowledged: 0,
    activeAcknowledged: 0,
  },
): RuntimeStatusView {
  const components = Object.entries(runtime.sections).map(([id, section]) => ({
    id,
    label: id.replaceAll(/([A-Z])/g, ' $1').replaceAll('_', ' '),
    status: section.freshness === 'fresh'
      ? 'healthy'
      : section.freshness === 'stale' ? 'fault' : 'error',
    detail: section.knownGaps.length > 0
      ? section.knownGaps.join(', ')
      : `${section.freshness} source generation ${section.sourceGeneration ?? 'unknown'}`,
  }));
  const alarmCount = context.unacknowledged + context.activeAcknowledged;
  if (alarmCount > 0) {
    components.push({
      id: 'operator_alarms',
      label: 'operator alarms',
      status: context.unacknowledged > 0 ? 'fault' : 'warning',
      detail: `${context.unacknowledged} acknowledgement required · ${context.activeAcknowledged} active acknowledged`,
    });
  }
  if ((context.clientErrorCount ?? 0) > 0) {
    components.push({
      id: 'composer_client_errors',
      label: 'Composer client errors',
      status: 'fault',
      detail: `${context.clientErrorCount} dismissal required`,
    });
  }
  if (context.programCycleState === 'terminal_awaiting_reset') {
    components.push({
      id: 'program_cycle',
      label: 'Program cycle',
      status: 'warning',
      detail: 'terminal; Reset is required before the next Run',
    });
  }
  components.push({
    id: 'execution_owner',
    label: 'Execution owner',
    status: context.executionOwnerAvailable === true ? 'healthy' : 'warning',
    detail: context.executionOwnerAvailable === true
      ? 'current addressed execution contract available'
      : 'execution readiness is not asserted by the H0 runtime projection',
  });
  const sourcesDegraded = components.some((component) => (
    component.status !== 'healthy'
  ));
  const readiness = !runtime.complete
    ? 'offline'
    : sourcesDegraded ? 'degraded' : 'ready';
  return {
    readiness,
    summaryLabel: readiness === 'ready'
      ? 'Application surfaces ready'
      : readiness === 'degraded'
        ? 'Operational attention'
        : 'Headless sources incomplete',
    profileLabel: 'H0 projection',
    authorityLabel: 'source-owned facts',
    userMessage: readiness === 'ready'
      ? 'Current runtime and addressed execution-owner projections are available.'
      : runtime.complete
        ? 'One or more current backend, execution-owner, cycle, or alarm contributors requires attention.'
        : `Missing or stale sources: ${runtime.reasons.join(', ') || 'unknown'}.`,
    capabilities: ['runtime observation', 'scene status', 'alarm/history'],
    components,
  };
}

function alarmPhase(eventType: string): HistoryAlarmPhase {
  if (eventType === 'operator_alarm_activated') return 'activation';
  if (eventType === 'operator_alarm_returned') return 'return';
  if (eventType === 'operator_alarm_acknowledged') return 'acknowledgement';
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
  onAcknowledgeClientError?: (id: string) => void;
  attentionRequest?: string | null;
}

interface HeadlessRuntimeSurfacesViewProps {
  enabled: boolean;
  headlessState: HeadlessStateResult;
  clientErrors?: ClientOperationalError[];
  onAcknowledgeClientError?: (id: string) => void;
  attentionRequest?: string | null;
}

export function HeadlessRuntimeSurfacesView({
  enabled,
  headlessState,
  clientErrors = [],
  onAcknowledgeClientError,
  attentionRequest = null,
}: HeadlessRuntimeSurfacesViewProps) {
  const { snapshot, error, reconnect } = headlessState;
  const [busyActivation, setBusyActivation] = useState<string | null>(null);
  const [bulkBusy, setBulkBusy] = useState(false);
  const [bulkFence, setBulkFence] = useState<AlarmAcknowledgementFence | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [mode, setMode] = useState<'attention' | 'active' | 'history'>('attention');
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState<string | null>(null);
  const [historyEvents, setHistoryEvents] = useState<ExecutionHistoryEventView[]>([]);
  const [historyDetail, setHistoryDetail] = useState<HistorianDetail | null>(null);
  const [historyDetailLoading, setHistoryDetailLoading] = useState(false);
  const [historyDetailError, setHistoryDetailError] = useState<string | null>(null);
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

  const loadHistory = async () => {
    setHistoryLoading(true);
    setHistoryError(null);
    try {
      const page = await api.history.list();
      setHistoryEvents(page.events.map(historianEventView));
    } catch (reason) {
      setHistoryError(actionErrorMessage(reason));
    } finally {
      setHistoryLoading(false);
    }
  };

  const loadHistoryDetail = async (event: ExecutionHistoryEventView) => {
    if (event.sequence === undefined) return;
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

  const confirmBulk = async () => {
    if (snapshot === null) return;
    const members = snapshot.alarms.unacknowledgedMembership;
    if (bulkFence === null) {
      setBulkFence(acknowledgementFence(
        snapshot.alarms.ownerEpoch,
        snapshot.alarms.generation,
        members,
      ));
      setActionError(null);
      return;
    }
    if (!acknowledgementFenceMatches(
      bulkFence,
      snapshot.alarms.ownerEpoch,
      snapshot.alarms.generation,
      members,
    )) {
      setBulkFence(null);
      setActionError('The alarm set changed. Review it before confirming again.');
      return;
    }
    setBulkBusy(true);
    setActionError(null);
    try {
      await api.alarms.acknowledgeAll(
        snapshot.alarms.ownerEpoch,
        snapshot.alarms.generation,
        members,
        requestId(),
      );
      setBulkFence(null);
    } catch (reason) {
      setActionError(actionErrorMessage(reason));
    } finally {
      setBulkBusy(false);
    }
  };

  if (!enabled) return null;
  return (
    <section className="runtime-observability-surface">
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
            {snapshot !== null && snapshot.alarms.unacknowledgedCount > 0 && (
              <button
                type="button"
                disabled={bulkBusy || busyActivation !== null}
                onClick={() => { void confirmBulk(); }}
              >
                {bulkFence === null ? 'Acknowledge all…' : 'Confirm exact set'}
              </button>
            )}
          </header>
          {attentionCount === 0 && <p className="muted">Nothing currently requires acknowledgement.</p>}
          {clientErrors.map((clientError) => (
          <div className="operator-alarm-panel" key={clientError.id}>
            <details open>
              <summary>
                COMPOSER ERROR — DISMISSAL REQUIRED · {clientError.title}
              </summary>
              <KernelErrorPanel
                error={{ code: clientError.id, message: clientError.message }}
                presentation={{
                  headline: clientError.title,
                  hint: 'Dismiss after reviewing this client-side command or response failure.',
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
                Dismiss message
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
        onReload={() => { void loadHistory(); }}
        events={historyEvents}
        loading={historyLoading}
        error={historyError}
        selectedDetail={historyDetail}
        detailLoading={historyDetailLoading}
        detailError={historyDetailError}
        onSelectEvent={(event) => { void loadHistoryDetail(event); }}
      />
    </section>
  );
}

export function HeadlessRuntimeSurfaces({
  enabled,
  headlessStateSource,
  clientErrors = [],
  onAcknowledgeClientError,
  attentionRequest = null,
}: HeadlessRuntimeSurfacesProps) {
  const headlessState = useShellHeadlessState(headlessStateSource);
  return (
    <HeadlessRuntimeSurfacesView
      enabled={enabled}
      headlessState={headlessState}
      clientErrors={clientErrors}
      onAcknowledgeClientError={onAcknowledgeClientError}
      attentionRequest={attentionRequest}
    />
  );
}
