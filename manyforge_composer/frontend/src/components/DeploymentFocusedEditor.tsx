import { useEffect, useMemo, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  DeploymentControlledStopProfile,
  JsonValue,
  SourceModelProjection,
} from '../types/api';
import type {
  ManualControlGripperDescriptor,
  ManualControlMotionDescriptor,
} from '../types/robotControl';

interface DeploymentFocusedEditorProps {
  controller: DeploymentWorkspaceController;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  motionDescriptors: ManualControlMotionDescriptor[];
  gripperDescriptors: ManualControlGripperDescriptor[];
}

interface RuntimeConfiguration {
  index: number;
  runtimeUnitId: string;
  configuration: Record<string, JsonValue>;
  configurationPath: Array<string | number>;
  intentTargetSettings: Record<string, JsonValue> | null;
  framePrefix: string | null;
}

interface MotionTargetLimit {
  runtimeIndex: number;
  rowIndex: number;
  componentId: string;
  targetId: string;
  value: number;
  settingsPath: Array<string | number>;
}

interface DirectControlJointLimitDraft {
  componentId: string;
  targetId: string;
  sourceJointName: string;
  maxVelocityRadS: number | null;
  maxAccelerationRadS2: number | null;
  maxJerkRadS3: number | null;
}

interface GripperTargetConfigurationDraft {
  runtimeIndex: number;
  rowIndex: number;
  componentId: string;
  targetId: string;
  description: string;
  actuationKind: 'position_commanded' | 'pneumatic';
  outputMode: 'commanded' | 'none';
  openStateRef: string;
  closedStateRef: string;
  commandDurationS: number;
  deliveryResultTimeoutS?: number;
  touchLinks: string[];
  mountFrame: string;
  attachFrame: string;
  tcpFrame: string;
  auxiliaryFrames: string[];
}

function objectValue(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value
    : null;
}

function numberValue(value: JsonValue | undefined): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function runtimeConfigurations(
  document: Record<string, JsonValue> | null,
): RuntimeConfiguration[] {
  if (document?.api_version === 'manyforge.deployment_intent.v2') {
    const components = document.components;
    if (!Array.isArray(components)) return [];
    return components.flatMap((value, index) => {
      const component = objectValue(value);
      const configuration = objectValue(component?.configuration);
      const targetSettings = objectValue(component?.target_settings);
      const componentId = component?.component_id;
      if (configuration === null || targetSettings === null || typeof componentId !== 'string') {
        return [];
      }
      const limits = Object.entries(targetSettings).flatMap(([targetId, setting]) => {
        const row = objectValue(setting);
        return row === null ? [] : [{
          ...row,
          controllable_component_id: componentId,
          command_target_id: targetId,
        }];
      });
      return [{
        index,
        runtimeUnitId: componentId,
        configuration: { ...configuration, motion_target_limits: limits },
        configurationPath: ['components', index, 'configuration'],
        intentTargetSettings: targetSettings,
        framePrefix: configuration.runtime_kind === 'ros2_control'
          ? (typeof component?.frame_prefix === 'string' ? component.frame_prefix : '')
          : null,
      }];
    });
  }
  const graph = objectValue(document?.graph);
  const runtimeUnits = graph?.runtime_units;
  if (!Array.isArray(runtimeUnits)) return [];
  return runtimeUnits.flatMap((value, index) => {
    const runtime = objectValue(value);
    const configuration = objectValue(runtime?.configuration);
    const runtimeUnitId = runtime?.runtime_unit_id;
    return configuration !== null && typeof runtimeUnitId === 'string'
      ? [{
        index,
        runtimeUnitId,
        configuration,
        configurationPath: ['graph', 'runtime_units', index, 'configuration'],
        intentTargetSettings: null,
        framePrefix: null,
      }]
      : [];
  });
}

function motionTargetLimits(runtimes: RuntimeConfiguration[]): MotionTargetLimit[] {
  return runtimes.flatMap((runtime) => {
    const rows = runtime.configuration.motion_target_limits;
    if (!Array.isArray(rows)) return [];
    return rows.flatMap((value, rowIndex) => {
      const row = objectValue(value);
      const componentId = row?.controllable_component_id;
      const targetId = row?.command_target_id;
      const limit = numberValue(row?.maximum_tcp_linear_speed_mps);
      return typeof componentId === 'string'
        && typeof targetId === 'string'
        && limit !== null
        ? [{
          runtimeIndex: runtime.index,
          rowIndex,
          componentId,
          targetId,
          value: limit,
          settingsPath: runtime.intentTargetSettings === null
            ? [...runtime.configurationPath, 'motion_target_limits', rowIndex]
            : ['components', runtime.index, 'target_settings', targetId],
        }]
        : [];
    });
  });
}

function directControlJointLimits(
  runtime: RuntimeConfiguration | undefined,
): DirectControlJointLimitDraft[] {
  const rows = runtime?.configuration.direct_control_joint_limits;
  if (!Array.isArray(rows)) return [];
  return rows.flatMap((value) => {
    const row = objectValue(value);
    const componentId = row?.controllable_component_id;
    const targetId = row?.command_target_id;
    const sourceJointName = row?.source_joint_name;
    if (
      typeof componentId !== 'string'
      || typeof targetId !== 'string'
      || typeof sourceJointName !== 'string'
    ) return [];
    return [{
      componentId,
      targetId,
      sourceJointName,
      maxVelocityRadS: numberValue(row?.max_velocity_rad_s),
      maxAccelerationRadS2: numberValue(row?.max_acceleration_rad_s2),
      maxJerkRadS3: numberValue(row?.max_jerk_rad_s3),
    }];
  });
}

function gripperTargetConfigurations(
  runtimes: RuntimeConfiguration[],
): GripperTargetConfigurationDraft[] {
  return runtimes.flatMap((runtime) => {
    const rows = runtime.configuration.gripper_target_configurations;
    if (!Array.isArray(rows)) return [];
    return rows.flatMap((value, rowIndex) => {
      const row = objectValue(value);
      const componentId = row?.controllable_component_id;
      const targetId = row?.command_target_id;
      const description = row?.description;
      const actuationKind = row?.actuation_kind;
      const outputMode = row?.output_mode;
      const openStateRef = row?.open_state_ref;
      const closedStateRef = row?.closed_state_ref;
      const commandDurationS = numberValue(row?.command_duration_s);
      const deliveryResultTimeoutS = row?.delivery_result_timeout_s;
      const touchLinks = row?.touch_links;
      const mountFrame = row?.mount_frame;
      const attachFrame = row?.attach_frame;
      const tcpFrame = row?.tcp_frame;
      const auxiliaryFrames = row?.auxiliary_frames;
      if (
        typeof componentId !== 'string'
        || typeof targetId !== 'string'
        || typeof description !== 'string'
        || (actuationKind !== 'position_commanded' && actuationKind !== 'pneumatic')
        || (outputMode !== 'commanded' && outputMode !== 'none')
        || typeof openStateRef !== 'string'
        || typeof closedStateRef !== 'string'
        || commandDurationS === null
        || (deliveryResultTimeoutS !== undefined
          && (actuationKind !== 'pneumatic'
            || typeof deliveryResultTimeoutS !== 'number'
            || !Number.isFinite(deliveryResultTimeoutS)
            || deliveryResultTimeoutS <= 0))
        || !Array.isArray(touchLinks)
        || touchLinks.some((item) => typeof item !== 'string')
        || typeof mountFrame !== 'string'
        || typeof attachFrame !== 'string'
        || typeof tcpFrame !== 'string'
        || !Array.isArray(auxiliaryFrames)
        || auxiliaryFrames.some((item) => typeof item !== 'string')
      ) return [];
      return [{
        runtimeIndex: runtime.index,
        rowIndex,
        componentId,
        targetId,
        description,
        actuationKind,
        outputMode,
        openStateRef,
        closedStateRef,
        commandDurationS,
        ...(deliveryResultTimeoutS === undefined ? {} : { deliveryResultTimeoutS }),
        touchLinks: touchLinks as string[],
        mountFrame,
        attachFrame,
        tcpFrame,
        auxiliaryFrames: auxiliaryFrames as string[],
      }];
    });
  });
}

function gripperConfigurationValue(
  draft: GripperTargetConfigurationDraft,
): JsonValue {
  return {
    controllable_component_id: draft.componentId,
    command_target_id: draft.targetId,
    description: draft.description.trim(),
    actuation_kind: draft.actuationKind,
    output_mode: draft.outputMode,
    open_state_ref: draft.openStateRef,
    closed_state_ref: draft.closedStateRef,
    command_duration_s: draft.commandDurationS,
    ...(draft.actuationKind === 'pneumatic' && draft.deliveryResultTimeoutS !== undefined
      ? { delivery_result_timeout_s: draft.deliveryResultTimeoutS } : {}),
    touch_links: [...draft.touchLinks].sort(),
    mount_frame: draft.mountFrame,
    attach_frame: draft.attachFrame,
    tcp_frame: draft.tcpFrame,
    auxiliary_frames: [...draft.auxiliaryFrames].sort(),
  };
}

interface ControlledStopTarget {
  key: string;
  componentId: string;
  targetId: string;
  label: string;
  profile: DeploymentControlledStopProfile | null;
  derivedRows: number;
}

function controlledStopTargets(document: Record<string, JsonValue> | null): ControlledStopTarget[] {
  if (document?.api_version === 'manyforge.deployment_intent.v2') {
    return (Array.isArray(document.components) ? document.components : []).flatMap((componentValue) => {
      const component = objectValue(componentValue);
      const componentId = component?.component_id;
      const settings = objectValue(component?.target_settings);
      if (typeof componentId !== 'string' || settings === null) return [];
      return Object.entries(settings).flatMap(([targetId, settingValue]) => {
        const stop = objectValue(objectValue(settingValue)?.controlled_stop);
        const mode = stop?.stop_mode;
        const deceleration = numberValue(stop?.deceleration_time_s);
        const natural = stop?.allow_natural_completion_on_short_path;
        const jerkEnabled = stop?.jerk_limits_enabled;
        const limits = stop?.per_joint_limits;
        if ((mode !== 'soft_on_path' && mode !== 'spring_back_hold')
          || deceleration === null || typeof natural !== 'boolean'
          || typeof jerkEnabled !== 'boolean' || !Array.isArray(limits)) return [];
        const normalizedLimits = limits.flatMap((limitValue) => {
          const limit = objectValue(limitValue);
          const sourceJoint = limit?.source_joint_name;
          const maximum = numberValue(limit?.max_deceleration_rad_s2);
          const threshold = numberValue(limit?.settle_velocity_threshold_rad_s);
          const jerk = limit?.max_jerk_rad_s3 === undefined
            ? null : numberValue(limit.max_jerk_rad_s3);
          if (typeof sourceJoint !== 'string' || maximum === null || threshold === null
            || (limit?.max_jerk_rad_s3 !== undefined && jerk === null)) return [];
          return [{
            resource_id: sourceJoint,
            max_deceleration: { coordinate_kind: 'angular_rad_s2', value: maximum },
            settle_velocity_threshold: { coordinate_kind: 'angular_rad_s', value: threshold },
            ...(jerk === null ? {} : {
              max_jerk: { coordinate_kind: 'angular_rad_s3', value: jerk },
            }),
          }];
        });
        if (normalizedLimits.length !== limits.length || normalizedLimits.length === 0) return [];
        const maxStopTime = stop?.max_stop_time_s === undefined
          ? undefined : numberValue(stop.max_stop_time_s);
        if (maxStopTime === null) return [];
        return [{
          key: JSON.stringify([componentId, targetId]),
          componentId,
          targetId,
          label: `${componentId} · ${targetId}`,
          profile: {
            stop_mode: mode,
            deceleration_time_s: deceleration,
            allow_natural_completion_on_short_path: natural,
            jerk_limits_enabled: jerkEnabled,
            per_resource_limits: normalizedLimits,
            ...(maxStopTime === undefined ? {} : { max_stop_time_s: maxStopTime }),
          },
          derivedRows: 1,
        } satisfies ControlledStopTarget];
      });
    });
  }
  const policies = objectValue(document?.policies);
  const rows = policies?.stop_stage_parameters;
  const graph = objectValue(document?.graph);
  if (!Array.isArray(rows) || !Array.isArray(graph?.control_bindings)) return [];
  const byBinding = new Map<string, JsonValue[]>();
  for (const value of rows) {
    const row = objectValue(value);
    if (typeof row?.control_binding_id !== 'string') continue;
    const group = byBinding.get(row.control_binding_id) ?? [];
    group.push(value);
    byBinding.set(row.control_binding_id, group);
  }
  const grouped = new Map<string, {
    componentId: string;
    targetId: string;
    rows: JsonValue[];
  }>();
  for (const value of graph.control_bindings) {
    const binding = objectValue(value);
    const id = binding?.control_binding_id;
    const revision = binding?.control_binding_revision;
    const targetRows = typeof id === 'string' ? byBinding.get(id) : undefined;
    const componentId = binding?.controllable_component_id;
    const targetId = binding?.command_target_id;
    if (!binding || typeof id !== 'string' || typeof revision !== 'string'
      || typeof componentId !== 'string' || typeof targetId !== 'string'
      || !targetRows?.length) continue;
    const key = JSON.stringify([componentId, targetId]);
    const group = grouped.get(key) ?? {
      componentId, targetId, rows: [],
    };
    group.rows.push(...targetRows.filter((rowValue) => {
      const row = objectValue(rowValue);
      return row?.stop_escalation_stage_id === 'motion_controlled_stop'
        && objectValue(row.provider_contract_ref)?.provider_contract_role_id === 'stop_contract';
    }));
    grouped.set(key, group);
  }
  return [...grouped.entries()].flatMap(([key, group]) => {
    const normalized = normalizedStopProfile(group.rows);
    if (group.rows.length === 0) return [];
    return [{
      key,
      componentId: group.componentId,
      targetId: group.targetId,
      label: `${group.componentId} · ${group.targetId}`,
      profile: normalized?.profile ?? null,
      derivedRows: normalized?.derivedRows ?? group.rows.length,
    }];
  });
}

function normalizedStopProfile(
  rows: JsonValue[],
): { profile: DeploymentControlledStopProfile; derivedRows: number } | null {
  const profiles = rows.flatMap((value) => {
    const row = objectValue(value);
    const provider = objectValue(row?.provider_contract_ref);
    if (
      row?.stop_escalation_stage_id !== 'motion_controlled_stop'
      || provider?.provider_contract_role_id !== 'stop_contract'
    ) return [];
    const parameters = objectValue(row.default_contract_parameters);
    const mode = parameters?.stop_mode;
    const deceleration = numberValue(parameters?.deceleration_time_s);
    const natural = parameters?.allow_natural_completion_on_short_path;
    const limits = parameters?.per_resource_limits;
    if (
      (mode !== 'soft_on_path' && mode !== 'spring_back_hold')
      || deceleration === null
      || typeof natural !== 'boolean'
      || !Array.isArray(limits)
    ) return [];
    const normalizedLimits = limits.flatMap((limitValue) => {
      const limit = objectValue(limitValue);
      const maximum = objectValue(limit?.max_deceleration);
      const threshold = objectValue(limit?.settle_velocity_threshold);
      const jerk = objectValue(limit?.max_jerk);
      const jerkEntry = Object.entries(jerk ?? {}).find(
        ([key, candidate]) => key.startsWith('value_') && typeof candidate === 'number',
      );
      const maximumEntry = Object.entries(maximum ?? {}).find(
        ([key, candidate]) => key.startsWith('value_') && typeof candidate === 'number',
      );
      const thresholdEntry = Object.entries(threshold ?? {}).find(
        ([key, candidate]) => key.startsWith('value_') && typeof candidate === 'number',
      );
      return typeof limit?.resource_id === 'string'
        && typeof maximum?.coordinate_kind === 'string'
        && typeof threshold?.coordinate_kind === 'string'
        && maximumEntry !== undefined
        && thresholdEntry !== undefined
        ? [{
            resource_id: limit.resource_id,
            max_deceleration: {
              coordinate_kind: maximum.coordinate_kind,
              value: maximumEntry[1] as number,
            },
            settle_velocity_threshold: {
              coordinate_kind: threshold.coordinate_kind,
              value: thresholdEntry[1] as number,
            },
            ...(typeof jerk?.coordinate_kind === 'string' && jerkEntry !== undefined ? {
              max_jerk: { coordinate_kind: jerk.coordinate_kind, value: jerkEntry[1] as number },
            } : {}),
          }]
        : [];
    });
    if (normalizedLimits.length !== limits.length || normalizedLimits.length === 0) return [];
    const maxStopTime = parameters?.max_stop_time_s === undefined
      ? undefined : numberValue(parameters.max_stop_time_s);
    if (maxStopTime === null) return [];
    return [{
      stop_mode: mode,
      deceleration_time_s: deceleration,
      allow_natural_completion_on_short_path: natural,
      jerk_limits_enabled: typeof parameters?.jerk_limits_enabled === 'boolean'
        ? parameters.jerk_limits_enabled : null,
      per_resource_limits: normalizedLimits,
      ...(maxStopTime === undefined ? {} : { max_stop_time_s: maxStopTime }),
    } satisfies DeploymentControlledStopProfile];
  });
  if (profiles.length === 0 || profiles.length !== rows.length) return null;
  const first = JSON.stringify(profiles[0]);
  if (profiles.some((profile) => JSON.stringify(profile) !== first)) return null;
  return { profile: profiles[0], derivedRows: profiles.length };
}

function format(value: number | null, digits = 3): string {
  if (value === null) return 'not declared';
  return Number(value.toFixed(digits)).toString();
}

function shortIdentity(value: string | null | undefined): string {
  if (!value) return 'not declared';
  return value.length > 38 ? `${value.slice(0, 34)}…` : value;
}

function FramePrefixDraft({ value, disabled, onCommit }: {
  value: string;
  disabled: boolean;
  onCommit: (value: string) => Promise<unknown> | unknown;
}) {
  const [draft, setDraft] = useState(value);
  const [invalid, setInvalid] = useState(false);
  useEffect(() => { setDraft(value); setInvalid(false); }, [value]);
  const formatInvalid = draft !== '' && (draft.startsWith('/') || !draft.endsWith('/')
    || /[^A-Za-z0-9_/]/.test(draft));
  const commit = async () => {
    if (draft === value || formatInvalid) return;
    setInvalid(await onCommit(draft) === null);
  };
  return (
    <label className={`deployment-focused-field${formatInvalid || invalid ? ' invalid' : ''}`}>
      <span>TF frame prefix</span>
      <input value={draft} disabled={disabled} aria-invalid={formatInvalid || invalid}
        onChange={(event) => { setDraft(event.target.value); setInvalid(false); }}
        onBlur={() => { void commit(); }}
        onKeyDown={(event) => {
          if (event.key === 'Enter') event.currentTarget.blur();
          if (event.key === 'Escape') setDraft(value);
        }} />
      <small>Empty keeps unprefixed frames. Applying a change restarts this robot holding.</small>
      {(formatInvalid || invalid) && <small className="deployment-field-error inline-message severity-error">
        {formatInvalid
          ? "Frame prefix must be empty or use only ASCII letters, digits, '_' and '/', without a leading '/' and with a trailing '/'."
          : 'Value was rejected.'}
      </small>}
    </label>
  );
}

function IsaacWorldDraft({ controller }: { controller: DeploymentWorkspaceController }) {
  const document = controller.workspace?.draft.document;
  const value = typeof document?.isaac_world === 'string' ? document.isaac_world : '';
  const [draft, setDraft] = useState(value);
  useEffect(() => { setDraft(value); }, [value]);
  const invalid = draft !== '' && !/^[a-z][a-z0-9._-]{0,127}$/.test(draft);
  if (document?.api_version !== 'manyforge.deployment_intent.v2') return null;
  return (
    <label className="deployment-focused-field">
      <span>Isaac world</span>
      <input value={draft} disabled={!controller.mutationAvailable || controller.busy}
        aria-invalid={invalid}
        onChange={(event) => setDraft(event.target.value)}
        onBlur={() => {
          if (!invalid && draft !== value) {
            void controller.mutate(['isaac_world'], draft === '' ? 'remove' : 'set', draft);
          }
        }} />
      <small>Logical world name from the Manager site settings. Required for Isaac robots.</small>
      {invalid && <small className="deployment-field-error inline-message severity-error">
        World must be a portable identifier.
      </small>}
    </label>
  );
}

function NumberDraft({
  label,
  title,
  value,
  unit,
  min,
  max,
  step,
  disabled,
  onCommit,
}: {
  label: string;
  title?: string;
  value: number;
  unit?: string;
  min?: number;
  max?: number;
  step?: number;
  disabled: boolean;
  onCommit: (value: number) => Promise<unknown> | unknown;
}) {
  const [draft, setDraft] = useState(String(value));
  const [invalid, setInvalid] = useState(false);
  useEffect(() => {
    setDraft(String(value));
    setInvalid(false);
  }, [value]);
  const commit = async () => {
    const parsed = Number(draft);
    if (
      !Number.isFinite(parsed)
      || (min !== undefined && parsed < min)
      || (max !== undefined && parsed > max)
    ) {
      setInvalid(true);
      return;
    }
    if (parsed === value) return;
    const result = await onCommit(parsed);
    setInvalid(result === null);
  };
  return (
    <label className={`deployment-focused-field${invalid ? ' invalid' : ''}`} title={title}>
      <span>{label}</span>
      <span className="deployment-number-with-unit">
        <input
          type="number"
          value={draft}
          min={min}
          max={max}
          step={step ?? 'any'}
          disabled={disabled}
          onChange={(event) => {
            setDraft(event.target.value);
            setInvalid(false);
          }}
          onBlur={() => { void commit(); }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              setDraft(String(value));
              setInvalid(false);
              event.currentTarget.blur();
            }
          }}
        />
        {unit && <small>{unit}</small>}
      </span>
      {invalid && <small className="deployment-field-error inline-message severity-error">Value was rejected.</small>}
    </label>
  );
}

function OptionalNumberDraft({
  label,
  value,
  unit,
  disabled,
  onCommit,
  maximum,
  minimum,
  placeholder = 'model only',
}: {
  label: string;
  value: number | null;
  unit: string;
  disabled: boolean;
  onCommit: (value: number | null) => Promise<unknown> | unknown;
  maximum?: number;
  minimum?: number;
  placeholder?: string;
}) {
  const [draft, setDraft] = useState(value === null ? '' : String(value));
  const [invalid, setInvalid] = useState(false);
  useEffect(() => {
    setDraft(value === null ? '' : String(value));
    setInvalid(false);
  }, [value]);
  const commit = async () => {
    const parsed = draft.trim() === '' ? null : Number(draft);
    if (parsed !== null && (!Number.isFinite(parsed) || parsed <= 0
      || (maximum !== undefined && parsed > maximum)
      || (minimum !== undefined && parsed < minimum))) {
      setInvalid(true);
      return;
    }
    if (parsed === value) return;
    const result = await onCommit(parsed);
    setInvalid(result === null);
  };
  return (
    <label className={`deployment-focused-field${invalid ? ' invalid' : ''}`}>
      <span>{label}</span>
      <span className="deployment-number-with-unit">
        <input
          type="number"
          value={draft}
          min={minimum ?? 0.000001}
          max={maximum}
          step="any"
          placeholder={placeholder}
          disabled={disabled}
          onChange={(event) => {
            setDraft(event.target.value);
            setInvalid(false);
          }}
          onBlur={() => { void commit(); }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              setDraft(value === null ? '' : String(value));
              setInvalid(false);
              event.currentTarget.blur();
            }
          }}
        />
        <small>{unit}</small>
      </span>
      {invalid && <small className="deployment-field-error inline-message severity-error">
        {maximum === 0 ? 'Enable ordinary jerk shaping first, or leave blank.'
          : minimum !== undefined ? `Use at least ${minimum}${maximum === undefined ? '' : ` and at most ${maximum}`}, or clear the manual jerk cap first.`
          : maximum === undefined ? 'Enter a positive value, or leave blank.'
            : `Enter a positive value at most ${maximum}, or leave blank.`}
      </small>}
    </label>
  );
}

function updatedDirectControlJointLimits(
  current: DirectControlJointLimitDraft[],
  identity: Pick<DirectControlJointLimitDraft, 'componentId' | 'targetId' | 'sourceJointName'>,
  field: 'maxVelocityRadS' | 'maxAccelerationRadS2' | 'maxJerkRadS3',
  value: number | null,
): JsonValue[] {
  const existing = current.find((row) => (
    row.componentId === identity.componentId
    && row.targetId === identity.targetId
    && row.sourceJointName === identity.sourceJointName
  ));
  const next = {
    ...identity,
    maxVelocityRadS: existing?.maxVelocityRadS ?? null,
    maxAccelerationRadS2: existing?.maxAccelerationRadS2 ?? null,
    maxJerkRadS3: existing?.maxJerkRadS3 ?? null,
    [field]: value,
  };
  return current
    .filter((row) => !(
      row.componentId === identity.componentId
      && row.targetId === identity.targetId
      && row.sourceJointName === identity.sourceJointName
    ))
    .concat(
      next.maxVelocityRadS === null
      && next.maxAccelerationRadS2 === null
      && next.maxJerkRadS3 === null
        ? []
        : [next],
    )
    .sort((left, right) => (
      `${left.componentId}\u0000${left.targetId}\u0000${left.sourceJointName}`
        .localeCompare(`${right.componentId}\u0000${right.targetId}\u0000${right.sourceJointName}`)
    ))
    .map((row) => ({
      controllable_component_id: row.componentId,
      command_target_id: row.targetId,
      source_joint_name: row.sourceJointName,
      ...(row.maxVelocityRadS === null ? {} : { max_velocity_rad_s: row.maxVelocityRadS }),
      ...(row.maxAccelerationRadS2 === null
        ? {}
        : { max_acceleration_rad_s2: row.maxAccelerationRadS2 }),
      ...(row.maxJerkRadS3 === null ? {} : { max_jerk_rad_s3: row.maxJerkRadS3 }),
    }));
}

function DirectControlJointLimitsEditor({
  controller,
  runtime,
  descriptor,
}: {
  controller: DeploymentWorkspaceController;
  runtime: RuntimeConfiguration;
  descriptor: ManualControlMotionDescriptor;
}) {
  const authored = directControlJointLimits(runtime);
  const targets = runtime.configuration.motion_target_limits;
  const target = Array.isArray(targets) ? targets.map(objectValue).find((row) => (
    row?.controllable_component_id === descriptor.controllableComponentId
    && row?.command_target_id === descriptor.commandTargetId
  )) : null;
  const ordinary = objectValue(target?.ordinary_motion_jerk);
  const ordinaryRows = Array.isArray(ordinary?.joint_limits) ? ordinary.joint_limits : [];
  const disabled = !controller.mutationAvailable || controller.busy;
  const commit = (
    sourceJointName: string,
    field: 'maxVelocityRadS' | 'maxAccelerationRadS2' | 'maxJerkRadS3',
    value: number | null,
  ) => controller.mutate(
    [
      ...runtime.configurationPath,
      'direct_control_joint_limits',
    ],
    'set',
    updatedDirectControlJointLimits(authored, {
      componentId: descriptor.controllableComponentId,
      targetId: descriptor.commandTargetId,
      sourceJointName,
    }, field, value),
  );
  return (
    <details className="deployment-joint-caps">
      <summary>Robot Control joint caps</summary>
      <div className="deployment-joint-cap-list">
        {descriptor.operationalJointLimits.map((limit) => {
          const draft = authored.find((row) => (
            row.componentId === descriptor.controllableComponentId
            && row.targetId === descriptor.commandTargetId
            && row.sourceJointName === limit.sourceJointName
          ));
          const ordinaryJoint = ordinaryRows.map(objectValue).find((row) => (
            row?.source_joint_name === limit.sourceJointName
          ));
          const imported = objectValue(ordinaryJoint?.imported);
          // Only explicit Draft caps are known here. Presets and final
          // consistency remain the native activation owner's responsibility.
          const jerkMaximum = ordinary?.enabled !== true ? 0
            : numberValue(ordinaryJoint?.override_rad_s3)
              ?? (imported?.enabled === true ? numberValue(imported.maximum_rad_s3) : null)
              ?? undefined;
          return (
            <section className="deployment-joint-cap" key={limit.sourceJointName}>
              <header>
                <strong>{limit.sourceJointName}</strong>
                <small>
                  Model: {format(limit.modelMaxVelocityRadS)} rad/s · {' '}
                  {format(limit.modelMaxAccelerationRadS2)} rad/s²
                </small>
              </header>
              <div className="deployment-focused-form-grid">
                <OptionalNumberDraft
                  label="Maximum velocity"
                  value={draft?.maxVelocityRadS ?? null}
                  maximum={limit.modelMaxVelocityRadS}
                  unit="rad/s"
                  disabled={disabled}
                  onCommit={(value) => commit(limit.sourceJointName, 'maxVelocityRadS', value)}
                />
                <OptionalNumberDraft
                  label="Maximum acceleration"
                  value={draft?.maxAccelerationRadS2 ?? null}
                  maximum={limit.modelMaxAccelerationRadS2 ?? undefined}
                  unit="rad/s²"
                  disabled={disabled}
                  onCommit={(value) => commit(limit.sourceJointName, 'maxAccelerationRadS2', value)}
                />
                <OptionalNumberDraft
                  label="Maximum jerk"
                  value={draft?.maxJerkRadS3 ?? null}
                  maximum={jerkMaximum}
                  placeholder={ordinary?.enabled === true ? 'ordinary profile' : 'enable ordinary jerk first'}
                  unit="rad/s³"
                  disabled={disabled}
                  onCommit={(value) => commit(limit.sourceJointName, 'maxJerkRadS3', value)}
                />
              </div>
              <small className="deployment-joint-effective">
                Currently applied effective limit: {format(limit.effectiveMaxVelocityRadS)} rad/s · {' '}
                {format(limit.effectiveMaxAccelerationRadS2)} rad/s² · {' '}
                {format(limit.effectiveMaxJerkRadS3)} rad/s³
              </small>
            </section>
          );
        })}
      </div>
      <p className="field-help">
        Blank velocity and acceleration fields use the robot model. Manual jerk caps require ordinary jerk shaping and cannot exceed its limits. Clear manual jerk caps before switching ordinary shaping off. Draft changes become effective only after Deployment Apply.
      </p>
    </details>
  );
}

function OrdinaryMotionJerkEditor({ controller, runtime, limit, descriptor }: {
  controller: DeploymentWorkspaceController;
  runtime: RuntimeConfiguration;
  limit: MotionTargetLimit;
  descriptor: ManualControlMotionDescriptor | null;
}) {
  const rows = runtime.configuration.motion_target_limits;
  const target = Array.isArray(rows) ? objectValue(rows[limit.rowIndex]) : null;
  const settings = objectValue(target?.ordinary_motion_jerk) ?? {
    enabled: false, preset_reference_time_s: 1, joint_limits: [],
  };
  const applied = descriptor?.ordinaryJerk ?? null;
  const disabled = !controller.mutationAvailable || controller.busy || applied === null;
  const enabled = settings.enabled === true;
  const joints = Array.isArray(settings.joint_limits) ? settings.joint_limits : [];
  const required = joints.some((row) => objectValue(objectValue(row)?.imported)?.machine_required === true);
  const manualCaps = directControlJointLimits(runtime).filter((row) => (
    row.componentId === limit.componentId && row.targetId === limit.targetId
    && row.maxJerkRadS3 !== null
  ));
  const commit = (changes: Record<string, JsonValue>) => controller.mutate(
    [...limit.settingsPath, 'ordinary_motion_jerk'],
    'set', { ...settings, ...changes },
  );
  const override = (sourceJointName: string, value: number | null) => {
    const existing = joints.map(objectValue).find((row) => row?.source_joint_name === sourceJointName);
    const next = { ...(existing ?? { source_joint_name: sourceJointName }) };
    if (value === null) delete next.override_rad_s3;
    else next.override_rad_s3 = value;
    return commit({ joint_limits: [
      ...joints.filter((row) => objectValue(row)?.source_joint_name !== sourceJointName),
      ...(next.imported !== undefined || value !== null ? [next] : []),
    ] });
  };
  return (
    <section className="deployment-joint-caps">
      <div className="deployment-focused-form-grid">
        <label className="deployment-focused-field"
          title={applied === null ? 'No qualified ordinary jerk capability is reported for this target.'
            : 'Applies to Program and manual trajectories. Controlled Stop has its own setting. Save and Apply to activate.'}>
          <span>Ordinary motion jerk</span>
          <select aria-label={`${limit.componentId} / ${limit.targetId} ordinary motion jerk`}
            disabled={disabled} value={enabled ? 'on' : 'off'}
            onChange={(event) => { void commit({ enabled: event.target.value === 'on' }); }}>
            <option value="off" disabled={required || manualCaps.length > 0}>Off</option>
            <option value="on">Enforce limits</option>
          </select>
        </label>
        <NumberDraft label="Preset reference time" unit="s" min={0.000001} max={1000}
          value={numberValue(settings.preset_reference_time_s) ?? 1} disabled={disabled || !enabled}
          onCommit={(value) => commit({ preset_reference_time_s: value })} />
      </div>
      {manualCaps.length > 0 && <p className="field-help">
        Clear this target's Robot Control jerk caps before switching ordinary shaping off.
      </p>}
      {applied !== null && <details>
        <summary>Ordinary joint jerk limits</summary>
        {applied.jointLimits.map((joint) => {
          const draft = joints.map(objectValue).find((row) => row?.source_joint_name === joint.sourceJointName);
          const imported = objectValue(draft?.imported);
          const ceiling = imported?.enabled === true ? numberValue(imported.maximum_rad_s3) ?? undefined : undefined;
          return <section className="deployment-joint-cap" key={joint.sourceJointName}>
            <OptionalNumberDraft label={joint.sourceJointName} unit="rad/s³"
              value={numberValue(draft?.override_rad_s3)} disabled={disabled || !enabled}
              minimum={manualCaps.find((row) => row.sourceJointName === joint.sourceJointName)?.maxJerkRadS3 ?? undefined}
              maximum={ceiling} placeholder={ceiling === undefined ? 'ManyForge preset' : `Imported: ${ceiling}`}
              onCommit={(value) => override(joint.sourceJointName, value)} />
            <small title={joint.imported ? `${joint.imported.sourceId}\n${joint.imported.sourceDigest}`
              : 'ManyForge preset uses the model acceleration and saved reference time; it is not manufacturer data.'}>
              Applied: {joint.maximumRadS3 === null ? 'off' : `${format(joint.maximumRadS3)} rad/s³ · ${joint.source}`}
              {' · '}Preset: {format(joint.presetRadS3)} rad/s³
              {joint.imported?.machineRequired && ' · machine-required'}
            </small>
          </section>;
        })}
      </details>}
    </section>
  );
}

function ModelAndTargetSection({
  controller,
  sourceModel,
  motionDescriptors,
  limits,
  runtimes,
}: Pick<DeploymentFocusedEditorProps, 'controller' | 'sourceModel' | 'motionDescriptors'> & {
  limits: MotionTargetLimit[];
  runtimes: RuntimeConfiguration[];
}) {
  const source = sourceModel.status === 'ready' ? sourceModel.data : null;
  const [selectedTarget, setSelectedTarget] = useState('');
  const targetKey = (limit: MotionTargetLimit) => JSON.stringify([limit.componentId, limit.targetId]);
  const target = limits.find((limit) => targetKey(limit) === selectedTarget)
    ?? limits.find((limit) => motionDescriptors.some((descriptor) => (
      limit.componentId === descriptor.controllableComponentId
      && limit.targetId === descriptor.commandTargetId
    )))
    ?? limits[0];
  const selectedDescriptor = motionDescriptors.find((descriptor) => (
    target?.componentId === descriptor.controllableComponentId
    && target?.targetId === descriptor.commandTargetId
  )) ?? null;
  const modelTarget = source?.targets.find((item) => item.component_id === target?.componentId
    && item.target_id === target?.targetId);
  const occurrences = source?.occurrences.filter((item) => modelTarget?.model_groups.some(
    (group) => group.model_instance_id === item.model_instance_id,
  )) ?? [];
  const editingDisabled = !controller.mutationAvailable || controller.busy;
  return (
    <details className="dcard" open>
      <summary>Robot model and target limits</summary>
      <div className="deployment-focused-body">
        {limits.length > 1 && target && <label className="deployment-focused-field">
          <span>Target</span>
          <select aria-label="Motion limits target" value={targetKey(target)}
            disabled={controller.busy} onChange={(event) => setSelectedTarget(event.target.value)}>
            {limits.map((limit) => <option key={targetKey(limit)} value={targetKey(limit)}>
              {limit.componentId} / {limit.targetId}
            </option>)}
          </select>
        </label>}
        <div className="deployment-resolved-grid">
          <span><small>Robot source</small><strong>{occurrences.map((item) => item.source_robot_name).join(', ') || 'Unavailable'}</strong></span>
          <span><small>Model</small><strong>{occurrences.map((item) => item.source_model_name).join(', ') || 'Unavailable'}</strong></span>
          <span title={occurrences.map((item) => item.model_revision).join('\n')}><small>Model revision</small><strong>{occurrences.map((item) => shortIdentity(item.model_revision)).join(', ') || 'not declared'}</strong></span>
          <span><small>Planning groups</small><strong>{modelTarget?.model_groups.map((group) => group.source_group_name).join(', ') || 'not declared'}</strong></span>
        </div>
        {sourceModel.status !== 'ready' && (
          <p className="muted">
            {sourceModel.status === 'loading' ? 'Reading source-model facts.' : sourceModel.message}
          </p>
        )}
        {selectedDescriptor !== null && (
          <div className="deployment-limit-table" role="table" aria-label="Source model joint limits">
            <div className="deployment-limit-row heading" role="row">
              <span>Joint</span><span>Position</span><span>Velocity</span><span>Acceleration</span>
            </div>
            {selectedDescriptor.joints.map((joint) => (
              <div className="deployment-limit-row" role="row" key={joint.sourceJointName}>
                <strong>{joint.sourceJointName}</strong>
                <span>{format(joint.minPositionRad)} to {format(joint.maxPositionRad)} rad</span>
                <span>{format(joint.maxVelocityRadS)} rad/s</span>
                <span>{format(joint.maxAccelerationRadS2)} rad/s²</span>
              </div>
            ))}
          </div>
        )}
        <div className="deployment-target-limits">
          {limits.length === 0 && <p className="muted">No Deployment motion-target maximum is declared.</p>}
          {(target ? [target] : []).map((limit) => {
            const matching = selectedDescriptor?.operationalLimit ?? null;
            const runtime = runtimes.find((item) => item.index === limit.runtimeIndex);
            return (
              <div className="deployment-target-limit" key={`${limit.componentId}:${limit.targetId}`}>
                <header>
                  <strong>{limit.componentId} / {limit.targetId}</strong>
                  <small>Compiler-validated Deployment input</small>
                </header>
                {selectedDescriptor === null && <p className="muted">
                  No current motion descriptor for this target. Jerk activation and joint caps are unavailable.
                </p>}
                <NumberDraft
                  label="Maximum TCP linear speed"
                  value={limit.value}
                  unit="m/s"
                  min={0.000001}
                  max={1000}
                  disabled={editingDisabled}
                  onCommit={(value) => controller.updateMotionTargetLimit(
                    limit.componentId,
                    limit.targetId,
                    value,
                  )}
                />
                <p className="field-help">
                  Maximum TCP speed enforced for this target after Apply.
                  Individual motion nodes and manual commands may request a
                  slower speed.
                </p>
                {matching !== null
                  && matching.maximumTcpLinearSpeedMps !== limit.value && (
                  <p className="deployment-applied-comparison">
                    Currently applied: {format(matching.maximumTcpLinearSpeedMps)} m/s
                  </p>
                )}
                {runtime !== undefined && <OrdinaryMotionJerkEditor
                  controller={controller} runtime={runtime} limit={limit}
                  descriptor={selectedDescriptor} />}
                {matching !== null && selectedDescriptor !== null && (
                  runtime !== undefined && (
                    <DirectControlJointLimitsEditor
                      controller={controller}
                      runtime={runtime}
                      descriptor={selectedDescriptor}
                    />
                  )
                )}
              </div>
            );
          })}
        </div>
        <p className="field-help">
          Physical joint limits remain source-model facts. The Deployment value
          above is the configured TCP-speed maximum; individual motions may
          request a lower speed, and execution validates the measured result.
        </p>
      </div>
    </details>
  );
}

function NativeRuntimeSection({
  controller,
  runtimes,
}: {
  controller: DeploymentWorkspaceController;
  runtimes: RuntimeConfiguration[];
}) {
  const disabled = !controller.mutationAvailable || controller.busy;
  return (
    <details className="dcard">
      <summary>Native runtime configuration</summary>
      <div className="deployment-focused-body">
        {runtimes.length === 0 && <p className="muted">No native runtime configuration is declared.</p>}
        {runtimes.map((runtime) => {
          const prefix = runtime.configurationPath;
          const positions = runtime.configuration.initial_joint_positions_rad;
          return (
            <section className="deployment-runtime-unit" key={runtime.runtimeUnitId}>
              <header><strong>{runtime.runtimeUnitId}</strong><small>Named authored inputs</small></header>
              <div className="deployment-focused-form-grid">
                {runtime.framePrefix !== null && (
                  <FramePrefixDraft value={runtime.framePrefix} disabled={disabled}
                    onCommit={(value) => controller.mutate(['components', runtime.index, 'frame_prefix'], 'set', value)} />
                )}
                {numberValue(runtime.configuration.execution_tick_hz) !== null && (
                  <NumberDraft
                    label="Execution cadence"
                    value={runtime.configuration.execution_tick_hz as number}
                    unit="Hz"
                    min={1}
                    max={10000}
                    disabled={disabled}
                    onCommit={(value) => controller.mutate([...prefix, 'execution_tick_hz'], 'set', value)}
                  />
                )}
                {numberValue(runtime.configuration.collision_monitor_period_ms) !== null && (
                  <NumberDraft
                    label="Collision monitor period"
                    value={runtime.configuration.collision_monitor_period_ms as number}
                    unit="ms"
                    min={1}
                    max={60000}
                    disabled={disabled}
                    onCommit={(value) => controller.mutate([...prefix, 'collision_monitor_period_ms'], 'set', value)}
                  />
                )}
                {numberValue(runtime.configuration.planner_rng_seed) !== null && (
                  <NumberDraft
                    label="Planner seed"
                    value={runtime.configuration.planner_rng_seed as number}
                    min={0}
                    disabled={disabled}
                    onCommit={(value) => controller.mutate([...prefix, 'planner_rng_seed'], 'set', value)}
                  />
                )}
                <label className="deployment-focused-field">
                  <span>Adapter mode</span>
                  <input value={String(runtime.configuration.mode ?? 'not declared')} readOnly />
                </label>
              </div>
              {Array.isArray(positions) && (
                <fieldset className="deployment-initial-joints">
                  <legend>Initial joint state</legend>
                  {positions.map((position, index) => typeof position === 'number' && (
                    <NumberDraft
                      key={index}
                      label={`Joint ${index + 1}`}
                      value={position}
                      unit="rad"
                      disabled={disabled}
                      onCommit={(value) => controller.mutate(
                        [...prefix, 'initial_joint_positions_rad', index],
                        'set',
                        value,
                      )}
                    />
                  ))}
                </fieldset>
              )}
            </section>
          );
        })}
        <p className="field-help">Changes remain in the Deployment Draft until deliberately saved and applied while execution is Idle.</p>
      </div>
    </details>
  );
}

function ControlledStopSection({
  controller,
  targets,
}: {
  controller: DeploymentWorkspaceController;
  targets: ControlledStopTarget[];
}) {
  const [selectedId, setSelectedId] = useState('');
  const target = targets.find((row) => row.key === selectedId) ?? targets[0];
  return (
    <details className="dcard">
      <summary>Stop settings</summary>
      <div className="deployment-focused-body">
        {target ? <>
          <label className="deployment-focused-field">
            <span>Target</span>
            <select aria-label="Controlled Stop target" value={target.key}
              disabled={controller.busy} onChange={(event) => setSelectedId(event.target.value)}>
              {targets.map((row) => <option key={row.key} value={row.key}>{row.label}</option>)}
            </select>
          </label>
          <ControlledStopEditor key={target.key}
            controller={controller} target={target} />
        </> : <p className="muted">No editable Stop contract is declared.</p>}
      </div>
    </details>
  );
}

function ControlledStopEditor({ controller, target }: {
  controller: DeploymentWorkspaceController;
  target: ControlledStopTarget;
}) {
  const [draft, setDraft] = useState<DeploymentControlledStopProfile | null>(
    target.profile,
  );
  useEffect(() => {
    setDraft(target.profile);
  }, [target.profile]);
  const disabled = !controller.mutationAvailable || controller.busy
    || target.profile?.jerk_limits_enabled == null;
  const changed = target.profile !== null && JSON.stringify(draft) !== JSON.stringify(target.profile);
  const requiresMigration = target.profile?.jerk_limits_enabled == null;
  const updateLimit = (
    index: number,
    field: 'max_deceleration' | 'settle_velocity_threshold',
    value: number,
  ) => setDraft((current) => {
    if (current === null) return current;
    const next = structuredClone(current);
    next.per_resource_limits[index][field].value = value;
    return next;
  });
  return (
    <>
        <div className="deployment-focused-form-grid">
          {draft && <>
          <label className="deployment-focused-field">
            <span>Stop mode</span>
            <select
              value={draft.stop_mode}
              disabled={disabled}
              onChange={(event) => setDraft({
                ...draft,
                stop_mode: event.target.value as DeploymentControlledStopProfile['stop_mode'],
                allow_natural_completion_on_short_path: event.target.value === 'soft_on_path',
              })}
            >
              <option value="soft_on_path">Soft on path</option>
              <option value="spring_back_hold">Spring-back hold</option>
            </select>
          </label>
          <NumberDraft
            label={draft.stop_mode === 'spring_back_hold' ? 'Brake + return time' : 'Deceleration time'}
            value={draft.deceleration_time_s}
            unit="s"
            min={0.001}
            max={2}
            disabled={disabled}
            onCommit={(value) => {
              // As `main`: an explicit maximum Stop time never stays below the
              // deceleration time.
              setDraft({
                ...draft,
                deceleration_time_s: value,
                ...(draft.max_stop_time_s !== undefined && draft.max_stop_time_s < value
                  ? { max_stop_time_s: value } : {}),
              });
              return true;
            }}
          />
          <label className="deployment-focused-checkbox" title="Unselected: this robot's controlled Stop must finish within its deceleration time plus 0.5 s. Selecting sets an explicit maximum Stop time for this profile (never below the deceleration time); Save and Apply are still required.">
            <input type="checkbox" checked={draft.max_stop_time_s !== undefined} disabled={disabled}
              onChange={(event) => {
                const next = { ...draft };
                if (event.target.checked) next.max_stop_time_s = draft.deceleration_time_s + 0.5;
                else delete next.max_stop_time_s;
                setDraft(next);
              }} />
            Explicit maximum Stop time
          </label>
          {draft.max_stop_time_s === undefined
            ? <p className="muted">{`Automatic: ${Number((draft.deceleration_time_s + 0.5).toFixed(3))} s (deceleration time + 0.5 s).`}</p>
            : <NumberDraft label="Max Stop time" value={draft.max_stop_time_s}
              title="This robot's controlled Stop must finish within this time from the Stop request; on overrun its hard stop follows, bounded by the lane's stop-completion timeout."
              unit="s" min={0.001} max={9_007_199} disabled={disabled}
              onCommit={(value) => {
                setDraft({ ...draft, max_stop_time_s: Math.max(draft.deceleration_time_s, value) });
                return true;
              }} />}
          </>}
          {draft && <>
          <label className="deployment-focused-checkbox" title={draft.stop_mode === 'spring_back_hold'
            ? 'Off by default: brake and return to the stop-trigger position. Enabling permits a short remaining move to finish at its endpoint instead; that is not a precision return.'
            : 'On by default: a sufficiently short remaining move may finish at its endpoint, with rest confirmed within the Stop deadline.'}>
            <input
              type="checkbox"
              checked={draft.allow_natural_completion_on_short_path}
              disabled={disabled}
              onChange={(event) => setDraft({
                ...draft,
                allow_natural_completion_on_short_path: event.target.checked,
              })}
            />
            Allow natural completion on a short remaining path
          </label>
          <label className="deployment-focused-field" title="Constrains the controlled attempt only, not ordinary moves or hard containment. Missing per-joint values use the explicit ManyForge preset at preparation.">
            <span>Controlled Stop jerk</span>
            <select
              aria-label="Controlled Stop jerk limits"
              value={draft.jerk_limits_enabled === null ? '' : draft.jerk_limits_enabled ? 'on' : 'off'}
              disabled={disabled || requiresMigration}
              onChange={(event) => setDraft({ ...draft, jerk_limits_enabled: event.target.value === 'on' })}
            >
              <option value="" disabled>Review required — choose</option>
              <option value="off">Off</option>
              <option value="on">Enforce jerk limits</option>
            </select>
          </label>
          </>}
        </div>
        {requiresMigration && <p className="field-help">This older Stop contract needs explicit migration before editing. Runtime Stop remains unchanged.</p>}
        {draft && <div className="deployment-stop-limits">
          <div className="deployment-stop-limit heading">
            <span>Resource</span><span>Maximum deceleration</span><span>Settled below</span>
          </div>
          {draft.per_resource_limits.map((limit, index) => (
            <div className="deployment-stop-limit" key={limit.resource_id}>
              <strong title={limit.resource_id}>{limit.resource_id.split('.').at(-1)}</strong>
              <input
                type="number"
                min="0.000001"
                step="any"
                value={limit.max_deceleration.value}
                disabled={disabled}
                aria-label={`${limit.resource_id} maximum deceleration`}
                onChange={(event) => updateLimit(index, 'max_deceleration', Number(event.target.value))}
              />
              <input
                type="number"
                min="0"
                step="any"
                value={limit.settle_velocity_threshold.value}
                disabled={disabled}
                aria-label={`${limit.resource_id} settle velocity threshold`}
                onChange={(event) => updateLimit(index, 'settle_velocity_threshold', Number(event.target.value))}
              />
            </div>
          ))}
        </div>}
        {draft?.jerk_limits_enabled === true && (
          <details>
            <summary>Joint jerk limits</summary>
            {draft.per_resource_limits.map((limit, index) => (
              <label className="deployment-focused-field" key={limit.resource_id}>
                <span>{limit.resource_id.split('.').at(-1)} ({limit.max_deceleration.coordinate_kind === 'angular_rad_s2' ? 'rad/s³' : 'm/s³'})</span>
                <input
                  type="number" min="0.000001" step="any" disabled={disabled}
                  aria-label={`${limit.resource_id} maximum jerk`}
                  placeholder="ManyForge preset" value={limit.max_jerk?.value ?? ''}
                  title="Empty uses the ManyForge acceleration/ramp preset. Explicit values are saved with this Deployment."
                  onChange={(event) => {
                    const next = structuredClone(draft);
                    if (event.target.value === '') delete next.per_resource_limits[index].max_jerk;
                    else next.per_resource_limits[index].max_jerk = {
                      coordinate_kind: limit.max_deceleration.coordinate_kind === 'angular_rad_s2' ? 'angular_rad_s3' : 'linear_m_s3',
                      value: Number(event.target.value),
                    };
                    setDraft(next);
                  }}
                />
              </label>
            ))}
          </details>
        )}
        <div className="button-row">
          <button
            type="button"
            className="primary-action"
            disabled={disabled || !changed || requiresMigration}
            onClick={() => { void controller.updateControlledStop({
              controllable_component_id: target.componentId,
              command_target_id: target.targetId,
              profile: draft,
            }); }}
          >Update Controlled Stop Draft</button>
          <button
            type="button"
            disabled={disabled || !changed}
            onClick={() => { setDraft(target.profile); }}
          >Discard Stop edits</button>
        </div>
        <p className="field-help" title={`Preparation expands this target profile to ${target.derivedRows} current route row(s). Each robot's controlled Stop is bounded by its own maximum Stop time; on overrun or nominal failure its hard stop follows.`}>
          Save records these choices; Apply activates them.
        </p>
    </>
  );
}

function GripperSection({
  controller,
  descriptors,
  runtimes,
}: {
  controller: DeploymentWorkspaceController;
  descriptors: ManualControlGripperDescriptor[];
  runtimes: RuntimeConfiguration[];
}) {
  const authored = useMemo(() => gripperTargetConfigurations(runtimes), [runtimes]);
  const [selectedTarget, setSelectedTarget] = useState('');
  const descriptorKey = (descriptor: ManualControlGripperDescriptor) => JSON.stringify([
    descriptor.controllableComponentId, descriptor.commandTargetId,
  ]);
  const descriptor = descriptors.find((candidate) => descriptorKey(candidate) === selectedTarget)
    ?? descriptors.find((candidate) => authored.some((row) => (
      row.componentId === candidate.controllableComponentId
      && row.targetId === candidate.commandTargetId
    )))
    ?? descriptors[0]
    ?? null;
  const source = descriptor === null ? null : authored.find((row) => (
    row.componentId === descriptor.controllableComponentId
    && row.targetId === descriptor.commandTargetId
  )) ?? null;
  const [draft, setDraft] = useState<GripperTargetConfigurationDraft | null>(source);
  const [touchLinksDraft, setTouchLinksDraft] = useState(source?.touchLinks.join(', ') ?? '');
  const [auxiliaryFramesDraft, setAuxiliaryFramesDraft] = useState(
    source?.auxiliaryFrames.join(', ') ?? '',
  );
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    setDraft(source);
    setTouchLinksDraft(source?.touchLinks.join(', ') ?? '');
    setAuxiliaryFramesDraft(source?.auxiliaryFrames.join(', ') ?? '');
    setError(null);
  }, [source]);
  const disabled = !controller.mutationAvailable || controller.busy;
  const listValue = (value: string) => Array.from(new Set(
    value.split(/[\n,]/).map((item) => item.trim()).filter(Boolean),
  ));
  const normalizedDraft = draft === null ? null : {
    ...draft,
    touchLinks: listValue(touchLinksDraft),
    auxiliaryFrames: listValue(auxiliaryFramesDraft),
  };
  const changed = source !== null
    && normalizedDraft !== null
    && JSON.stringify(gripperConfigurationValue(source))
      !== JSON.stringify(gripperConfigurationValue(normalizedDraft));
  const validate = (value: GripperTargetConfigurationDraft): string | null => {
    if (value.outputMode !== 'commanded' && value.outputMode !== 'none') {
      return 'Choose Send command or No output.';
    }
    if (value.outputMode === 'none' && value.actuationKind !== 'pneumatic') {
      return 'No output is available only for pneumatic grippers.';
    }
    if (
      value.description.trim() === ''
      || value.openStateRef === ''
      || value.closedStateRef === ''
      || !Number.isFinite(value.commandDurationS)
      || value.commandDurationS < 0
      || (value.actuationKind !== 'pneumatic' && value.commandDurationS === 0)
      || value.commandDurationS > 60
      || value.touchLinks.length === 0
      || value.mountFrame === ''
      || value.attachFrame === ''
      || value.tcpFrame === ''
    ) return value.actuationKind === 'pneumatic'
      ? 'Complete every required field and use a motion time from 0 to 60 s.'
      : 'Complete every required field and use a motion time above 0 and at most 60 s.';
    if (
      descriptor !== null
      && (!descriptor.namedPresets.some((preset) => preset.name === value.openStateRef)
        || !descriptor.namedPresets.some((preset) => preset.name === value.closedStateRef))
    ) return 'Open and closed references must name discovered SRDF presets.';
    return null;
  };
  const update = async () => {
    if (normalizedDraft === null) return;
    const validation = validate(normalizedDraft);
    if (validation !== null) {
      setError(validation);
      return;
    }
    const result = await controller.mutate(
      [
        ...runtimes.find((runtime) => runtime.index === normalizedDraft.runtimeIndex)!
          .configurationPath,
        'gripper_target_configurations', normalizedDraft.rowIndex,
      ],
      'set',
      gripperConfigurationValue(normalizedDraft),
    );
    setError(result === null ? 'The Deployment owner rejected this gripper configuration.' : null);
  };
  return (
    <details className="dcard">
      <summary>Current gripper configuration</summary>
      <div className="deployment-focused-body">
        {descriptor === null ? <p className="muted">No current addressed gripper descriptor is available.</p> : (
          <>
            {descriptors.length > 1 && <label className="deployment-focused-field">
              <span>Target</span>
              <select aria-label="Gripper target" value={descriptorKey(descriptor)}
                disabled={controller.busy} onChange={(event) => setSelectedTarget(event.target.value)}>
                {descriptors.map((candidate) => (
                  <option key={descriptorKey(candidate)} value={descriptorKey(candidate)}>
                    {candidate.controllableComponentId} / {candidate.commandTargetId}
                  </option>
                ))}
              </select>
            </label>}
            <div className="deployment-resolved-grid">
              <span><small>Target</small><strong>{descriptor.controllableComponentId} / {descriptor.commandTargetId}</strong></span>
              <span><small>Group / command joint</small><strong>{descriptor.sourceGroupName ?? 'not declared'} / {descriptor.sourceCoordinateName}</strong></span>
              <span><small>Range</small><strong>{format(descriptor.minPosition)} to {format(descriptor.maxPosition)} {descriptor.unit}</strong></span>
              <span><small>Command / actuation</small><strong>{descriptor.commandKind.replaceAll('_', ' ')} / {descriptor.actuationKind?.replaceAll('_', ' ') ?? 'not configured'}</strong></span>
              <span><small>Output</small><strong>{descriptor.outputMode === 'commanded' ? 'Send command' : descriptor.outputMode === 'none' ? 'No output' : 'not configured'}</strong></span>
              <span><small>Transport</small><strong>{descriptor.transportKind.replaceAll('_', ' ')} · {descriptor.controlInterfaceContractId ?? 'no resolved interface'}</strong></span>
              <span><small>State authority</small><strong>{descriptor.stateAuthorityClass ?? 'unavailable'} · {descriptor.stateAuthorityId ?? descriptor.sourceKind.replaceAll('_', ' ')}</strong></span>
              <span><small>Completion policy</small><strong>{descriptor.completionPolicy.replaceAll('_', ' ')}</strong></span>
              <span><small>Grasp confirmation</small><strong>{descriptor.graspConfirmationPolicy.replaceAll('_', ' ')}</strong></span>
              <span><small>Command capability / schema</small><strong>{descriptor.contracts.operationCapabilityId} · {shortIdentity(descriptor.contracts.commandSchema?.commandSchemaId)}</strong></span>
              <span><small>Presets</small><strong>{descriptor.namedPresets.map((preset) => preset.name).join(', ') || 'not declared'}</strong></span>
              <span><small>Mimic followers</small><strong>{descriptor.mimicFollowers.length}</strong></span>
            </div>
            {draft === null ? (
              <p className="muted">This addressed gripper has no Deployment-owned configuration row to edit.</p>
            ) : (
              <fieldset className="deployment-gripper-editor">
                <legend>Deployment-owned tool inputs</legend>
                <div className="deployment-focused-form-grid">
                  <label className="deployment-focused-field">
                    <span>Description</span>
                    <input
                      value={draft.description}
                      disabled={disabled}
                      onChange={(event) => setDraft({ ...draft, description: event.target.value })}
                    />
                  </label>
                  <label className="deployment-focused-field">
                    <span>Actuation kind</span>
                    <select
                      value={draft.actuationKind}
                      disabled={disabled}
                      onChange={(event) => setDraft({
                        ...draft,
                        actuationKind: event.target.value as GripperTargetConfigurationDraft['actuationKind'],
                      })}
                    >
                      <option value="position_commanded">Position commanded</option>
                      <option value="pneumatic">Pneumatic</option>
                    </select>
                  </label>
                  <label className="deployment-focused-field">
                    <span>Output</span>
                    <select
                      value={draft.outputMode}
                      disabled={disabled}
                      onChange={(event) => setDraft({
                        ...draft,
                        outputMode: event.target.value as GripperTargetConfigurationDraft['outputMode'],
                      })}
                    >
                      <option value="commanded">Send command</option>
                      {(draft.actuationKind === 'pneumatic' || draft.outputMode === 'none') && (
                        <option value="none" disabled={draft.actuationKind !== 'pneumatic'}>No output</option>
                      )}
                    </select>
                  </label>
                  <label className="deployment-focused-field">
                    <span>Open state</span>
                    <select
                      value={draft.openStateRef}
                      disabled={disabled}
                      onChange={(event) => setDraft({ ...draft, openStateRef: event.target.value })}
                    >
                      {descriptor.namedPresets.map((preset) => (
                        <option key={preset.name} value={preset.name}>{preset.name}</option>
                      ))}
                    </select>
                  </label>
                  <label className="deployment-focused-field">
                    <span>Closed state</span>
                    <select
                      value={draft.closedStateRef}
                      disabled={disabled}
                      onChange={(event) => setDraft({ ...draft, closedStateRef: event.target.value })}
                    >
                      {descriptor.namedPresets.map((preset) => (
                        <option key={preset.name} value={preset.name}>{preset.name}</option>
                      ))}
                    </select>
                  </label>
                  <label className="deployment-focused-field">
                    <span>{draft.actuationKind === 'pneumatic' ? 'Program dwell' : 'Motion time'}</span>
                    <span className="deployment-number-with-unit">
                      <input
                        type="number"
                        min={draft.actuationKind === 'pneumatic' ? '0' : '0.000001'}
                        max="60"
                        step="any"
                        value={draft.commandDurationS}
                        disabled={disabled}
                        onChange={(event) => setDraft({
                          ...draft,
                          commandDurationS: Number(event.target.value),
                        })}
                      />
                      <small>s</small>
                    </span>
                  </label>
                  {([
                    ['Mount frame', 'mountFrame'],
                    ['Attach frame', 'attachFrame'],
                    ['TCP frame', 'tcpFrame'],
                  ] as const).map(([label, field]) => (
                    <label className="deployment-focused-field" key={field}>
                      <span>{label}</span>
                      <input
                        value={draft[field]}
                        disabled={disabled}
                        onChange={(event) => setDraft({ ...draft, [field]: event.target.value })}
                      />
                    </label>
                  ))}
                  <label className="deployment-focused-field wide">
                    <span>Touch links</span>
                    <textarea
                      value={touchLinksDraft}
                      disabled={disabled}
                      onChange={(event) => setTouchLinksDraft(event.target.value)}
                    />
                  </label>
                  <label className="deployment-focused-field wide">
                    <span>Auxiliary frames</span>
                    <textarea
                      value={auxiliaryFramesDraft}
                      disabled={disabled}
                      onChange={(event) => setAuxiliaryFramesDraft(event.target.value)}
                    />
                  </label>
                </div>
                {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
                <div className="button-row">
                  <button
                    type="button"
                    className="primary-action"
                    disabled={disabled || !changed}
                    onClick={() => { void update(); }}
                  >Update gripper Draft</button>
                  <button
                    type="button"
                    disabled={disabled || !changed}
                    onClick={() => {
                      setDraft(source);
                      setTouchLinksDraft(source?.touchLinks.join(', ') ?? '');
                      setAuxiliaryFramesDraft(source?.auxiliaryFrames.join(', ') ?? '');
                      setError(null);
                    }}
                  >Discard gripper edits</button>
                </div>
              </fieldset>
            )}
            <p className="field-help">The Deployment owns tool geometry, presets, and motion time. Transport, state authority, completion, and grasp confirmation above are exact addressed-owner facts and remain read-only.</p>
          </>
        )}
      </div>
    </details>
  );
}

export function DeploymentFocusedEditor({
  controller,
  sourceModel,
  motionDescriptors,
  gripperDescriptors,
}: DeploymentFocusedEditorProps) {
  const document = controller.workspace?.draft.document ?? null;
  const runtimes = useMemo(() => runtimeConfigurations(document), [document]);
  const limits = useMemo(() => motionTargetLimits(runtimes), [runtimes]);
  const controlledStop = useMemo(() => controlledStopTargets(document), [document]);
  if (document === null) {
    return <div className="panel-state">Open a Deployment artifact to edit its Draft.</div>;
  }
  return (
    <section className="deployment-focused-editor" aria-labelledby="deployment-focused-title">
      <header className="scene-authoring-heading">
        <div>
          <p className="eyebrow">Focused compiler inputs</p>
          <h3 id="deployment-focused-title">Robot and runtime configuration</h3>
        </div>
      </header>
      <IsaacWorldDraft controller={controller} />
      <ModelAndTargetSection
        controller={controller}
        sourceModel={sourceModel}
        motionDescriptors={motionDescriptors}
        limits={limits}
        runtimes={runtimes}
      />
      <NativeRuntimeSection controller={controller} runtimes={runtimes} />
      <ControlledStopSection controller={controller} targets={controlledStop} />
      <GripperSection controller={controller} descriptors={gripperDescriptors} runtimes={runtimes} />
    </section>
  );
}
