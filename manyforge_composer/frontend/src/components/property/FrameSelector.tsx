import {
  frameKey,
  frameLabel,
  offeredFrameChoices,
  type FrameOffer,
  type FrameScope,
  type ProgramFrameChoice,
  type ProgramPoseFrame,
} from '../../utils/programFrames';

/**
 * The one frame selector: the moving robot's own frames first, then other
 * robots' frames labelled "robot · frame", then the cell and Scene objects.
 * A current frame the field may not take (undeclared, outside a key's
 * `allowed_frames`) stays visible and is marked invalid.
 */
export function FrameSelector({
  scope,
  value,
  offer,
  disabled,
  ariaLabel,
  onChange,
}: {
  scope: FrameScope;
  value: ProgramPoseFrame | null;
  offer?: FrameOffer;
  disabled: boolean;
  ariaLabel: string;
  onChange: (frame: ProgramPoseFrame) => void;
}) {
  const choices = offeredFrameChoices(scope, offer);
  const current = value === null ? '' : frameKey(value);
  const unavailable = value !== null && !choices.some((choice) => choice.key === current);
  const groups = new Map<string, ProgramFrameChoice[]>();
  for (const choice of choices) groups.set(choice.group, [...(groups.get(choice.group) ?? []), choice]);
  return (
    <select
      aria-label={ariaLabel}
      className={unavailable ? 'program-reference-invalid' : undefined}
      aria-invalid={unavailable || undefined}
      title={unavailable ? `${frameLabel(value!)} is not a frame this field may use.` : undefined}
      value={current}
      disabled={disabled}
      onChange={(event) => {
        const chosen = choices.find((choice) => choice.key === event.target.value);
        if (chosen) onChange(chosen.frame);
      }}
    >
      {value === null && <option value="" disabled>Select a frame</option>}
      {unavailable && <option value={current}>Not available · {frameLabel(value!)}</option>}
      {[...groups.entries()].map(([group, rows]) => (
        <optgroup key={group} label={group}>
          {rows.map((choice) => (
            <option key={choice.key} value={choice.key}>{choice.label}</option>
          ))}
        </optgroup>
      ))}
    </select>
  );
}
