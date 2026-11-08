import { useCallback, useEffect, useState } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import { errorMessage } from '../state/structuralResource';
import type {
  JsonValue,
  PersistentKeyValue,
  PersistentKeyValues,
  ProgramDocument,
  ProgramFrameReference,
} from '../types/api';
import type { FrameScope } from '../utils/programFrames';
import {
  BlackboardValueEditor,
  blackboardValueDraft,
  blackboardValueDraftIssue,
  blackboardValueEditable,
  formatBlackboardValue,
  parseBlackboardValueDraft,
} from './BlackboardValueEditor';

type Confirmation =
  | { kind: 'edit'; key: string; draft: string; poseIssue: string | null }
  | { kind: 'reset'; key: string }
  | { kind: 'clear' };

function initialLabel(row: PersistentKeyValue): string {
  return row.initial === undefined
    ? 'empty'
    : formatBlackboardValue(row.type, row.initial);
}

/**
 * What a stored pose is chosen from: the Applied Program's Scene and key
 * declarations (each key's `allowed_frames`) and the frames its binding sealed.
 */
export interface PersistentKeyFrameProgram {
  program: Pick<ProgramDocument, 'scene' | 'blackboard'>;
  /** The Applied binding's frames (`frame_bindings[].reference`). */
  frames: readonly ProgramFrameReference[];
}

function keyFrameScope(
  source: PersistentKeyFrameProgram | undefined,
  key: string,
): FrameScope | undefined {
  if (source === undefined) return undefined;
  const allowed = source.program.blackboard?.keys.find((row) => row.key === key)?.allowed_frames;
  return {
    program: source.program,
    frames: source.frames,
    ...(allowed === undefined ? {} : { allowedFrames: allowed }),
  };
}

export interface PersistentKeyValuesViewProps {
  values: PersistentKeyValues;
  sceneResourceIds: readonly string[];
  frameProgram?: PersistentKeyFrameProgram;
  pending: boolean;
  error: string | null;
  onSet: (key: string, value: JsonValue) => Promise<boolean>;
  onReset: (key: string) => Promise<boolean>;
  onClear: () => Promise<boolean>;
  onRefresh: () => void;
}

/**
 * Stored values of the Applied Scenario's persistent keys. Every change is a
 * value edit confirmed here and applied at once by the Program runner; the
 * Program itself never changes.
 */
export function PersistentKeyValuesView({
  values,
  sceneResourceIds,
  frameProgram,
  pending,
  error,
  onSet,
  onReset,
  onClear,
  onRefresh,
}: PersistentKeyValuesViewProps) {
  const [confirmation, setConfirmation] = useState<Confirmation | null>(null);
  const liveRun = values.programRunId;
  // Clear is one Reset per writable key; a read key's stored value stays.
  const anyWritableStored = values.keys.some((row) => row.stored && row.access !== 'read');
  const finish = (accepted: boolean) => {
    if (accepted) setConfirmation(null);
  };
  return (
    <section className="runtime-control-section" aria-labelledby="stored-values-title">
      <div className="runtime-control-heading">
        <div>
          <span title={values.projectPath}>Kept between runs · Scenario {values.scenarioId}</span>
          <h3 id="stored-values-title">Stored values</h3>
        </div>
        <button type="button" className="quiet-button" disabled={pending} onClick={onRefresh}>
          Refresh
        </button>
      </div>
      {liveRun !== null && (
        <p className="hint">
          Run <code>{liveRun}</code> holds these values; edits go through it.
        </p>
      )}
      {error !== null && <p className="inline-message severity-error" role="alert">{error}</p>}
      {values.storageFault !== null && (
        <p className="inline-message severity-error" role="alert">
          Stored values could not follow the last committed Apply
          ({values.storageFault.code}); none are shown or restored until they can.
          {' '}{values.storageFault.message}
        </p>
      )}
      {values.applyObservationFault !== null && (
        <p className="inline-message severity-error" role="alert">
          A committed Apply of this Scenario was never observed by the Program runner
          ({values.applyObservationFault.code}): stored values were kept and checked against
          the current Program. Find and fix the missed notification.
          {values.applyObservationFault.recordDigests.map((digest) => (
            <code key={digest}> {digest}</code>
          ))}
        </p>
      )}
      {values.keys.map((row) => {
        const editing = confirmation?.kind === 'edit' && confirmation.key === row.key
          ? confirmation : null;
        const resetting = confirmation?.kind === 'reset' && confirmation.key === row.key;
        const editable = blackboardValueEditable(row.type) && row.access !== 'read';
        const issue = editing === null
          ? null
          : editing.poseIssue ?? blackboardValueDraftIssue(row.type, editing.draft, sceneResourceIds);
        return (
          <div key={row.key} className="runtime-control-record" data-stored-key={row.key}>
            <span>
              <code>{row.key}</code> · {row.type}
              {' · '}
              {row.stored
                ? <>stored <strong>{formatBlackboardValue(row.type, row.value)}</strong></>
                : <>not stored, starts from {initialLabel(row)}</>}
            </span>
            {editing !== null ? (
              <>
                <BlackboardValueEditor
                  id={`stored-value-${row.key}`}
                  label={`New stored value (${row.type})`}
                  type={row.type}
                  draft={editing.draft}
                  sceneResourceIds={sceneResourceIds}
                  frameScope={keyFrameScope(frameProgram, row.key)}
                  disabled={pending}
                  onDraftChange={(draft) => setConfirmation({ ...editing, draft })}
                  onInvalidChange={(poseIssue) => setConfirmation((current) => (
                    current?.kind === 'edit' && current.key === row.key
                      ? { ...current, poseIssue }
                      : current
                  ))}
                />
                {issue !== null && <p className="inline-message severity-error">{issue}</p>}
                <div className="runtime-control-actions">
                  <button
                    type="button"
                    disabled={pending || issue !== null}
                    onClick={() => {
                      const value = parseBlackboardValueDraft(row.type, editing.draft, sceneResourceIds);
                      void onSet(row.key, value).then(finish);
                    }}
                  >
                    Confirm
                  </button>
                  <button type="button" onClick={() => setConfirmation(null)}>Cancel</button>
                </div>
              </>
            ) : resetting ? (
              <>
                <p>Forget the stored value? The key starts again from {initialLabel(row)}.</p>
                <div className="runtime-control-actions">
                  <button type="button" disabled={pending} onClick={() => { void onReset(row.key).then(finish); }}>
                    Confirm reset
                  </button>
                  <button type="button" onClick={() => setConfirmation(null)}>Cancel</button>
                </div>
              </>
            ) : (
              <div className="runtime-control-actions">
                <button
                  type="button"
                  disabled={pending || !editable}
                  title={editable ? undefined : row.access === 'read'
                    ? 'Declared read-only: operator writes are refused.'
                    : `${row.type} values are written only by Program nodes.`}
                  onClick={() => setConfirmation({
                    kind: 'edit',
                    key: row.key,
                    draft: blackboardValueDraft(
                      row.type,
                      row.stored ? row.value : row.initial,
                      sceneResourceIds,
                    ),
                    poseIssue: null,
                  })}
                >
                  Edit
                </button>
                <button
                  type="button"
                  disabled={pending || row.access === 'read' || (!row.stored && liveRun === null)}
                  title={row.access === 'read'
                    ? 'Declared read-only: operator changes are refused.'
                    : !row.stored && liveRun === null ? 'Nothing is stored for this key.' : undefined}
                  onClick={() => setConfirmation({ kind: 'reset', key: row.key })}
                >
                  Reset to initial
                </button>
              </div>
            )}
          </div>
        );
      })}
      {confirmation?.kind === 'clear' ? (
        <div className="runtime-control-record" role="group" aria-label="Confirm clearing stored values">
          <p>
            Clear the stored values of the writable keys of Scenario {values.scenarioId}?
            Each starts again from its initial value, or empty if it has none; read keys
            are untouched.
          </p>
          <div className="runtime-control-actions">
            <button type="button" className="danger-button" disabled={pending} onClick={() => { void onClear().then(finish); }}>
              Clear stored values
            </button>
            <button type="button" onClick={() => setConfirmation(null)}>Cancel</button>
          </div>
        </div>
      ) : (
        <div className="runtime-control-actions">
          <button
            type="button"
            disabled={pending || !anyWritableStored || liveRun !== null}
            title={liveRun !== null
              ? 'A run holds these values: reset keys one by one, or clear after the run.'
              : anyWritableStored ? undefined : 'No writable key has a stored value.'}
            onClick={() => setConfirmation({ kind: 'clear' })}
          >
            Clear stored values
          </button>
        </div>
      )}
      {values.edits.length > 0 && (
        <details>
          <summary>Recent stored-value edits ({values.edits.length})</summary>
          <ol className="runtime-control-audit">
            {values.edits.map((edit) => (
              <li key={edit.sequence}>
                <code>{edit.sequence}</code> {edit.action} {edit.key ?? 'all keys'} · {edit.sourceLabel}
                {edit.reason !== null && <> · <code>{edit.reason}</code></>}
                {edit.detail !== null && <> · {edit.detail}</>}
                {edit.nodeInstanceId !== null && <> · node <code>{edit.nodeInstanceId}</code></>}
                {edit.programRunId !== null && <> · run <code>{edit.programRunId}</code></>}
              </li>
            ))}
          </ol>
        </details>
      )}
    </section>
  );
}

/** One stored value a Reset with Reload initial scene returns to `initial:`. */
export interface PersistentKeyResetRow {
  key: string;
  current: string;
  next: string;
}

/**
 * The stored values that Reload initial scene resets, as the Reset
 * confirmation lists them; a value already equal to `initial:` is not a change.
 */
export function persistentKeyResetRows(values: PersistentKeyValues): PersistentKeyResetRow[] {
  return values.keys
    .filter((row) => row.stored && JSON.stringify(row.value) !== JSON.stringify(row.initial))
    .map((row) => ({
      key: row.key,
      current: formatBlackboardValue(row.type, row.value),
      next: initialLabel(row),
    }));
}

export function PersistentKeyResetList({ rows }: { rows: readonly PersistentKeyResetRow[] }) {
  if (rows.length === 0) return null;
  return (
    <div>
      <p>Stored values reset to their initial values:</p>
      <ul>
        {rows.map((row) => (
          <li key={row.key}><code>{row.key}</code>: {row.current} → {row.next}</li>
        ))}
      </ul>
    </div>
  );
}

/** Reads the stored values for the Reset confirmation while Reload initial scene is chosen. */
export function PersistentKeyResetNotice({
  client,
}: {
  client: ProgramRuntimeController['persistentValues'];
}) {
  const [rows, setRows] = useState<PersistentKeyResetRow[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    const controller = new AbortController();
    client.read(controller.signal)
      .then((values) => {
        if (!controller.signal.aborted) setRows(persistentKeyResetRows(values));
      })
      .catch((reason: unknown) => {
        if (!controller.signal.aborted) setError(errorMessage(reason, 'Stored values are unavailable.'));
      });
    return () => controller.abort();
  }, [client]);
  if (error !== null) {
    return <p className="hint">Stored values could not be read ({error}); persistent keys are still reset to their initial values.</p>;
  }
  return rows === null ? null : <PersistentKeyResetList rows={rows} />;
}

export interface PersistentKeyValuesSectionProps {
  /** The Applied Scenario project folder: the values follow it. */
  projectPath: string;
  /** Re-read when the Applied Program or the live run changes. */
  appliedRevision: string;
  runId: string | null;
  sceneResourceIds: readonly string[];
  frameProgram?: PersistentKeyFrameProgram;
  client: ProgramRuntimeController['persistentValues'];
}

/** Reads and edits the stored values through the Program runner. */
export function PersistentKeyValuesSection({
  projectPath,
  appliedRevision,
  runId,
  sceneResourceIds,
  frameProgram,
  client,
}: PersistentKeyValuesSectionProps) {
  const [values, setValues] = useState<PersistentKeyValues | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [readSequence, setReadSequence] = useState(0);

  useEffect(() => {
    const controller = new AbortController();
    client.read(controller.signal)
      .then((next) => {
        if (controller.signal.aborted) return;
        setValues(next);
        setError(null);
      })
      .catch((reason: unknown) => {
        if (!controller.signal.aborted) {
          setError(errorMessage(reason, 'Stored values are unavailable.'));
        }
      });
    return () => controller.abort();
  }, [appliedRevision, client, projectPath, readSequence, runId]);

  const perform = useCallback(async (
    action: () => Promise<PersistentKeyValues | null>,
    fallback: string,
  ): Promise<boolean> => {
    setPending(true);
    try {
      const next = await action();
      if (next === null) setReadSequence((current) => current + 1);
      else setValues(next);
      setError(null);
      return true;
    } catch (reason) {
      setError(errorMessage(reason, fallback));
      return false;
    } finally {
      setPending(false);
    }
  }, []);

  if (values === null) {
    return error === null ? null : (
      <section className="runtime-control-section" aria-label="Stored values">
        <p className="inline-message severity-error" role="alert">{error}</p>
      </section>
    );
  }
  return (
    <PersistentKeyValuesView
      values={values}
      sceneResourceIds={sceneResourceIds}
      frameProgram={frameProgram}
      pending={pending}
      error={error}
      onSet={(key, value) => perform(
        () => client.edit(values, key, value),
        'The stored value was refused.',
      )}
      onReset={(key) => perform(
        () => client.edit(values, key),
        'The stored value could not be reset.',
      )}
      onClear={() => perform(
        async () => { await client.clear(values); return null; },
        'The stored values could not be cleared.',
      )}
      onRefresh={() => setReadSequence((current) => current + 1)}
    />
  );
}
