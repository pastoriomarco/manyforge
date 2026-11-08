import type { JsonValue } from '../types/api';
import { IO_SOURCE_PROFILE_LABELS, type IoSourceProfile } from '../utils/ioSignalConfiguration';
import {
  mockSignalId,
  mockSimulationRowProblems,
  parseMockSimulation,
  serializeMockSimulation,
  type MockSignal,
  type MockSimulationRow,
} from '../utils/mockSimulation';
import { DraftTextInput } from './common/DraftInputs';

function afterMsProblem(text: string): string | null {
  const trimmed = text.trim();
  return /^\d+$/.test(trimmed) && Number.isSafeInteger(Number(trimmed))
    ? null : 'Enter a whole number of milliseconds, 0 or more.';
}

/**
 * The node's "Mock simulation": one line per signal the
 * node's condition reads — its value when the node starts and optionally one
 * new value a number of ms after node start. Replayed on every node start;
 * applies only to Mock I/O sources. Stored rows for signals the node no
 * longer reads are listed with their refusal so they can be removed.
 */
export function MockSimulationEditor({
  value, signals, profiles, disabled, onCommit, onClear, onInvalidChange,
}: {
  value: JsonValue | undefined;
  signals: readonly MockSignal[];
  /** Declared source profile by `mockSignalId`; a missing signal is unknown (editable). */
  profiles: ReadonlyMap<string, IoSourceProfile>;
  disabled: boolean;
  onCommit: (next: JsonValue) => void;
  onClear: () => void;
  onInvalidChange?: (message: string | null) => void;
}) {
  const parsed = parseMockSimulation(value);
  if (!parsed.ok) {
    return (
      <div className="interrupt-condition-editor" aria-label="Mock simulation">
        <p className="program-form-error inline-message severity-error" role="alert">{parsed.problem}</p>
        <button type="button" className="text-button" disabled={disabled} onClick={onClear}>Clear Mock simulation</button>
      </div>
    );
  }
  const { rows } = parsed;
  const problems = mockSimulationRowProblems(rows, signals);
  const save = (next: MockSimulationRow[]) => (next.length === 0 ? onClear() : onCommit(serializeMockSimulation(next)));
  const replace = (index: number, row: MockSimulationRow | null) => save(row === null
    ? rows.filter((_, at) => at !== index)
    : rows.map((item, at) => (at === index ? row : item)));
  return (
    <div className="interrupt-condition-editor" aria-label="Mock simulation">
      <p className="program-form-hint">
        Mock I/O only: each node start (incl. retries) sets the start value and plays the change; a change still pending when the node ends is cancelled. The manual toggle still works; the last change wins.
      </p>
      {signals.length === 0 && (
        <p className="muted">This node reads no input signal yet.</p>
      )}
      {signals.map((signal) => {
        const id = mockSignalId(signal);
        const label = `${signal.component_id} · ${signal.signal_name}`;
        const index = rows.findIndex((row) => mockSignalId(row.signal) === id);
        const row = index < 0 ? null : rows[index];
        const profile = profiles.get(id);
        const inactive = profile !== undefined && profile !== 'mock';
        const set = (next: MockSimulationRow | null) => (index < 0
          ? (next === null ? undefined : save([...rows, next]))
          : replace(index, next));
        return (
          <div className="interrupt-condition-compare" role="group" aria-label={`Mock ${label}`} key={id}>
            <code>{label}</code>
            <select aria-label={`${label} start value`} value={row === null ? '' : String(row.startValue)}
              disabled={disabled}
              onChange={(event) => set(event.target.value === '' ? null
                : { ...(row ?? { signal }), startValue: event.target.value === 'true' })}>
              <option value="">Not simulated</option>
              <option value="false">Start off</option>
              <option value="true">Start on</option>
            </select>
            {row !== null && (
              <select aria-label={`${label} change`} value={row.change === undefined ? 'none' : 'change'}
                disabled={disabled}
                onChange={(event) => {
                  const start = { signal: row.signal, startValue: row.startValue };
                  set(event.target.value === 'none' ? start
                    : { ...start, change: { afterMs: 0, value: !row.startValue } });
                }}>
                <option value="none">No change</option>
                <option value="change">Change after</option>
              </select>
            )}
            {row?.change !== undefined && (
              <>
                <DraftTextInput
                  aria-label={`${label} change after (ms)`}
                  inputMode="numeric"
                  size={6}
                  value={row.change.afterMs}
                  disabled={disabled}
                  validate={afterMsProblem}
                  onInvalidChange={onInvalidChange}
                  onCommit={(text) => set({ ...row, change: { ...row.change!, afterMs: Number(text.trim()) } })}
                />
                <span>ms to</span>
                <select aria-label={`${label} change value`} value={String(row.change.value)} disabled={disabled}
                  onChange={(event) => set({ ...row, change: { ...row.change!, value: event.target.value === 'true' } })}>
                  <option value="false">off</option>
                  <option value="true">on</option>
                </select>
              </>
            )}
            {inactive && (
              <span className="muted" data-mock-inactive>
                Inactive: {IO_SOURCE_PROFILE_LABELS[profile]} source, not Mock
              </span>
            )}
          </div>
        );
      })}
      {rows.map((_, index) => (problems.has(index) ? (
        <div className="interrupt-condition-compare" role="group" aria-label={`Mock row ${index + 1}`} key={`stale-${index}`}>
          <p className="program-form-error inline-message severity-error" role="alert">{problems.get(index)}</p>
          <button type="button" className="text-button" disabled={disabled}
            aria-label={`Remove Mock row ${index + 1}`} onClick={() => replace(index, null)}>Remove</button>
        </div>
      ) : null))}
    </div>
  );
}
