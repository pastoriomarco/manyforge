import { KernelErrorPanel } from './KernelErrorPanel';
import type { KernelErrorPresentation } from './KernelErrorPanel';

export interface OperatorAlarmView {
  conditionId: string;
  activationId: string;
  recordRevision: number;
  definitionId: string;
  lifecycleLabel: string;
  priorityLabel: string;
  severity: KernelErrorPresentation['severity'];
  title: string;
  message: string;
  operatorResponse: string;
  details: Record<string, unknown>;
}

/**
 * What went wrong, for the headline: the owner's failure message when the
 * record carries one, else the alarm message. The definition title is the
 * category and goes into the status line.
 */
export function operatorAlarmCause(
  alarm: { message: string; details: Record<string, unknown> },
): string {
  const failure = alarm.details.failure;
  const message = failure !== null && typeof failure === 'object' && !Array.isArray(failure)
    ? (failure as Record<string, unknown>).message
    : undefined;
  return typeof message === 'string' && message.trim() !== '' ? message : alarm.message;
}

/** "<category> · node <id> · <lifecycle> · <priority> · <n> occurrences". */
export function operatorAlarmStatus(alarm: OperatorAlarmView): string {
  const node = alarm.details.node_instance_id;
  const occurrenceCount = alarm.details.occurrence_count;
  return [
    alarm.title,
    typeof node === 'string' ? `node ${node}` : null,
    alarm.lifecycleLabel,
    alarm.priorityLabel,
    typeof occurrenceCount === 'number'
      ? `${occurrenceCount} occurrence${occurrenceCount === 1 ? '' : 's'}`
      : null,
  ].filter((part) => part !== null).join(' · ');
}

interface OperatorAlarmPanelProps {
  alarm: OperatorAlarmView;
  busy?: boolean;
  onAcknowledge?: (alarm: OperatorAlarmView) => void;
  onMinimize?: () => void;
}

/**
 * Alarm presentation and exact-record callback binding retained for a future client. The
 * headless owner supplies lifecycle and freshness; this component only maps
 * those facts and submits acknowledgement intent through its callback.
 */
export function OperatorAlarmPanel({
  alarm,
  busy = false,
  onAcknowledge,
  onMinimize,
}: OperatorAlarmPanelProps) {
  const cause = operatorAlarmCause(alarm);
  const status = operatorAlarmStatus(alarm);
  return (
    <div className="operator-alarm-panel">
      <details>
        <summary>{cause} · {status}</summary>
        <KernelErrorPanel
          error={{
            code: alarm.definitionId,
            message: alarm.message,
            details: {
              ...alarm.details,
              condition_id: alarm.conditionId,
              activation_id: alarm.activationId,
              record_revision: alarm.recordRevision,
            },
          }}
          presentation={{
            headline: cause,
            status,
            hint: alarm.operatorResponse,
            severity: alarm.severity,
          }}
          presentationKey={`${alarm.conditionId}:${alarm.activationId}`}
          eyebrow="Operator alarm"
        />
      </details>
      <div className="operator-alarm-actions">
        {onMinimize && <button type="button" onClick={onMinimize}>Minimize</button>}
        {onAcknowledge && (
          <button
            type="button"
            disabled={busy}
            onClick={() => onAcknowledge(alarm)}
            title="Acknowledge this exact activation and revision; this does not clear the condition or resume motion."
          >
            Acknowledge alarm
          </button>
        )}
      </div>
    </div>
  );
}
