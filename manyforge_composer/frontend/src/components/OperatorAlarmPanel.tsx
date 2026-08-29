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

interface OperatorAlarmPanelProps {
  alarm: OperatorAlarmView;
  busy?: boolean;
  onAcknowledge?: (alarm: OperatorAlarmView) => void;
  onMinimize?: () => void;
}

/**
 * Alarm presentation and exact-record callback binding retained for I5. The
 * headless owner supplies lifecycle and freshness; this component only maps
 * those facts and submits acknowledgement intent through its callback.
 */
export function OperatorAlarmPanel({
  alarm,
  busy = false,
  onAcknowledge,
  onMinimize,
}: OperatorAlarmPanelProps) {
  const occurrenceCount = alarm.details.occurrence_count;
  const countLabel = typeof occurrenceCount === 'number'
    ? ` · ${occurrenceCount} occurrence${occurrenceCount === 1 ? '' : 's'}`
    : '';
  return (
    <div className="operator-alarm-panel">
      <details>
        <summary>
          {alarm.lifecycleLabel} · {alarm.priorityLabel} · {alarm.title}
          {countLabel}
        </summary>
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
            headline: `${alarm.lifecycleLabel} · ${alarm.priorityLabel} · ${alarm.title}`,
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
