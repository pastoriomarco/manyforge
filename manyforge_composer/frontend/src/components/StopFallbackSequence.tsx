import { useId } from 'react';

import {
  fallbackCandidates, toggleFallback, type FallbackStopMode,
} from '../utils/stopFallbackSequence';

const FALLBACK_ROWS: Record<FallbackStopMode, { label: string; description: string }> = {
  spring_back_hold: {
    label: 'Spring-back on path', description: 'returns along the path; needs planning',
  },
  spring_back_fast: {
    label: 'Spring-back fast', description: 'returns at once, no planning or checks',
  },
};

/**
 * The ordered fallbacks under the Stop mode row: each mode below the primary
 * as a checkbox, then the fixed final hard deceleration. Every change reports
 * the whole list, already in the fixed order.
 */
export function StopFallbackSequence({ primary, value, disabled, onChange }: {
  primary: string;
  value: readonly string[];
  disabled: boolean;
  onChange: (next: FallbackStopMode[]) => void;
}) {
  const headingId = useId();
  if (primary === 'hard_deceleration') return null;
  const candidates = fallbackCandidates(primary);
  return (
    <div className="deployment-stop-fallbacks" role="group" aria-labelledby={headingId}>
      <span className="deployment-stop-fallbacks-heading" id={headingId}>Fallback if this Stop can&apos;t be carried out</span>
      {candidates.map((mode) => (
        <label className="toggle-control" key={mode}>
          <input type="checkbox" aria-label={`Fallback: ${FALLBACK_ROWS[mode].label}`}
            checked={value.includes(mode)} disabled={disabled}
            onChange={(event) => onChange(toggleFallback(primary, value, mode, event.target.checked))} />
          <span>{FALLBACK_ROWS[mode].label} <span className="field-help">({FALLBACK_ROWS[mode].description})</span></span>
        </label>
      ))}
      <label className="toggle-control" title="Always the final step; it cannot be removed.">
        <input type="checkbox" aria-label="Fallback: Hard deceleration (always last)" checked disabled readOnly />
        <span>Hard deceleration (always last)</span>
      </label>
      <p className="field-help">
        Tried in this order, only when the previous step cannot be carried out. All attempts share this
        Stop&apos;s one deadline; a fallback gets no extra time.
      </p>
    </div>
  );
}
