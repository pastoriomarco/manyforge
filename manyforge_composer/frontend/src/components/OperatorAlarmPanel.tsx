import type { OperatorAlarm } from '../types/api';
import { KernelErrorPanel } from './KernelErrorPanel';

function lifecycleLabel(alarm: OperatorAlarm): string {
  switch (alarm.lifecycleState) {
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

function diagnosticDetails(alarm: OperatorAlarm): Record<string, unknown> {
  const details: Record<string, unknown> = {
    lifecycle_state: alarm.lifecycleState,
    priority: alarm.priority,
    alarm_class: alarm.alarmClass,
    condition_id: alarm.conditionId,
    activation_id: alarm.activationId,
    activation_count: alarm.activationCount,
    occurrence_count: alarm.occurrenceCount,
    record_revision: alarm.recordRevision,
    source_id: alarm.sourceId,
    source_epoch: alarm.sourceEpoch,
    source_sequence: alarm.sourceSequence,
    source_quality: alarm.sourceQuality,
    consequence: alarm.consequence,
    operator_response: alarm.operatorResponse,
  };
  Object.entries(alarm.scope).forEach(([key, value]) => {
    details[`scope_${key}`] = value;
  });
  Object.entries(alarm.displayArgs).forEach(([key, value]) => {
    details[`display_${key}`] = value;
  });
  Object.entries(alarm.correlations).forEach(([key, value]) => {
    details[`correlation_${key}`] = value;
  });
  Object.entries(alarm.details).forEach(([key, value]) => {
    details[key] = value;
  });
  if (alarm.evidenceRefs.length > 0) {
    details.evidence_refs = alarm.evidenceRefs.join(', ');
  }
  return details;
}

export function incidentIdForAlarm(alarm: OperatorAlarm): string | null {
  const value = alarm.correlations.incident_id ?? alarm.correlations.incidentId;
  return typeof value === 'string' && value.length > 0 ? value : null;
}

export function OperatorAlarmPanel({
  alarm,
  incidentSnapshot,
  onAcknowledge,
  onMinimize,
}: {
  alarm: OperatorAlarm;
  incidentSnapshot?: Record<string, unknown> | null;
  onAcknowledge: () => void;
  onMinimize: () => void;
}) {
  const status = lifecycleLabel(alarm);
  const priority = `${alarm.priority.toUpperCase()} PRIORITY`;
  return (
    <KernelErrorPanel
      message={alarm.message}
      errorCode={alarm.definitionId}
      errorDetails={diagnosticDetails(alarm)}
      incidentSnapshot={incidentSnapshot}
      presentationKey={alarm.activationId}
      headlineOverride={`${status} · ${priority} · ${alarm.title}`}
      hintOverride={alarm.operatorResponse}
      onAcknowledge={onAcknowledge}
      acknowledgeLabel="Acknowledge alarm"
      acknowledgeTitle="Mark this exact alarm activation as seen. This does not clear the condition, change an interlock, resume execution, or move the robot."
      onMinimize={onMinimize}
    />
  );
}
