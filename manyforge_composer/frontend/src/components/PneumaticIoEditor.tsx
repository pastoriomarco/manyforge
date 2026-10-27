import type { JsonValue } from '../types/api';
import type { IoSignalInputDraft, IoSignalOutputDraft } from '../utils/ioSignalConfiguration';
import { PNEUMATIC_INPUT_KEYS, type PneumaticInputKey } from '../utils/pneumaticGripperAuthoring';

const LABELS: Record<PneumaticInputKey, string> = {
  open: 'Open', closed: 'Closed', pressure: 'Pressure', workpiece_grasped: 'Workpiece grasped',
};

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== undefined && value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

function signalKey(value: JsonValue | undefined): string {
  const signal = record(value);
  return typeof signal?.controllable_component_id === 'string' && typeof signal?.signal_name === 'string'
    && signal.controllable_component_id !== '' && signal.signal_name !== ''
    ? JSON.stringify([signal.controllable_component_id, signal.signal_name]) : '';
}

function signalFromKey(key: string): JsonValue {
  if (key === '') return { controllable_component_id: '', signal_name: '' };
  const [component, signal] = JSON.parse(key) as [string, string];
  return { controllable_component_id: component, signal_name: signal };
}

function signalOptions(rows: readonly (IoSignalInputDraft | IoSignalOutputDraft)[]) {
  return [...new Map(rows.filter((row) => row.componentId && row.signalName).map((row) => {
    const value = JSON.stringify([row.componentId, row.signalName]);
    return [value, { value, label: `${row.componentId} · ${row.signalName}` }] as const;
  })).values()];
}

export function pneumaticIoProblem(value: JsonValue | undefined): string | null {
  if (value === undefined) return null;
  const io = record(value);
  const outputs = io?.outputs;
  if (!Array.isArray(outputs) || outputs.length < 1 || outputs.length > 2) {
    return 'Select one or two valve outputs.';
  }
  const seen = new Set<string>();
  for (const value of outputs) {
    const output = record(value);
    const signal = signalKey(output?.signal);
    if (!signal || typeof output?.open_value !== 'boolean'
        || typeof output?.close_value !== 'boolean') {
      return 'Choose a signal and Open and Close values for each valve output.';
    }
    if (seen.has(signal)) return 'Choose a different signal for each valve output.';
    seen.add(signal);
  }
  const inputs = record(io?.inputs);
  if (io?.inputs !== undefined && inputs === null) return 'Choose valid gripper input signals.';
  for (const key of PNEUMATIC_INPUT_KEYS) {
    if (inputs?.[key] !== undefined && !signalKey(record(inputs[key])?.signal)) {
      return `Choose a signal for ${LABELS[key]}.`;
    }
  }
  return null;
}

export function PneumaticIoEditor({
  value, enabled, disabled, inputs, outputs, onChange,
}: {
  value: JsonValue | undefined;
  enabled: boolean;
  disabled: boolean;
  inputs: readonly IoSignalInputDraft[];
  outputs: readonly IoSignalOutputDraft[];
  onChange: (value: JsonValue | undefined) => void;
}) {
  const io = record(value) ?? {};
  const outputRows = Array.isArray(io.outputs) ? io.outputs : [];
  const inputRows = record(io.inputs) ?? {};
  const outputOptions = signalOptions(outputs);
  const inputOptions = signalOptions(inputs);
  const setOutputs = (next: JsonValue[]) => onChange({ ...io, outputs: next });
  const setOutput = (index: number, field: string, next: JsonValue) => {
    const rows = [...outputRows];
    rows[index] = { ...(record(rows[index]) ?? {}), [field]: next };
    setOutputs(rows);
  };
  const setInput = (name: PneumaticInputKey, next: string) => {
    const rows = { ...inputRows };
    if (next === '') delete rows[name];
    else rows[name] = { signal: signalFromKey(next) };
    onChange({ ...io, inputs: rows });
  };
  return (
    <section className="deployment-gripper-editor" aria-label="Pneumatic gripper I/O">
      <h4>Pneumatic gripper I/O</h4>
      <p className="field-help">Choose signals declared on any I/O component. Outputs going false are written before outputs going true, in a stable signal order within each group.</p>
      {value === undefined ? (
        <button type="button" disabled={disabled || !enabled} onClick={() => onChange({ outputs: [] })}>Configure pneumatic I/O</button>
      ) : (
        <>
          {outputRows.map((value, index) => {
            const row = record(value) ?? {};
            const current = signalKey(row.signal);
            return <fieldset key={index} aria-label={`Valve output ${index + 1}`}>
              <legend>Valve output {index + 1}</legend>
              <label>Output signal
                <select aria-label={`Valve output ${index + 1} signal`} value={current} disabled={disabled || !enabled} onChange={(event) => setOutput(index, 'signal', signalFromKey(event.target.value))}>
                  <option value="">Select an output</option>
                  {current && !outputOptions.some((option) => option.value === current) && <option value={current}>Previously selected signal (not declared)</option>}
                  {outputOptions.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
                </select>
              </label>
              {(['open_value', 'close_value'] as const).map((field) => <label key={field}>{field === 'open_value' ? 'Open value' : 'Close value'}
                <select aria-label={`Valve output ${index + 1} ${field === 'open_value' ? 'open' : 'close'} value`} value={typeof row[field] === 'boolean' ? String(row[field]) : ''} disabled={disabled || !enabled}
                  onChange={(event) => setOutput(index, field, event.target.value === '' ? null : event.target.value === 'true')}>
                  <option value="">Select a value</option><option value="false">False</option><option value="true">True</option>
                </select>
              </label>)}
              <button type="button" disabled={disabled || !enabled} onClick={() => setOutputs(outputRows.filter((_row, at) => at !== index))}>Remove output</button>
            </fieldset>;
          })}
          <button type="button" disabled={disabled || !enabled || outputRows.length >= 2}
            onClick={() => setOutputs([...outputRows, { signal: signalFromKey(''), open_value: null, close_value: null }])}>Add valve output</button>
          <h5>Optional inputs</h5>
          {PNEUMATIC_INPUT_KEYS.map((key) => {
            const current = signalKey(record(inputRows[key])?.signal);
            return <label key={key}>{LABELS[key]}
              <select aria-label={`${LABELS[key]} input signal`} value={current} disabled={disabled || !enabled} onChange={(event) => setInput(key, event.target.value)}>
                <option value="">Not configured</option>
                {current && !inputOptions.some((option) => option.value === current) && <option value={current}>Previously selected signal (not declared)</option>}
                {inputOptions.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
              </select>
            </label>;
          })}
          <button type="button" disabled={disabled} onClick={() => onChange(undefined)}>Remove pneumatic I/O</button>
        </>
      )}
    </section>
  );
}
