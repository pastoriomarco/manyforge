import { useEffect, useMemo, useState } from 'react';
import { api } from '../api/client';
import { useHeadlessState } from '../hooks/useHeadlessState';
import type { HeadlessStateResult } from '../hooks/useHeadlessState';
import type {
  AlarmLifecycleState,
  AlarmPriority,
  HeadlessRuntimeSnapshot,
  HistorianStoredEvent,
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
import { RuntimeStatusOverlay } from './RuntimeStatusOverlay';
import type { RuntimeStatusView } from './RuntimeStatusOverlay';


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
  return {
    readiness: runtime.complete ? 'degraded' : 'offline',
    summaryLabel: runtime.complete
      ? 'Headless facts available'
      : 'Headless sources incomplete',
    profileLabel: 'H0 projection',
    authorityLabel: 'source-owned facts',
    userMessage: runtime.complete
      ? 'Manager and scene facts are fresh. Robot Control verifies execution-owner readiness separately.'
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
}

interface HeadlessRuntimeSurfacesViewProps extends HeadlessRuntimeSurfacesProps {
  headlessState: HeadlessStateResult;
}

export function HeadlessRuntimeSurfacesView({
  enabled,
  headlessState,
}: HeadlessRuntimeSurfacesViewProps) {
  const { snapshot, error, reconnect } = headlessState;
  const [busyActivation, setBusyActivation] = useState<string | null>(null);
  const [bulkBusy, setBulkBusy] = useState(false);
  const [minimized, setMinimized] = useState<Set<string>>(() => new Set());
  const [bulkFence, setBulkFence] = useState<AlarmAcknowledgementFence | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [historyOpen, setHistoryOpen] = useState(false);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState<string | null>(null);
  const [historyEvents, setHistoryEvents] = useState<ExecutionHistoryEventView[]>([]);
  const runtimeView = useMemo(
    () => snapshot === null ? null : runtimeStatusView(snapshot.runtime),
    [snapshot],
  );
  const attention = useMemo(
    () => attentionAlarmsNewestFirst(snapshot?.alarms.alarms ?? []),
    [snapshot],
  );
  const panels = attention
    .filter((alarm) => !minimized.has(alarm.activationId))
    .slice(0, 3);
  const standing = snapshot?.alarms.alarms.filter((alarm) =>
    alarm.lifecycleState === 'active_acknowledged') ?? [];

  useEffect(() => {
    const currentActivationIds = new Set(
      attention.map((alarm) => alarm.activationId),
    );
    setMinimized((current) => {
      const retained = [...current].filter((activationId) =>
        currentActivationIds.has(activationId));
      return retained.length === current.size ? current : new Set(retained);
    });
  }, [attention]);

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
    <>
      <RuntimeStatusOverlay
        status={runtimeView}
        error={error}
        panelObscured={panels.length > 0}
      />
      <aside className="operator-alarm-stack" aria-label="Operator alarms">
        {panels.map((alarm) => (
          <OperatorAlarmPanel
            key={alarm.activationId}
            alarm={operatorAlarmView(alarm)}
            busy={busyActivation === alarm.activationId}
            onAcknowledge={(value) => { void acknowledge(value); }}
            onMinimize={() => setMinimized((current) => {
              const next = new Set(current);
              next.add(alarm.activationId);
              return next;
            })}
          />
        ))}
        {snapshot !== null && (
          <div className="operator-alarm-summary" role="status">
            <span>{snapshot.alarms.unacknowledgedCount} acknowledgement required</span>
            <span>{snapshot.alarms.activeAcknowledgedCount} active acknowledged</span>
            {standing.map((alarm) => (
              <span
                key={alarm.activationId}
                className="standing-alarm-chip"
                title={`${alarm.message} — ${alarm.operatorResponse}`}
              >
                ACTIVE — ACKNOWLEDGED · {alarm.title}
              </span>
            ))}
            {minimized.size > 0 && (
              <button type="button" onClick={() => setMinimized(new Set())}>
                Restore {minimized.size} minimized
              </button>
            )}
            {snapshot.alarms.unacknowledgedCount > 0 && (
              <button
                type="button"
                disabled={bulkBusy || busyActivation !== null}
                onClick={() => { void confirmBulk(); }}
              >
                {bulkFence === null ? 'Acknowledge all…' : 'Confirm exact set'}
              </button>
            )}
            <button
              type="button"
              onClick={() => {
                setHistoryOpen(true);
                void loadHistory();
              }}
            >
              Messages / history
            </button>
          </div>
        )}
        {actionError !== null && (
          <p className="error-state" role="alert">{actionError}</p>
        )}
        {error !== null && (
          <button type="button" className="text-button" onClick={reconnect}>
            Reconnect headless projection
          </button>
        )}
      </aside>
      <ExecutionHistoryPanel
        open={historyOpen}
        onClose={() => setHistoryOpen(false)}
        events={historyEvents}
        loading={historyLoading}
        error={historyError}
      />
    </>
  );
}

export function HeadlessRuntimeSurfaces({ enabled }: HeadlessRuntimeSurfacesProps) {
  const headlessState = useHeadlessState(enabled);
  return (
    <HeadlessRuntimeSurfacesView
      enabled={enabled}
      headlessState={headlessState}
    />
  );
}
