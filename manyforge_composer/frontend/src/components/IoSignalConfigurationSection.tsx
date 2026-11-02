import { useEffect, useMemo, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import {
  IO_DRIVER_FRESHNESS_NOTE,
  IO_LIVENESS_UNPROVEN_WARNING,
  IO_OUTPUT_PROFILE_LABELS,
  IO_SOURCE_PROFILE_LABELS,
  inputLivenessMode,
  ioSignalRows,
  ioSignalRowsValue,
  ioSignalsConfiguration,
  newIoSignalInput,
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

/**
 * Declare the boolean input and output signals of one runtime unit. The whole
 * `io_signals` array commits through the ordinary Deployment Draft mutation.
 * New signals are added only to I/O devices (contract 84): ROS-connected I/O,
 * including I/O physically on a robot, is its own I/O device component, so an
 * arm's runtime only lists (and may remove) rows it already declares.
 */
export function IoSignalConfigurationSection({
  controller,
  runtimes,
  geometricContactRobots = [],
}: {
  controller: DeploymentWorkspaceController;
  runtimes: IoSignalRuntimeConfiguration[];
  /** Motion robots used to select a dynamic geometric-contact source. */
  geometricContactRobots?: readonly GeometricContactRobotOption[];
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
  useEffect(() => {
    setRows(source);
    setError(null);
  }, [source]);
  const disabled = !controller.mutationAvailable || controller.busy;
  const declared = location !== null && Array.isArray(location.configuration.io_signals);
  const sameValue = (left: IoSignalRow[], right: IoSignalRow[]) => (
    validateIoSignalRows(left) === null && validateIoSignalRows(right) === null
      ? JSON.stringify(ioSignalRowsValue(left)) === JSON.stringify(ioSignalRowsValue(right))
      : left === right
  );
  const changed = !sameValue(rows, source);
  const updateRow = (index: number, patch: Partial<IoSignalInputDraft>) => {
    setRows(rows.map((row, rowIndex) => (
      rowIndex === index && row.kind === 'input' ? { kind: 'input', draft: { ...row.draft, ...patch } } : row
    )));
  };
  const updateOutput = (index: number, patch: Partial<IoSignalOutputDraft>) => {
    setRows(rows.map((row, rowIndex) => (
      rowIndex === index && row.kind === 'output' ? { kind: 'output', draft: { ...row.draft, ...patch } } : row
    )));
  };
  const setDirection = (index: number, direction: 'input' | 'output') => {
    setRows(rows.map((row, rowIndex) => (
      rowIndex === index && row.kind !== 'preserved' ? withIoDirection(row, direction) : row
    )));
  };
  const commit = async () => {
    if (location === null) return;
    const problem = validateIoSignalRows(rows, runtime?.ioProvider ? runtime.runtimeUnitId : undefined);
    if (problem !== null) {
      setError(problem);
      return;
    }
    if (rows.length === 0 && !declared) return;
    setError(null);
    // A refusal shows once, in the yellow Deployment notice row.
    if (rows.length === 0) await controller.mutate(location.path, 'remove');
    else await controller.mutate(location.path, 'set', ioSignalRowsValue(rows));
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
            {rows.length === 0 && <p className="muted">No I/O signals are declared for {runtime.runtimeUnitId}.</p>}
            {rows.map((row, index) => (
              row.kind === 'preserved' ? (
                <fieldset className="deployment-gripper-editor" key={`preserved-${index}`}>
                  <legend>{String(row.raw.signal_name ?? `Signal ${index + 1}`)} · {String(row.raw.direction ?? 'unknown')}</legend>
                  <p className="field-help">{row.reason}</p>
                  <div className="button-row">
                    <button type="button" disabled={disabled}
                      onClick={() => setRows(rows.filter((_row, rowIndex) => rowIndex !== index))}>Remove signal</button>
                  </div>
                </fieldset>
              ) : row.kind === 'output' ? (
                <IoSignalOutputFields
                  key={`output-${index}`}
                  index={index}
                  draft={row.draft}
                  disabled={disabled}
                  ioProvider={runtime.ioProvider === true}
                  onChange={(patch) => updateOutput(index, patch)}
                  onDirection={(direction) => setDirection(index, direction)}
                  onRemove={() => setRows(rows.filter((_row, rowIndex) => rowIndex !== index))}
                />
              ) : (
                <IoSignalInputFields
                  key={`input-${index}`}
                  index={index}
                  draft={row.draft}
                  disabled={disabled}
                  ioProvider={runtime.ioProvider === true}
                  geometricContactRobots={geometricContactRobots}
                  onChange={(patch) => updateRow(index, patch)}
                  onDirection={(direction) => setDirection(index, direction)}
                  onRemove={() => setRows(rows.filter((_row, rowIndex) => rowIndex !== index))}
                />
              )
            ))}
            {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
            {!canAdd && (
              <p className="field-help">ROS-connected I/O (ROS Bool / SetBool, ros2_control GPIO) is its own I/O device: add it in I/O devices; a machine's own I/O banks are its I/O members. Signals listed here on {runtime.runtimeUnitId} can only be reviewed or removed.</p>
            )}
            <div className="button-row">
              {canAdd && <button type="button" disabled={disabled}
                onClick={() => setRows([...rows, {
                  kind: 'input',
                  draft: newIoSignalInput(runtime.intentComponent ? runtime.runtimeUnitId : ''),
                }])}>Add input signal</button>}
              {canAdd && <button type="button" disabled={disabled}
                onClick={() => setRows([...rows, withIoDirection({
                  kind: 'input',
                  draft: newIoSignalInput(runtime.intentComponent ? runtime.runtimeUnitId : ''),
                }, 'output')])}>Add output signal</button>}
              <button type="button" className="primary-action" disabled={disabled || !changed}
                onClick={() => { void commit(); }}>Update I/O Draft</button>
              <button type="button" disabled={disabled || !changed}
                onClick={() => { setRows(source); setError(null); }}>Discard I/O edits</button>
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

export function IoSignalOutputFields({
  index,
  draft,
  disabled,
  ioProvider = false,
  onChange,
  onDirection,
  onRemove,
}: {
  index: number;
  draft: IoSignalOutputDraft;
  disabled: boolean;
  /** An I/O device row: its component is the device itself (read-only). */
  ioProvider?: boolean;
  onChange: (patch: Partial<IoSignalOutputDraft>) => void;
  onDirection: (direction: 'input' | 'output') => void;
  onRemove?: () => void;
}) {
  const label = draft.signalName.trim() === '' ? `Signal ${index + 1}` : draft.signalName;
  const text = (name: string, field: keyof IoSignalOutputDraft, help?: string) => (
    <label className="deployment-focused-field" key={field}>
      <span>{name}</span>
      <input aria-label={`${label} ${name}`} value={draft[field]} title={help}
        disabled={disabled} readOnly={ioProvider && field === 'componentId'}
        onChange={(event) => onChange({ [field]: event.target.value })} />
    </label>
  );
  return (
    <fieldset className="deployment-gripper-editor">
      <legend>{label} · bool output</legend>
      <div className="deployment-focused-form-grid">
        {text('Signal name', 'signalName')}
        {text('Component', 'componentId')}
        {text('Target', 'targetId')}
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
      {onRemove !== undefined && (
        <div className="button-row">
          <button type="button" disabled={disabled} onClick={onRemove}>Remove signal</button>
        </div>
      )}
    </fieldset>
  );
}

export function IoSignalInputFields({
  index,
  draft,
  disabled,
  ioProvider = false,
  geometricContactRobots = [],
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
  onChange: (patch: Partial<IoSignalInputDraft>) => void;
  onDirection: (direction: 'input' | 'output') => void;
  onRemove?: () => void;
}) {
  const label = draft.signalName.trim() === '' ? `Signal ${index + 1}` : draft.signalName;
  const profiles = Object.keys(IO_SOURCE_PROFILE_LABELS) as IoSourceProfile[];
  const liveness = inputLivenessMode(draft);
  const robotOptions = [...geometricContactRobots]
    .sort((left, right) => left.componentId.localeCompare(right.componentId));
  const text = (name: string, field: keyof IoSignalInputDraft, help?: string) => (
    <label className="deployment-focused-field" key={field}>
      <span>{name}</span>
      <input aria-label={`${label} ${name}`} value={draft[field] as string} title={help}
        disabled={disabled} readOnly={ioProvider && field === 'componentId'}
        onChange={(event) => onChange({ [field]: event.target.value })} />
    </label>
  );
  const number = (name: string, field: keyof IoSignalInputDraft, unit: string, help?: string) => (
    <label className="deployment-focused-field" key={field}>
      <span>{name}</span>
      <span className="deployment-number-with-unit">
        <input aria-label={`${label} ${name}`} inputMode="decimal" value={draft[field] as string}
          disabled={disabled} title={help} onChange={(event) => onChange({ [field]: event.target.value })} />
        <small>{unit}</small>
      </span>
    </label>
  );
  return (
    <fieldset className="deployment-gripper-editor">
      <legend>{label} · bool input</legend>
      <div className="deployment-focused-form-grid">
        {text('Signal name', 'signalName')}
        {text('Component', 'componentId')}
        {text('Target', 'targetId')}
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
              text('Robot component id', 'robotComponentId', 'The motion robot whose Active TCP is followed.')
            )}
            {text('Target Scene resource', 'targetSceneResourceId',
              'Exact logical Scene resource id whose current collision shapes are queried.')}
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
            {draft.echoEnabled && text('Output signal name', 'echoOutputSignalName',
              'Name of a Mock output on this I/O device.')}
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
      {draft.profile === 'geometric_contact' && (
        <p className="field-help" data-geometric-contact-note>
          Derived read-only input. Runtime reads the selected robot&apos;s current Active TCP at each evaluation and
          uses the contact bodies declared for that TCP&apos;s owner with the selected Scene resource; neither is copied
          into the Program. No Active TCP, no contact bodies declared for its owner, an uncarried TCP tool, or
          missing, stale, busy or inconclusive geometry makes the input unavailable, never false. A maximum age
          below runtime production latency can make the input stale. Contact bodies must move with the robot;
          bodies static relative to its motion group produce contact_bodies_static (unavailable, never false).
          Manual and timed Mock writes do not apply.
        </p>
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
          <button type="button" disabled={disabled} onClick={onRemove}>Remove signal</button>
        </div>
      )}
    </fieldset>
  );
}
