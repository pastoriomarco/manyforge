import { useEffect, useMemo, useRef, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { DraftTextInput } from './common/DraftInputs';
import {
  IO_DRIVER_FRESHNESS_NOTE,
  IO_LIVENESS_UNPROVEN_WARNING,
  IO_OUTPUT_PROFILE_LABELS,
  IO_SOURCE_PROFILE_LABELS,
  inputLivenessMode,
  ioSignalRows,
  ioSignalRowsValue,
  ioSignalsConfiguration,
  msTextToNs,
  newIoSignalInput,
  rosNameProblem,
  rosServiceNameProblem,
  validateIoSignalRows,
  withIoDirection,
  type IoOutputProfile,
  type IoSignalInputDraft,
  type IoSignalOutputDraft,
  type IoSignalRow,
  type IoSourceProfile,
  type RosBoolLiveness,
} from '../utils/ioSignalConfiguration';

/** The runtime configuration facts this section needs (a focused-editor row). */
export interface IoSignalRuntimeConfiguration {
  index: number;
  runtimeUnitId: string;
  configuration: Record<string, JsonValue>;
  configurationPath: Array<string | number>;
  /** Intent v2: the component itself, a sensible default owner of new rows. */
  intentComponent: boolean;
  /** An I/O device component (runtime_kind io_provider): own rows only, ≥1 row. */
  ioProvider?: boolean;
}

/** One motion robot offered to geometric-contact authoring. */
export interface GeometricContactRobotOption {
  componentId: string;
}

type NewIoSignalRow = Extract<IoSignalRow, { kind: 'input' | 'output' }>;

/**
 * Declare the boolean input and output signals of one runtime unit. The whole
 * `io_signals` array commits through the ordinary Deployment Draft mutation.
 * New signals are added only to I/O devices: ROS-connected I/O,
 * including I/O physically on a robot, is its own I/O device component, so an
 * arm's runtime only lists (and may remove) rows it already declares.
 */
export function IoSignalConfigurationSection({
  controller,
  runtimes,
  geometricContactRobots = [],
  sceneResourceIds = [],
}: {
  controller: DeploymentWorkspaceController;
  runtimes: IoSignalRuntimeConfiguration[];
  /** Motion robots used to select a dynamic geometric-contact source. */
  geometricContactRobots?: readonly GeometricContactRobotOption[];
  sceneResourceIds?: readonly string[];
}) {
  const [selected, setSelected] = useState(0);
  // Arms are listed only while they still declare rows (to review/remove them).
  const listed = useMemo(() => runtimes.filter((candidate) => (
    candidate.ioProvider === true
    || Array.isArray(ioSignalsConfiguration(candidate.configuration, candidate.configurationPath).configuration.io_signals)
  )), [runtimes]);
  const runtime = listed.find((candidate) => candidate.index === selected) ?? listed[0] ?? null;
  const canAdd = runtime?.ioProvider === true;
  const location = useMemo(
    () => (runtime === null ? null : ioSignalsConfiguration(runtime.configuration, runtime.configurationPath)),
    [runtime],
  );
  const source = useMemo(() => ioSignalRows(location?.configuration ?? null), [location]);
  const [rows, setRows] = useState<IoSignalRow[]>(source);
  const [error, setError] = useState<string | null>(null);
  const [adding, setAdding] = useState<NewIoSignalRow | null>(null);
  const [addError, setAddError] = useState<string | null>(null);
  const sourceFingerprint = JSON.stringify(source);
  const previousSource = useRef(sourceFingerprint);
  useEffect(() => {
    if (sourceFingerprint === previousSource.current) return;
    previousSource.current = sourceFingerprint;
    setRows(source);
    setError(null);
    setAdding(null);
    setAddError(null);
  }, [source, sourceFingerprint]);
  const targetsFor = (componentId: string) => [...new Set(source.flatMap((row) => {
    const owner = row.kind === 'preserved' ? row.raw.controllable_component_id : row.draft.componentId;
    const target = row.kind === 'preserved' ? row.raw.command_target_id : row.draft.targetId;
    return owner === componentId && typeof target === 'string' && target !== '' ? [target] : [];
  }))].sort();
  const targets = targetsFor(runtime?.runtimeUnitId ?? '');
  const outputsFor = (componentId: string) => rows.flatMap((row) => row.kind === 'output'
    && row.draft.profile === 'mock_output' && row.draft.componentId === componentId
    && row.draft.signalName !== '' ? [row.draft.signalName] : []);
  const disabled = !controller.mutationAvailable || controller.busy;
  const removeDisabledReason = runtime?.ioProvider === true && rows.length === 1
    ? 'An I/O device needs at least one signal; remove the device instead.' : null;
  const declared = location !== null && Array.isArray(location.configuration.io_signals);
  const commitRows = async (nextRows: IoSignalRow[], declaredBefore = declared) => {
    if (location === null) return false;
    const problem = validateIoSignalRows(nextRows, runtime?.ioProvider ? runtime.runtimeUnitId : undefined);
    if (problem !== null) { setError(problem); return false; }
    setError(null);
    if (nextRows.length === 0) {
      if (declaredBefore && await controller.mutate(location.path, 'remove') === null) {
        setRows(source);
        return false;
      }
      return true;
    }
    if (JSON.stringify(ioSignalRowsValue(nextRows)) !== JSON.stringify(ioSignalRowsValue(source))) {
      if (await controller.mutate(location.path, 'set', ioSignalRowsValue(nextRows)) === null) {
        setRows(source);
        return false;
      }
    }
    return true;
  };
  const editRows = (nextRows: IoSignalRow[]) => {
    setRows(nextRows);
    const problem = validateIoSignalRows(nextRows, runtime?.ioProvider ? runtime.runtimeUnitId : undefined);
    setError(problem);
    if (problem === null) void commitRows(nextRows);
  };
  const updateRow = (index: number, patch: Partial<IoSignalInputDraft>) => {
    editRows(rows.map((row, rowIndex) => (
      rowIndex === index && row.kind === 'input' ? { kind: 'input', draft: { ...row.draft, ...patch } } : row
    )));
  };
  const updateOutput = (index: number, patch: Partial<IoSignalOutputDraft>) => {
    editRows(rows.map((row, rowIndex) => (
      rowIndex === index && row.kind === 'output' ? { kind: 'output', draft: { ...row.draft, ...patch } } : row
    )));
  };
  const setDirection = (index: number, direction: 'input' | 'output') => {
    editRows(rows.map((row, rowIndex) => (
      rowIndex === index && row.kind !== 'preserved' ? withIoDirection(row, direction) : row
    )));
  };
  const removeRow = (index: number) => {
    const next = rows.filter((_row, rowIndex) => rowIndex !== index);
    const problem = validateIoSignalRows(next, runtime?.ioProvider ? runtime.runtimeUnitId : undefined);
    if (problem !== null) { setError(problem); return; }
    setRows(next);
    void commitRows(next);
  };
  const beginAdd = (direction: 'input' | 'output') => {
    const input = {
      kind: 'input' as const,
      draft: { ...newIoSignalInput(runtime?.runtimeUnitId ?? ''),
        targetId: targets.length === 1 ? targets[0] : '' },
    };
    setAdding((direction === 'input' ? input : withIoDirection(input, 'output')) as NewIoSignalRow);
    setAddError(null);
  };
  const updateAdding = (patch: Partial<IoSignalInputDraft | IoSignalOutputDraft>) => {
    if (adding === null) return;
    setAdding(adding.kind === 'input'
      ? { kind: 'input', draft: { ...adding.draft, ...patch } as IoSignalInputDraft }
      : { kind: 'output', draft: { ...adding.draft, ...patch } as IoSignalOutputDraft });
    setAddError(null);
  };
  const changeAddingDirection = (direction: 'input' | 'output') => {
    if (adding !== null) setAdding(withIoDirection(adding, direction) as NewIoSignalRow);
    setAddError(null);
  };
  const addRow = async () => {
    if (adding === null) return;
    const next = [...rows, adding];
    const problem = validateIoSignalRows(next, runtime?.ioProvider ? runtime.runtimeUnitId : undefined);
    if (problem !== null) { setAddError(problem); return; }
    if (await commitRows(next)) {
      setRows(next);
      setAdding(null);
      setAddError(null);
    }
  };

  return (
    <details className="dcard">
      <summary>I/O signals</summary>
      <div className="deployment-focused-body">
        {runtime === null ? <p className="muted">No I/O device is declared. Add one in I/O devices to declare signals.</p> : (
          <>
            {listed.length > 1 && (
              <label className="deployment-focused-field">
                <span>Runtime unit</span>
                <select aria-label="I/O runtime unit" value={runtime.index} disabled={controller.busy}
                  onChange={(event) => setSelected(Number(event.target.value))}>
                  {listed.map((candidate) => (
                    <option key={candidate.index} value={candidate.index}>{candidate.runtimeUnitId}</option>
                  ))}
                </select>
              </label>
            )}
            {rows.length === 0 && adding === null && <p className="muted">No I/O signals are declared for {runtime.runtimeUnitId}.</p>}
            {rows.map((row, index) => (
              row.kind === 'preserved' ? (
                <fieldset className="deployment-gripper-editor" key={`preserved-${index}`}>
                  <legend>{String(row.raw.signal_name ?? `Signal ${index + 1}`)} · {String(row.raw.direction ?? 'unknown')}</legend>
                  <p className="field-help">{row.reason}</p>
                  <div className="button-row">
                    <button type="button" disabled={disabled || removeDisabledReason !== null}
                      title={removeDisabledReason ?? undefined}
                      onClick={() => removeRow(index)}>Remove signal</button>
                  </div>
                </fieldset>
              ) : row.kind === 'output' ? (
                <IoSignalOutputFields
                  key={`output-${index}`}
                  index={index}
                  draft={row.draft}
                  disabled={disabled}
                  ioProvider={runtime.ioProvider === true}
                  removeDisabledReason={removeDisabledReason}
                  targetOptions={targetsFor(row.draft.componentId)}
                  onChange={(patch) => updateOutput(index, patch)}
                  onDirection={(direction) => setDirection(index, direction)}
                  onRemove={() => removeRow(index)}
                />
              ) : (
                <IoSignalInputFields
                  key={`input-${index}`}
                  index={index}
                  draft={row.draft}
                  disabled={disabled}
                  ioProvider={runtime.ioProvider === true}
                  removeDisabledReason={removeDisabledReason}
                  geometricContactRobots={geometricContactRobots}
                  targetOptions={targetsFor(row.draft.componentId)}
                  sceneResourceIds={sceneResourceIds} outputSignalNames={outputsFor(row.draft.componentId)}
                  onChange={(patch) => updateRow(index, patch)}
                  onDirection={(direction) => setDirection(index, direction)}
                  onRemove={() => removeRow(index)}
                />
              )
            ))}
            {adding !== null && (
              adding.kind === 'output' ? (
                <IoSignalOutputFields index={source.length} draft={adding.draft} disabled={disabled}
                  ioProvider={runtime.ioProvider === true}
                  targetOptions={targetsFor(adding.draft.componentId)} error={addError}
                  onChange={updateAdding} onDirection={changeAddingDirection} />
              ) : (
                <IoSignalInputFields index={source.length} draft={adding.draft} disabled={disabled}
                  ioProvider={runtime.ioProvider === true}
                  geometricContactRobots={geometricContactRobots}
                  targetOptions={targetsFor(adding.draft.componentId)} sceneResourceIds={sceneResourceIds}
                  outputSignalNames={outputsFor(adding.draft.componentId)} error={addError}
                  onChange={updateAdding} onDirection={changeAddingDirection} />
              )
            )}
            {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
            {!canAdd && (
              <p className="field-help">ROS-connected I/O (ROS Bool / SetBool, ros2_control GPIO) is its own I/O device: add it in I/O devices; a machine's own I/O banks are its I/O members. Signals listed here on {runtime.runtimeUnitId} can only be reviewed or removed.</p>
            )}
            <div className="button-row">
              {canAdd && adding === null && <button type="button" disabled={disabled}
                onClick={() => beginAdd('input')}>Add input signal</button>}
              {canAdd && adding === null && <button type="button" disabled={disabled}
                onClick={() => beginAdd('output')}>Add output signal</button>}
              {adding !== null && <>
                <button type="button" className="primary-action" disabled={disabled}
                  onClick={() => { void addRow(); }}>Add signal</button>
                <button type="button" disabled={disabled}
                  onClick={() => { setAdding(null); setAddError(null); }}>Cancel</button>
              </>}
            </div>
            <p className="field-help">Boolean signals. Mock inputs and outputs are simulated sources for offline tests; ROS Bool and ros2_control GPIO inputs read real topics. ROS SetBool writes complete when the service accepts them; ros2_control GPIO writes are submitted only; none is confirmed by a physical readback.</p>
          </>
        )}
      </div>
    </details>
  );
}

function DirectionField({
  label,
  value,
  disabled,
  onDirection,
}: {
  label: string;
  value: 'input' | 'output';
  disabled: boolean;
  onDirection: (direction: 'input' | 'output') => void;
}) {
  return (
    <label className="deployment-focused-field">
      <span>Direction</span>
      <select aria-label={`${label} Direction`} value={value} disabled={disabled}
        onChange={(event) => onDirection(event.target.value as 'input' | 'output')}>
        <option value="input">Input</option>
        <option value="output">Output</option>
      </select>
    </label>
  );
}

function ChoiceField({ label, name, value, options, disabled, onChange, readOnly = false }: {
  label: string; name: string; value: string; options: readonly string[];
  disabled: boolean; onChange: (value: string) => void; readOnly?: boolean;
}) {
  const values = [...new Set(options.filter(Boolean))].sort();
  return <label className="deployment-focused-field">
    <span>{name}</span>
    <select aria-label={`${label} ${name}`} title={`Select the ${name.toLowerCase()} for this signal.`}
      value={value} className={readOnly ? 'io-signal-readonly' : undefined} disabled={disabled || readOnly} onChange={(event) => onChange(event.target.value)}>
      <option value="">Select {name.toLowerCase()}</option>
      {value !== '' && !values.includes(value) && <option value={value}>{value} (not currently listed)</option>}
      {values.map((item) => <option key={item} value={item}>{item}</option>)}
    </select>
  </label>;
}

const PORTABLE_SIGNAL_ID = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/;

function signalIdProblem(value: string, name: string) {
  return PORTABLE_SIGNAL_ID.test(value) ? null
    : `The ${name} must be 1–128 letters, digits, '.', '_' or '-', starting with a letter or digit.`;
}

function inputFieldProblem(
  draft: IoSignalInputDraft,
  field: keyof IoSignalInputDraft,
  value: string,
): string | null {
  if (field === 'componentId' || field === 'initialValue' || field === 'echoEnabled' || field === 'explicit') return null;
  if (['targetId', 'resourceRoleId', 'signalName', 'sourceId', 'robotComponentId',
    'targetSceneResourceId', 'echoOutputSignalName'].includes(field)) {
    const names: Partial<Record<keyof IoSignalInputDraft, string>> = {
      targetId: 'target', resourceRoleId: 'resource role', signalName: 'signal name', sourceId: 'source',
      robotComponentId: 'robot', targetSceneResourceId: 'target Scene resource id',
      echoOutputSignalName: 'Mock output signal name',
    };
    return signalIdProblem(value, names[field] ?? 'value');
  }
  if (field === 'maximumAgeMs') {
    const ns = msTextToNs(value);
    return ns !== null && ns >= 1 ? null : 'The maximum sample age must be above 0 ms, with at most 6 decimals.';
  }
  if (field === 'ingressDepth') {
    const depth = /^\d+$/.test(value) ? Number(value) : NaN;
    return Number.isSafeInteger(depth) && depth >= 1 ? null
      : `Ingress depth must be a whole number of samples from 1 to ${Number.MAX_SAFE_INTEGER}.`;
  }
  if (field === 'echoDelayMs') {
    const delay = /^\d+$/.test(value) ? Number(value) : NaN;
    return Number.isSafeInteger(delay) && delay <= 9_007_199_254 ? null
      : 'The Mock echo delay must be a whole number of milliseconds from 0 to 9007199254.';
  }
  if (field === 'topic') {
    if (draft.profile === 'ros2_control_gpio') return rosNameProblem(value, 'GPIO topic');
    return value.trim() === '' ? 'Enter the source topic.' : null;
  }
  if (field === 'group') return value !== '' && !value.includes('\0') ? null : 'Enter the GPIO group.';
  if (field === 'interfaceName') return value !== '' && !value.includes('\0') ? null : 'Enter the GPIO interface name.';
  return null;
}

function outputFieldProblem(
  field: keyof IoSignalOutputDraft,
  value: string,
): string | null {
  if (field === 'componentId') return null;
  if (field === 'targetId' || field === 'resourceRoleId' || field === 'signalName' || field === 'sourceId') {
    const names: Partial<Record<keyof IoSignalOutputDraft, string>> = {
      targetId: 'target', resourceRoleId: 'resource role', signalName: 'signal name', sourceId: 'source',
    };
    return signalIdProblem(value, names[field] ?? 'value');
  }
  if (field === 'service') return rosServiceNameProblem(value);
  if (field === 'channel') return signalIdProblem(value, 'Mock output channel');
  if (field === 'topic') return rosNameProblem(value, 'GPIO topic');
  if (field === 'group') return value !== '' && !value.includes('\0') ? null : 'Enter the GPIO group.';
  if (field === 'interfaceName') return value !== '' && !value.includes('\0') ? null : 'Enter the GPIO interface name.';
  return null;
}

function IoDraftTextField({ label, ariaLabel = label, value, disabled, validate, onCommit, help, readOnly = false, inputMode, unit }: {
  label: string;
  ariaLabel?: string;
  value: string;
  disabled: boolean;
  validate: (value: string) => string | null;
  onCommit: (value: string) => void;
  help?: string;
  readOnly?: boolean;
  inputMode?: 'decimal';
  unit?: string;
}) {
  const [error, setError] = useState<string | null>(null);
  const input = <DraftTextInput aria-label={ariaLabel} value={value} title={help} disabled={disabled} readOnly={readOnly}
    className={readOnly ? 'io-signal-readonly' : undefined}
    inputMode={inputMode} validate={validate} onInvalidChange={setError} onCommit={onCommit} />;
  return <label className="deployment-focused-field">
    <span>{label}</span>
    {unit === undefined ? input : <span className="deployment-number-with-unit">{input}<small>{unit}</small></span>}
    {error !== null && <span className="deployment-field-error inline-message severity-error" role="alert">{error}</span>}
  </label>;
}

export function IoSignalOutputFields({
  index,
  draft,
  disabled,
  targetOptions = [],
  addDevice = false,
  removeDisabledReason = null,
  error = null,
  onChange,
  onDirection,
  onRemove,
}: {
  index: number;
  draft: IoSignalOutputDraft;
  disabled: boolean;
  /** An I/O device row: its component is the device itself (read-only). */
  ioProvider?: boolean;
  targetOptions?: readonly string[];
  addDevice?: boolean;
  removeDisabledReason?: string | null;
  error?: string | null;
  onChange: (patch: Partial<IoSignalOutputDraft>) => void;
  onDirection: (direction: 'input' | 'output') => void;
  onRemove?: () => void;
}) {
  const label = draft.signalName.trim() === '' ? `Signal ${index + 1}` : draft.signalName;
  const patchForText = (field: keyof IoSignalOutputDraft, value: string): Partial<IoSignalOutputDraft> => ({
    [field]: value,
    ...(field === 'signalName' && !addDevice && value !== '' ? {
      ...(draft.resourceRoleId === '' ? { resourceRoleId: value } : {}),
      ...(draft.sourceId === '' && draft.componentId !== '' ? { sourceId: `${draft.componentId}.${value}` } : {}),
    } : {}),
  });
  const text = (name: string, field: keyof IoSignalOutputDraft, help?: string) => {
    const fieldLabel = `${label} ${name}`;
    const patch = (value: string) => patchForText(field, value);
    if (addDevice) return <label className="deployment-focused-field" key={field}>
      <span>{name}</span>
      <input aria-label={fieldLabel} value={draft[field]} title={help}
        placeholder={addDevice && field === 'sourceId' ? `${draft.componentId}_source` : undefined}
        disabled={disabled} readOnly={field === 'componentId'} aria-invalid={error !== null}
        className={field === 'componentId' ? 'io-signal-readonly' : undefined}
        onChange={(event) => onChange({ [field]: event.target.value })}
        onBlur={field === 'signalName' && !addDevice ? () => {
          if (draft.signalName === '') return;
          onChange(patchForText('signalName', draft.signalName));
        } : undefined} />
    </label>;
    return <IoDraftTextField key={field} label={name} ariaLabel={fieldLabel} value={draft[field] as string} help={help} disabled={disabled}
      readOnly={field === 'componentId'} validate={(value) => outputFieldProblem(field, value)}
      onCommit={(value) => onChange(patch(value))} />;
  };
  return (
    <fieldset className="deployment-gripper-editor">
      <legend>{label} · bool output</legend>
      <div className="deployment-focused-form-grid">
        {text('Signal name', 'signalName')}
        {text('Component', 'componentId', 'This signal belongs to this component.')}
        {addDevice || targetOptions.length === 0 ? text('Target', 'targetId', 'The command target that carries this signal.') : <ChoiceField label={label} name="Target" value={draft.targetId}
          options={targetOptions} disabled={disabled} readOnly={targetOptions.length === 1 && draft.targetId === targetOptions[0]}
          onChange={(value) => onChange({ targetId: value })} />}
        {text('Resource role', 'resourceRoleId', 'The Program resource role this signal serves on the target.')}
        <DirectionField label={label} value="output" disabled={disabled} onDirection={onDirection} />
        <label className="deployment-focused-field">
          <span>Output profile</span>
          <select aria-label={`${label} Output profile`} value={draft.profile} disabled={disabled}
            onChange={(event) => onChange({ profile: event.target.value as IoOutputProfile })}>
            {(Object.keys(IO_OUTPUT_PROFILE_LABELS) as IoOutputProfile[]).map((profile) => (
              <option key={profile} value={profile}>{IO_OUTPUT_PROFILE_LABELS[profile]}</option>
            ))}
          </select>
        </label>
        {text('Source', 'sourceId', 'Signals sharing one source must agree on profile and service or topic.')}
        {draft.profile === 'mock_output' && text('Channel', 'channel', 'The simulated Mock output channel (portable id).')}
        {draft.profile === 'ros_set_bool' && text('Service', 'service',
          'Absolute, fully resolved ROS service name, e.g. /cell/valve/set.')}
        {draft.profile === 'ros2_control_gpio' && (
          <>
            {text('Topic', 'topic', 'Absolute, fully resolved GPIO command topic of the gpio command controller.')}
            {text('Group', 'group', 'The exact GPIO group name of the controller.')}
            {text('Interface name', 'interfaceName', 'The exact command interface name within the group.')}
          </>
        )}
      </div>
      <p className="field-help">
        {draft.profile === 'ros2_control_gpio'
          ? 'A write is submitted only: the gpio command controller gives no acknowledgement, so it has no acknowledgement timeout. Every command message carries all channels of the group ManyForge has written.'
          : `A write completes when the source accepts it (${draft.profile === 'mock_output' ? 'the Mock acknowledges it' : 'the SetBool response'}), not on the physical effect.`}
        {' '}To read the state back, declare a separate input signal.
      </p>
      {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
      {onRemove !== undefined && (
        <div className="button-row">
          <button type="button" disabled={disabled || removeDisabledReason !== null}
            title={removeDisabledReason ?? undefined} onClick={onRemove}>Remove signal</button>
        </div>
      )}
    </fieldset>
  );
}

export function IoSignalInputFields({
  index,
  draft,
  disabled,
  geometricContactRobots = [],
  targetOptions = [],
  sceneResourceIds = [],
  outputSignalNames = [],
  addDevice = false,
  removeDisabledReason = null,
  error = null,
  onChange,
  onDirection,
  onRemove,
}: {
  index: number;
  draft: IoSignalInputDraft;
  disabled: boolean;
  /** An I/O device row: component read-only. */
  ioProvider?: boolean;
  /** Motion robots used to select a dynamic geometric-contact source. */
  geometricContactRobots?: readonly GeometricContactRobotOption[];
  targetOptions?: readonly string[];
  sceneResourceIds?: readonly string[];
  outputSignalNames?: readonly string[];
  addDevice?: boolean;
  removeDisabledReason?: string | null;
  error?: string | null;
  onChange: (patch: Partial<IoSignalInputDraft>) => void;
  onDirection: (direction: 'input' | 'output') => void;
  onRemove?: () => void;
}) {
  const label = draft.signalName.trim() === '' ? `Signal ${index + 1}` : draft.signalName;
  const profiles = Object.keys(IO_SOURCE_PROFILE_LABELS) as IoSourceProfile[];
  const liveness = inputLivenessMode(draft);
  const robotOptions = [...geometricContactRobots]
    .sort((left, right) => left.componentId.localeCompare(right.componentId));
  const patchForText = (field: keyof IoSignalInputDraft, value: string): Partial<IoSignalInputDraft> => ({
    [field]: value,
    ...(field === 'signalName' && !addDevice && value !== '' ? {
      ...(draft.resourceRoleId === '' ? { resourceRoleId: value } : {}),
      ...(draft.sourceId === '' && draft.componentId !== '' ? { sourceId: `${draft.componentId}.${value}` } : {}),
    } : {}),
  });
  const text = (name: string, field: keyof IoSignalInputDraft, help?: string) => {
    const fieldLabel = `${label} ${name}`;
    const patch = (value: string) => patchForText(field, value);
    if (addDevice) return <label className="deployment-focused-field" key={field}>
      <span>{name}</span>
      <input aria-label={fieldLabel} value={draft[field] as string} title={help}
        placeholder={addDevice && field === 'sourceId' ? `${draft.componentId}_source` : undefined}
        disabled={disabled} readOnly={field === 'componentId'} aria-invalid={error !== null}
        className={field === 'componentId' ? 'io-signal-readonly' : undefined}
        onChange={(event) => onChange({ [field]: event.target.value })}
        onBlur={field === 'signalName' && !addDevice ? () => {
          if (draft.signalName === '') return;
          onChange(patchForText('signalName', draft.signalName));
        } : undefined} />
    </label>;
    return <IoDraftTextField key={field} label={name} ariaLabel={fieldLabel} value={draft[field] as string} help={help} disabled={disabled}
      readOnly={field === 'componentId'} validate={(value) => inputFieldProblem({ ...draft, ...patch(value) }, field, value)}
      onCommit={(value) => onChange(patch(value))} />;
  };
  const number = (name: string, field: keyof IoSignalInputDraft, unit: string, help?: string) => {
    const fieldLabel = `${label} ${name}`;
    if (addDevice) return <label className="deployment-focused-field" key={field}>
      <span>{name}</span><span className="deployment-number-with-unit">
        <input aria-label={fieldLabel} inputMode="decimal" value={draft[field] as string}
          disabled={disabled} title={help} onChange={(event) => onChange({ [field]: event.target.value })} />
        <small>{unit}</small>
      </span>
    </label>;
    return <IoDraftTextField key={field} label={name} ariaLabel={fieldLabel} value={draft[field] as string} help={help} disabled={disabled}
      inputMode="decimal" unit={unit}
      validate={(value) => inputFieldProblem({ ...draft, [field]: value }, field, value)}
      onCommit={(value) => onChange({ [field]: value })} />;
  };
  return (
    <fieldset className="deployment-gripper-editor">
      <legend>{label} · bool input</legend>
      <div className="deployment-focused-form-grid">
        {text('Signal name', 'signalName')}
        {text('Component', 'componentId', 'This signal belongs to this component.')}
        {addDevice || targetOptions.length === 0 ? text('Target', 'targetId', 'The command target that carries this signal.') : <ChoiceField label={label} name="Target" value={draft.targetId}
          options={targetOptions} disabled={disabled} readOnly={targetOptions.length === 1 && draft.targetId === targetOptions[0]}
          onChange={(value) => onChange({ targetId: value })} />}
        {text('Resource role', 'resourceRoleId', 'The Program resource role this signal serves on the target.')}
        <DirectionField label={label} value="input" disabled={disabled} onDirection={onDirection} />
        <label className="deployment-focused-field">
          <span>Source profile</span>
          <select aria-label={`${label} Source profile`} value={draft.profile} disabled={disabled}
            onChange={(event) => onChange({ profile: event.target.value as IoSourceProfile })}>
            {profiles.map((profile) => (
              <option key={profile} value={profile}>{IO_SOURCE_PROFILE_LABELS[profile]}</option>
            ))}
          </select>
        </label>
        {text('Source', 'sourceId', 'Signals sharing one source must agree on profile, topic, ingress depth and liveness.')}
        {draft.profile !== 'mock' && draft.profile !== 'geometric_contact'
          && text('Topic', 'topic', draft.profile === 'ros_bool' ? undefined
          : 'Absolute, fully resolved topic, e.g. /gpio_controller/gpio_states.')}
        {draft.profile === 'ros2_control_gpio' && text('Group', 'group', 'The exact GPIO group name.')}
        {draft.profile === 'ros2_control_gpio' && text('Interface name', 'interfaceName', 'The exact state interface name within the group.')}
        {draft.profile === 'ros_bool' && (
          <label className="deployment-focused-field">
            <span>Liveness</span>
            <select aria-label={`${label} Liveness`} value={draft.liveness} disabled={disabled}
              title="Periodic: the topic publishes at a steady rate, so a reading older than the maximum sample age is stale. Change only: it publishes only on change, so liveness is unproven between changes."
              onChange={(event) => onChange({ liveness: event.target.value as RosBoolLiveness })}>
              <option value="periodic">Periodic</option>
              <option value="change_only">Change only (liveness unproven)</option>
            </select>
          </label>
        )}
        {draft.profile === 'geometric_contact' && (
          <>
            {robotOptions.length > 0 ? (
              <label className="deployment-focused-field">
                <span>Robot</span>
                <select aria-label={`${label} Robot`} value={draft.robotComponentId}
                  disabled={disabled}
                  onChange={(event) => onChange({ robotComponentId: event.target.value })}>
                  <option value="">Select a robot</option>
                  {!robotOptions.some((option) => option.componentId === draft.robotComponentId)
                    && draft.robotComponentId !== ''
                    && <option value={draft.robotComponentId}>{draft.robotComponentId} (not currently listed)</option>}
                  {robotOptions.map((option) => (
                    <option key={option.componentId} value={option.componentId}>{option.componentId}</option>
                  ))}
                </select>
              </label>
            ) : (
              text('Robot component id', 'robotComponentId', 'The motion robot used by this contact input.')
            )}
            {sceneResourceIds.length === 0
              ? text('Target Scene resource', 'targetSceneResourceId', 'The object in the cell Scene that this input checks for contact.')
              : <ChoiceField label={label} name="Target Scene resource" value={draft.targetSceneResourceId}
                options={sceneResourceIds} disabled={disabled} onChange={(value) => onChange({ targetSceneResourceId: value })} />}
          </>
        )}
        {draft.profile === 'mock' ? (
          <>
            <label className="deployment-focused-field">
              <span>Initial value</span>
              <select aria-label={`${label} Initial value`} value={String(draft.initialValue)} disabled={disabled}
                onChange={(event) => onChange({ initialValue: event.target.value === 'true' })}>
                <option value="false">false</option>
                <option value="true">true</option>
              </select>
            </label>
            <label className="deployment-focused-field">
              <span>Echo a Mock output</span>
              <input type="checkbox" aria-label={`${label} Echo a Mock output`} checked={draft.echoEnabled}
                disabled={disabled} onChange={(event) => onChange({ echoEnabled: event.target.checked })} />
            </label>
            {draft.echoEnabled && (outputSignalNames.length === 0
              ? text('Output signal name', 'echoOutputSignalName', 'The Mock output that this input follows.')
              : <ChoiceField label={label} name="Output signal name" value={draft.echoOutputSignalName}
                options={outputSignalNames} disabled={disabled} onChange={(value) => onChange({ echoOutputSignalName: value })} />)}
            {draft.echoEnabled && number('Echo delay', 'echoDelayMs', 'ms',
              'The input follows the output after this delay.')}
          </>
        ) : liveness === 'periodic'
          ? number('Maximum sample age', 'maximumAgeMs', 'ms', 'The freshness tolerance: a reading older than this is stale.')
          : null}
      </div>
      <p className="field-help">
        Freshness: {liveness === 'in_process'
          ? 'provider validity (a Mock source is in process and never goes stale).'
          : liveness === 'periodic'
            ? 'periodic; the source is stale once its latest reading is older than the maximum sample age.'
            : 'provider validity only; no sample age applies.'}
        {' '}Transitions are sampled only.
      </p>
      {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
      {draft.profile === 'geometric_contact' && (
        <>
          <p className="field-help" data-geometric-contact-note>
            This input is true while the selected robot&apos;s contact bodies touch the selected Scene resource, checked
            against live geometry. If ManyForge can&apos;t make the check, the input is unavailable, never false. Contact
            bodies are the collision shapes on the robot, or on the mounted tool, that owns its Active TCP (the tool
            center point currently selected for that robot); a Scene resource is a named object in the cell&apos;s Scene.
          </p>
          <details className="field-help">
            <summary>How geometric contact works</summary>
            <p>
              At every sample, ManyForge reads the current Active TCP and collision geometry. A Program may read this
              signal, but it does not store those live values or the contact result.
            </p>
            <p>A live unavailable sample uses <code>io_source_unavailable</code>; its <code>reason</code> says why:</p>
            <ul>
              <li><code>active_tcp_unavailable</code>: the robot has no current Active TCP.</li>
              <li><code>active_tcp_unknown</code>: the current Active TCP is not in this robot&apos;s TCP list.</li>
              <li><code>tcp_tool_not_mounted</code>: the TCP&apos;s tool is not mounted on this robot.</li>
              <li><code>active_tcp_contact_bodies_unavailable</code>: no contact bodies are declared for
                the Active TCP&apos;s component.</li>
              <li>
                <code>runtime_motion_endpoint_body_unresolved</code>, <code>runtime_motion_endpoint_invalid</code>,
                <code>runtime_motion_endpoint_body_invalid</code>,
                <code>runtime_collision_body_alias_table_invalid</code>, or
                <code>runtime_collision_body_alias_unresolved</code>: the runtime cannot identify or
                validate a contact body in the robot&apos;s model.
              </li>
              <li><code>observed_state_missing_or_stale</code>: current robot or tool positions needed
                for the check are missing or past their freshness limit.</li>
              <li><code>collision_geometry_missing_or_stale</code>: current collision geometry for
                the robot or Scene resource is missing or stale.</li>
              <li><code>collision_geometry_unresolved</code>: ManyForge cannot match or prepare the
                contact bodies and Scene resource geometry for a complete check.</li>
              <li><code>collision_query_busy</code>: the previous contact check is still running.</li>
              <li><code>collision_query_inconclusive</code>: the geometry query cannot confirm
                contact or no contact.</li>
              <li><code>contact_bodies_static</code>: at least one contact body is static relative to
                this robot&apos;s motion group, so it does not move with the robot.</li>
            </ul>
            <p>
              Before the first sample the signal has no value. A valid sample becomes stale when it
              is older than Maximum sample age; setting that age below normal sample-production time
              can make it stale. This input is read-only: manual and timed Mock writes do not apply.
            </p>
          </details>
        </>
      )}
      {liveness === 'change_only' && (
        <p className="inline-message severity-warning" data-io-liveness-warning>{IO_LIVENESS_UNPROVEN_WARNING}</p>
      )}
      {draft.profile === 'ros2_control_gpio' && (
        <p className="field-help" data-io-driver-note>{IO_DRIVER_FRESHNESS_NOTE}</p>
      )}
      <details>
        <summary>Advanced</summary>
        <div className="deployment-focused-form-grid">
          {number('Ingress depth', 'ingressDepth', 'samples', 'Bounded samples queued per source (default 64).')}
        </div>
      </details>
      {onRemove !== undefined && (
        <div className="button-row">
          <button type="button" disabled={disabled || removeDisabledReason !== null}
            title={removeDisabledReason ?? undefined} onClick={onRemove}>Remove signal</button>
        </div>
      )}
    </fieldset>
  );
}
