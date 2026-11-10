import { useEffect, useState } from 'react';
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

function PneumaticSelectDraft({ label, value, disabled, options, validate, onCommit }: {
  label: string;
  value: string;
  disabled: boolean;
  options: Array<{ value: string; label: string }>;
  validate?: (value: string) => string | null;
  onCommit: (value: string) => void | Promise<unknown>;
}) {
  const [source, setSource] = useState(value);
  const [draft, setDraft] = useState(value);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    if (source === value) return;
    setSource(value);
    setDraft(value);
    setError(null);
  }, [source, value]);
  return <label>
    {label}
    <select aria-label={label} value={draft} disabled={disabled} aria-invalid={error !== null}
      onChange={(event) => {
        const next = event.target.value;
        if (next === value) { setDraft(value); setError(null); return; }
        const problem = validate?.(next) ?? null;
        setDraft(next);
        setError(problem);
        if (problem === null) {
          const result = onCommit(next);
          if (result instanceof Promise) void result.then((response) => {
            if (response === null) { setDraft(value); setError(null); }
          });
        }
      }}>
      <option value="">Select {label.toLowerCase()}</option>
      {draft && !options.some((option) => option.value === draft)
        && <option value={draft}>Previously selected value (not declared)</option>}
      {options.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
    </select>
    {error !== null && <small className="deployment-field-error inline-message severity-error" role="alert">{error}</small>}
  </label>;
}

export function PneumaticIoEditor({
  value, enabled, disabled, inputs, outputs, onChange,
}: {
  value: JsonValue | undefined;
  enabled: boolean;
  disabled: boolean;
  inputs: readonly IoSignalInputDraft[];
  outputs: readonly IoSignalOutputDraft[];
  /** `path` edits one existing field; omitted path adds a complete mapping. */
  onChange: (value: JsonValue | undefined, path?: Array<string | number>, operation?: 'set' | 'remove') => void | Promise<unknown>;
}) {
  const [configuring, setConfiguring] = useState(false);
  const [addingOutput, setAddingOutput] = useState(false);
  const [newOutput, setNewOutput] = useState<Record<string, JsonValue> | null>(null);
  const [error, setError] = useState<string | null>(null);
  const configured = value !== undefined || configuring;
  const io = record(value) ?? {};
  const outputRows = Array.isArray(io.outputs) ? io.outputs : [];
  const inputRows = record(io.inputs) ?? {};
  const outputOptions = signalOptions(outputs);
  const inputOptions = signalOptions(inputs);
  const setOutput = (index: number, field: string, next: JsonValue) => {
    const rows = [...outputRows];
    rows[index] = { ...(record(rows[index]) ?? {}), [field]: next };
    return onChange({ ...io, outputs: rows }, ['outputs', index, field], 'set');
  };
  const setInput = (name: PneumaticInputKey, next: string) => {
    const rows = { ...inputRows };
    if (next === '') delete rows[name];
    else rows[name] = { signal: signalFromKey(next) };
    return onChange({ ...io, inputs: rows }, ['inputs', name], next === '' ? 'remove' : 'set');
  };
  return (
    <section className="deployment-gripper-editor" aria-label="Pneumatic gripper I/O">
      <h4>Pneumatic gripper I/O</h4>
      <p className="field-help">Choose signals declared on any I/O component. Outputs going false are written before outputs going true, in a stable signal order within each group.</p>
      {!configured ? (
        <button type="button" disabled={disabled || !enabled} onClick={() => {
          setConfiguring(true);
          setAddingOutput(true);
          setNewOutput({ signal: signalFromKey(''), open_value: null, close_value: null });
          setError(null);
        }}>Configure pneumatic I/O</button>
      ) : (
        <>
          {outputRows.map((value, index) => {
            const row = record(value) ?? {};
            const current = signalKey(row.signal);
            return <fieldset key={index} aria-label={`Valve output ${index + 1}`}>
              <legend>Valve output {index + 1}</legend>
              <PneumaticSelectDraft label={`Valve output ${index + 1} signal`} value={current}
                disabled={disabled || !enabled} options={outputOptions}
                validate={(next) => {
                  if (next === '') return 'Choose an output signal.';
                  if (outputRows.some((other, at) => at !== index && signalKey(record(other)?.signal) === next)) {
                    return 'Choose a different signal for each valve output.';
                  }
                  return null;
                }} onCommit={(next) => setOutput(index, 'signal', signalFromKey(next))} />
              {(['open_value', 'close_value'] as const).map((field) => (
                <PneumaticSelectDraft key={field} label={`Valve output ${index + 1} ${field === 'open_value' ? 'open' : 'close'} value`}
                  value={typeof row[field] === 'boolean' ? String(row[field]) : ''}
                  disabled={disabled || !enabled} options={[{ value: 'false', label: 'False' }, { value: 'true', label: 'True' }]}
                  validate={(next) => next === '' ? 'Choose an Open and Close value.' : null}
                  onCommit={(next) => setOutput(index, field, next === 'true')} />
              ))}
              <button type="button" disabled={disabled || !enabled || outputRows.length <= 1}
                onClick={() => {
                  const next = outputRows.filter((_row, at) => at !== index);
                  const candidate = { ...io, outputs: next };
                  const problem = pneumaticIoProblem(candidate);
                  if (problem !== null) { setError(problem); return; }
                  setError(null);
                  onChange(candidate, ['outputs'], 'set');
                }}>Remove output</button>
            </fieldset>;
          })}
          {!addingOutput && outputRows.length < 2 && <button type="button" disabled={disabled || !enabled}
            onClick={() => {
              setAddingOutput(true);
              setNewOutput({ signal: signalFromKey(''), open_value: null, close_value: null });
              setError(null);
            }}>Add valve output</button>}
          {addingOutput && newOutput !== null && <fieldset aria-label="New valve output">
            <legend>New valve output</legend>
            <PneumaticSelectDraft label="New valve output signal" value={signalKey(newOutput.signal)}
              disabled={disabled || !enabled} options={outputOptions}
              validate={(next) => {
                if (next === '') return 'Choose an output signal.';
                if (outputRows.some((row) => signalKey(record(row)?.signal) === next)) {
                  return 'Choose a different signal for each valve output.';
                }
                return null;
              }} onCommit={(next) => setNewOutput({ ...newOutput, signal: signalFromKey(next) })} />
            {(['open_value', 'close_value'] as const).map((field) => (
              <PneumaticSelectDraft key={field} label={`New valve output ${field === 'open_value' ? 'open' : 'close'} value`}
                value={typeof newOutput[field] === 'boolean' ? String(newOutput[field]) : ''}
                disabled={disabled || !enabled} options={[{ value: 'false', label: 'False' }, { value: 'true', label: 'True' }]}
                validate={(next) => next === '' ? 'Choose an Open and Close value.' : null}
                onCommit={(next) => setNewOutput({ ...newOutput, [field]: next === 'true' })} />
            ))}
            <div className="button-row">
              <button type="button" className="primary-action" disabled={disabled || !enabled}
                onClick={() => {
                  const candidate = { ...io, outputs: [...outputRows, newOutput] };
                  const problem = pneumaticIoProblem(candidate);
                  if (problem !== null) { setError(problem); return; }
                  setError(null);
                  const result = onChange(candidate);
                  if (result instanceof Promise) void result.then((response) => {
                    if (response === null) return;
                    setAddingOutput(false);
                    setNewOutput(null);
                    setConfiguring(false);
                  });
                  else {
                    setAddingOutput(false);
                    setNewOutput(null);
                    setConfiguring(false);
                  }
                }}>Add valve output</button>
              <button type="button" disabled={disabled} onClick={() => {
                setAddingOutput(false);
                setNewOutput(null);
                setError(null);
                if (value === undefined) setConfiguring(false);
              }}>Cancel</button>
            </div>
          </fieldset>}
          {outputRows.length > 0 && <>
            <h5>Optional inputs</h5>
            {PNEUMATIC_INPUT_KEYS.map((key) => {
            const current = signalKey(record(inputRows[key])?.signal);
            const options: Array<{ value: string; label: string }> = [...inputOptions];
            if (current && !options.some((option) => option.value === current)) {
              options.push({ value: current, label: 'Previously selected signal (not declared)' });
            }
            return <PneumaticSelectDraft key={key} label={`${LABELS[key]} input signal`} value={current}
              disabled={disabled || !enabled} options={options}
              onCommit={(next) => setInput(key, next)} />;
            })}
          </>}
          {value !== undefined && <button type="button" disabled={disabled} onClick={() => {
            setError(null);
            setAddingOutput(false);
            setNewOutput(null);
            onChange(undefined);
          }}>Remove pneumatic I/O</button>}
        </>
      )}
      {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
    </section>
  );
}
