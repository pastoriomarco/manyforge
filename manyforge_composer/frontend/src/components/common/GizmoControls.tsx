import { useCallback, useMemo, useState } from 'react';
import { ANGULAR_STEP_OPTIONS, DEFAULT_GIZMO_SETTINGS, LINEAR_STEP_OPTIONS } from '../../utils/gizmo';
import type { GizmoSettings } from '../../utils/gizmo';

export interface GizmoController {
  settings: GizmoSettings;
  update: (update: Partial<GizmoSettings>) => void;
}

/** One owner in the workspace, with a local default for standalone panels. */
export function useGizmoSettings(): GizmoController {
  const [settings, setSettings] = useState(DEFAULT_GIZMO_SETTINGS);
  const update = useCallback((change: Partial<GizmoSettings>) => {
    setSettings((current) => ({ ...current, ...change }));
  }, []);
  return useMemo(() => ({ settings, update }), [settings, update]);
}

export function StepSelect({ label, unit, options, value, disabled, onChange }: {
  label: string; unit: string; options: readonly number[]; value: number;
  disabled: boolean; onChange: (value: number) => void;
}) {
  return <label className="robot-control-step">
    <span title={`${label} (${unit})`}>Step ({unit})</span>
    <select aria-label={`${label} (${unit})`} value={value} disabled={disabled}
      onChange={(event) => onChange(Number(event.currentTarget.value))}>
      {options.map((option) => <option key={option} value={option}>{option}</option>)}
    </select>
  </label>;
}

export function GizmoSteps({ gizmo, disabled = false }: { gizmo: GizmoController; disabled?: boolean }) {
  return <>
    <StepSelect label="Position step" unit="m" options={LINEAR_STEP_OPTIONS}
      value={gizmo.settings.linearStepM} disabled={disabled} onChange={(linearStepM) => gizmo.update({ linearStepM })} />
    <StepSelect label="Angle step" unit="deg" options={ANGULAR_STEP_OPTIONS}
      value={gizmo.settings.angularStepDeg} disabled={disabled} onChange={(angularStepDeg) => gizmo.update({ angularStepDeg })} />
  </>;
}
