import type { JsonValue } from '../types/api';
import { PNEUMATIC_INPUT_KEYS, type PneumaticInputKey } from '../utils/pneumaticGripperAuthoring';

const LABELS: Record<PneumaticInputKey, string> = {
  open: 'Open', closed: 'Closed', pressure: 'Pressure', workpiece_grasped: 'Workpiece grasped',
};

function row(value: JsonValue): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

export function PneumaticRequiredInputsEditor({ value, declaredKeys, disabled, onCommit }: {
  value: JsonValue | undefined;
  declaredKeys: readonly PneumaticInputKey[];
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  const rows = Array.isArray(value) ? value : [];
  const selected = (key: PneumaticInputKey) => rows.find((item) => row(item)?.input === key);
  const available = PNEUMATIC_INPUT_KEYS.filter((key) => declaredKeys.includes(key) || selected(key) !== undefined);
  const setSelected = (key: PneumaticInputKey, checked: boolean) => onCommit(checked
    ? [...rows, { input: key, value: true }]
    : rows.filter((item) => row(item)?.input !== key));
  const setValue = (key: PneumaticInputKey, next: boolean) => onCommit(rows.map((item) => (
    row(item)?.input === key ? { ...(row(item) ?? {}), value: next } : item
  )));
  return (
    <fieldset className="program-property" aria-label="Required gripper inputs">
      <legend>Required gripper inputs</legend>
      <p className="program-form-hint">Select the inputs that must all match. Once they match, their current values are read again to confirm. With inputs selected, a command duration of 0 waits without a time limit.</p>
      <p className="program-form-hint">An accepted valve write may settle while held; Resume continues the attempt. A write refused before admission starts a new attempt on Resume and writes the same values, with false outputs first. During input waits or dwell, the node's while-held timing rules apply.</p>
      <p className="program-form-hint">Do not command the same pneumatic gripper, command grippers that share valve outputs, or write a gripper's valve outputs through ordinary I/O in parallel branches. Operators sharing a cell must also avoid overlapping manual commands to the same valves. The last write wins for each output.</p>
      {available.length === 0 && <p className="muted">Choose a gripper target with inputs declared in the Deployment Draft.</p>}
      {available.map((key) => {
        const choice = selected(key);
        return <div key={key}>
          <label>
            <input type="checkbox" aria-label={`${LABELS[key]} required`} checked={choice !== undefined} disabled={disabled}
              onChange={(event) => setSelected(key, event.target.checked)} />
            {LABELS[key]}{!declaredKeys.includes(key) && ' (not declared for this gripper)'}
          </label>
          {choice !== undefined && <label>Expected value
            <select aria-label={`${LABELS[key]} expected value`} value={String(row(choice)?.value)}
              disabled={disabled} onChange={(event) => setValue(key, event.target.value === 'true')}>
              <option value="true">True</option><option value="false">False</option>
            </select>
          </label>}
        </div>;
      })}
    </fieldset>
  );
}
