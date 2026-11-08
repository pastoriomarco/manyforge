import {
  STOP_STAGE_LABELS,
  STOP_STAGES_UNAVAILABLE_HINT,
  type QualifiedStopStage,
} from '../utils/interruptAuthoring';

/**
 * One rule's Stop stage choice: the robot's qualified stages (their sealed row
 * ids) when known; otherwise the three stage labels, not selectable, with the
 * unavailable hint. A stored id is never invented, and an existing
 * one is kept and can be cleared.
 */
export function StopStagePicker({ label, value, stages, omittedLabel, disabled, onChange }: {
  label: string;
  value: string | undefined;
  /** Null when the GUI has no qualified stage ids for this robot. */
  stages: readonly QualifiedStopStage[] | null;
  omittedLabel: string;
  disabled: boolean;
  onChange: (next: string | undefined) => void;
}) {
  if (stages === null) {
    return (
      <div className="stop-stage-picker" aria-label={label}>
        <ol className="stop-stage-labels" aria-label={`${label} stages`}>
          {STOP_STAGE_LABELS.map((stage) => <li key={stage} aria-disabled="true">{stage}</li>)}
        </ol>
        <p className="program-form-hint">{STOP_STAGES_UNAVAILABLE_HINT}</p>
        {value !== undefined && (
          <p className="program-form-hint">
            Stored stage id <code>{value}</code> is kept.{' '}
            <button type="button" className="text-button" disabled={disabled}
              onClick={() => onChange(undefined)}>Clear</button>
          </p>
        )}
      </div>
    );
  }
  const known = value === undefined || stages.some((stage) => stage.id === value);
  return (
    <select aria-label={label} value={value ?? ''} disabled={disabled}
      onChange={(event) => {
        const next = event.target.value;
        if (next === '' || stages.some((stage) => stage.id === next)) onChange(next === '' ? undefined : next);
      }}>
      <option value="">{omittedLabel}</option>
      {!known && <option value={value} disabled title="This stage is not qualified for the selected route.">{`Not in this robot's chain: ${value}`}</option>}
      {stages.map((stage) => <option key={stage.id} value={stage.id}>{stage.label}</option>)}
    </select>
  );
}
