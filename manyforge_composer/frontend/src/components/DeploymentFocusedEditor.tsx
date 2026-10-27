import { useEffect, useMemo, useState } from 'react';
import { ScenePoseSourcePolicySection } from './ScenePoseSourcePolicySection';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import {
  IoSignalConfigurationSection,
  IoSignalInputFields,
  IoSignalOutputFields,
  type GeometricContactEndpointOption,
} from './IoSignalConfigurationSection';
import {
  IO_DEVICE_MAXIMUM_LIFECYCLE_TIMEOUT_S,
  IO_DEVICE_PROVISIONAL_OUTPUT_ACK_TIMEOUT_S,
  ioDeviceComponentValue,
  ioDeviceHasAcknowledgedOutput,
  ioDeviceHasGpioOutput,
  ioProviderDeviceTimeout,
  ioProviderDeviceValue,
  isIoProviderComponent,
  lifecycleTimeoutProblem,
  newIoDeviceDraft,
  outputAckTimeoutProblem,
  validateIoDeviceDraft,
  withIoDirection,
  type IoDeviceDraft,
  type IoSignalInputDraft,
  type IoSignalOutputDraft,
} from '../utils/ioSignalConfiguration';
import type { StructuralResourceState } from '../state/structuralResource';
import {
  canonicalScope,
  globalRuleScopeGroups,
  globalRuleScopeKind,
  interruptKeyInitialProblem,
  GLOBAL_RULE_KEY_UNDECLARED,
  globalRuleChoices,
  globalRuleKeyNotes,
  GLOBAL_RULE_STATE_OPERAND_UNSUPPORTED,
  globalRuleProblem,
  commonStopStages,
  globalRulesOf,
  newGlobalRule,
  type TargetStopChain,
} from '../utils/interruptAuthoring';
import { DraftTextInput } from './common/DraftInputs';
import { InterruptConditionEditor } from './InterruptConditionEditor';
import { StopStagePicker } from './StopStagePicker';
import { StopFallbackSequence } from './StopFallbackSequence';
import { pruneFallbacks, recordedFallbacks } from '../utils/stopFallbackSequence';
import type {
  BlackboardKeyDeclaration,
  DeploymentControlledStopProfile,
  JsonValue,
  SourceModelProjection,
  SceneRuntimePoseSource,
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
  /** Keys a global rule may read: the Program Draft's declared and system keys; null without a Program. */
  programKeys?: readonly BlackboardKeyDeclaration[] | null;
  /** Published motion targets' qualified Stop chains (SX-2167). */
  stopChains?: readonly TargetStopChain[];
  /** The Applied Program's execution group ids; null when no Program is applied (contract 73). */
  executionGroupIds?: readonly string[] | null;
  poseSources?: readonly SceneRuntimePoseSource[];
}

const NO_PROGRAM_KEYS: readonly BlackboardKeyDeclaration[] = [];
const NO_STOP_CHAINS: readonly TargetStopChain[] = [];

interface RuntimeConfiguration {
  index: number;
  runtimeUnitId: string;
  configuration: Record<string, JsonValue>;
  configurationPath: Array<string | number>;
  intentTargetSettings: Record<string, JsonValue> | null;
  framePrefix: string | null;
  /** A model-free I/O device (runtime_kind io_provider): I/O sections only. */
  ioProvider: boolean;
  /** A model-free pose source (runtime_kind pose_source): shown read-only only. */
  poseSource: boolean;
}

const POSE_SOURCE_RUNTIME_KIND = 'pose_source';

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
  stateFreshnessTimeoutS: number;
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
        ioProvider: isIoProviderComponent(component ?? undefined),
        poseSource: configuration.runtime_kind === POSE_SOURCE_RUNTIME_KIND,
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
        ioProvider: false,
        poseSource: false,
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
      const stateFreshnessTimeoutS = numberValue(row?.state_freshness_timeout_s);
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
        || stateFreshnessTimeoutS === null
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
        stateFreshnessTimeoutS,
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
    state_freshness_timeout_s: draft.stateFreshnessTimeoutS,
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
        if ((mode !== 'soft_on_path' && mode !== 'spring_back_hold' && mode !== 'spring_back_fast')
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
        const fallbacks = recordedFallbacks(stop);
        if (fallbacks === null) return [];
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
            fallback_stop_modes: pruneFallbacks(mode, fallbacks),
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
      (mode !== 'soft_on_path' && mode !== 'spring_back_hold' && mode !== 'spring_back_fast')
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
    // Legacy `recoverable_substitution_mode` reads as [spring_back_hold];
    // carrying both fields is ambiguous (the backend refuses it).
    const fallbacks = recordedFallbacks(parameters);
    if (fallbacks === null) return [];
    return [{
      stop_mode: mode,
      deceleration_time_s: deceleration,
      allow_natural_completion_on_short_path: natural,
      jerk_limits_enabled: typeof parameters?.jerk_limits_enabled === 'boolean'
        ? parameters.jerk_limits_enabled : null,
      per_resource_limits: normalizedLimits,
      ...(maxStopTime === undefined ? {} : { max_stop_time_s: maxStopTime }),
      fallback_stop_modes: pruneFallbacks(mode, fallbacks),
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

// Bare number cell with the NumberDraft commit pattern: commits a valid value on
// Enter/blur only, Escape restores; for table rows that carry no field label.
function CommitNumberInput({
  value,
  min,
  disabled,
  ariaLabel,
  onCommit,
}: {
  value: number;
  min: number;
  disabled: boolean;
  ariaLabel: string;
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
    if (draft.trim() === '' || !Number.isFinite(parsed) || parsed < min) {
      setInvalid(true);
      return;
    }
    if (parsed === value) return;
    const result = await onCommit(parsed);
    setInvalid(result === null);
  };
  return (
    <input
      type="number"
      className={invalid ? 'draft-input-invalid' : undefined}
      value={draft}
      min={min}
      step="any"
      disabled={disabled}
      aria-label={ariaLabel}
      aria-invalid={invalid || undefined}
      title={invalid ? 'Value was rejected.' : undefined}
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
  ariaLabel,
}: {
  label: string;
  value: number | null;
  unit: string;
  disabled: boolean;
  onCommit: (value: number | null) => Promise<unknown> | unknown;
  maximum?: number;
  minimum?: number;
  placeholder?: string;
  ariaLabel?: string;
}) {
  const [draft, setDraft] = useState(value === null ? '' : String(value));
  const [invalid, setInvalid] = useState(false);
  const [rejected, setRejected] = useState(false);
  useEffect(() => {
    setDraft(value === null ? '' : String(value));
    setInvalid(false);
    setRejected(false);
  }, [value]);
  const commit = async () => {
    const parsed = draft.trim() === '' ? null : Number(draft);
    setRejected(false);
    if (parsed !== null && (!Number.isFinite(parsed) || parsed <= 0
      || (maximum !== undefined && parsed > maximum)
      || (minimum !== undefined && parsed < minimum))) {
      setInvalid(true);
      return;
    }
    if (parsed === value) return;
    const result = await onCommit(parsed);
    // A well-formed value the owner refused is not an input-format error.
    setInvalid(false);
    setRejected(result === null);
  };
  return (
    <label className={`deployment-focused-field${invalid || rejected ? ' invalid' : ''}`}>
      <span>{label}</span>
      <span className="deployment-number-with-unit">
        <input
          type="number"
          value={draft}
          min={minimum ?? 0.000001}
          max={maximum}
          step="any"
          placeholder={placeholder}
          aria-label={ariaLabel}
          disabled={disabled}
          onChange={(event) => {
            setDraft(event.target.value);
            setInvalid(false);
            setRejected(false);
          }}
          onBlur={() => { void commit(); }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              setDraft(value === null ? '' : String(value));
              setInvalid(false);
              setRejected(false);
              event.currentTarget.blur();
            }
          }}
        />
        <small>{unit}</small>
      </span>
      {rejected && <small className="deployment-field-error inline-message severity-error">Value was rejected.</small>}
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
  // Each edit commits the full profile straight to the Deployment Draft, like
  // the other Deployment fields; number inputs keep only their uncommitted text.
  const draft = target.profile;
  const disabled = !controller.mutationAvailable || controller.busy
    || target.profile?.jerk_limits_enabled == null;
  const requiresMigration = target.profile?.jerk_limits_enabled == null;
  const commit = (profile: DeploymentControlledStopProfile) => (requiresMigration ? null
    : controller.updateControlledStop({
      controllable_component_id: target.componentId,
      command_target_id: target.targetId,
      profile,
    }));
  const updateLimit = (
    index: number,
    field: 'max_deceleration' | 'settle_velocity_threshold',
    value: number,
  ) => {
    if (draft === null) return null;
    const next = structuredClone(draft);
    next.per_resource_limits[index][field].value = value;
    return commit(next);
  };
  return (
    <>
        <div className="deployment-focused-form-grid deployment-stop-form">
          {draft && <>
          <label className="deployment-focused-field">
            <span>Stop mode</span>
            <select
              value={draft.stop_mode}
              disabled={disabled}
              onChange={(event) => { void commit({
                ...draft,
                stop_mode: event.target.value as DeploymentControlledStopProfile['stop_mode'],
                allow_natural_completion_on_short_path: event.target.value === 'soft_on_path',
                // Only modes below the new primary stay fallbacks.
                fallback_stop_modes: pruneFallbacks(event.target.value, draft.fallback_stop_modes ?? []),
              }); }}
            >
              <option value="soft_on_path">Soft on path</option>
              <option value="spring_back_hold">Spring-back on path</option>
              <option value="spring_back_fast">Spring-back fast</option>
            </select>
          </label>
          {draft.stop_mode === 'spring_back_fast' && (
            <p className="field-help">
              Sent at once, with no planning or checks: the robot brakes, overshoots and returns to the
              stop-trigger position within the Stop time. Speed, acceleration and collision of that move are
              not checked, so the robot&apos;s own safety controller may trip. Meant for searches and probes
              stopped by an interrupt.
            </p>
          )}
          <StopFallbackSequence primary={draft.stop_mode} value={draft.fallback_stop_modes ?? []}
            disabled={disabled}
            onChange={(fallbacks) => { void commit({ ...draft, fallback_stop_modes: fallbacks }); }} />
          <NumberDraft
            label={draft.stop_mode === 'soft_on_path' ? 'Deceleration time' : 'Brake + return time'}
            value={draft.deceleration_time_s}
            unit="s"
            min={0.001}
            max={2}
            disabled={disabled}
            onCommit={(value) => {
              // As `main`: an explicit maximum Stop time never stays below the
              // deceleration time.
              return commit({
                ...draft,
                deceleration_time_s: value,
                ...(draft.max_stop_time_s !== undefined && draft.max_stop_time_s < value
                  ? { max_stop_time_s: value } : {}),
              });
            }}
          />
          <label className="toggle-control" title="Unselected: this robot's controlled Stop must finish within its deceleration time plus 0.5 s. Selecting sets an explicit maximum Stop time for this profile (never below the deceleration time); Save and Apply are still required.">
            <input type="checkbox" checked={draft.max_stop_time_s !== undefined} disabled={disabled}
              onChange={(event) => {
                const next = { ...draft };
                if (event.target.checked) next.max_stop_time_s = draft.deceleration_time_s + 0.5;
                else delete next.max_stop_time_s;
                void commit(next);
              }} />
            Explicit maximum Stop time
          </label>
          {draft.max_stop_time_s === undefined
            ? <p className="field-help">{`Automatic: ${Number((draft.deceleration_time_s + 0.5).toFixed(3))} s (deceleration time + 0.5 s).`}</p>
            : <NumberDraft label="Max Stop time" value={draft.max_stop_time_s}
              title="This robot's controlled Stop must finish within this time from the Stop request; on overrun its hard stop follows, bounded by the lane's stop-completion timeout."
              unit="s" min={0.001} max={9_007_199} disabled={disabled}
              onCommit={(value) => commit({
                ...draft, max_stop_time_s: Math.max(draft.deceleration_time_s, value),
              })} />}
          </>}
          {draft && <>
          <label className="toggle-control" title={draft.stop_mode !== 'soft_on_path'
            ? 'Off by default: brake and return to the stop-trigger position. Enabling permits a short remaining move to finish at its endpoint instead; that is not a precision return.'
            : 'On by default: a sufficiently short remaining move may finish at its endpoint, with rest confirmed within the Stop deadline.'}>
            <input
              type="checkbox"
              checked={draft.allow_natural_completion_on_short_path}
              disabled={disabled}
              onChange={(event) => { void commit({
                ...draft,
                allow_natural_completion_on_short_path: event.target.checked,
              }); }}
            />
            Allow natural completion on a short remaining path
          </label>
          <label className="deployment-focused-field" title="Constrains the controlled attempt only, not ordinary moves or hard containment. Missing per-joint values use the explicit ManyForge preset at preparation.">
            <span>Controlled Stop jerk</span>
            <select
              aria-label="Controlled Stop jerk limits"
              value={draft.jerk_limits_enabled === null ? '' : draft.jerk_limits_enabled ? 'on' : 'off'}
              disabled={disabled || requiresMigration}
              onChange={(event) => { void commit({ ...draft, jerk_limits_enabled: event.target.value === 'on' }); }}
            >
              <option value="" disabled>Review required: choose</option>
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
              <CommitNumberInput
                min={0.000001}
                value={limit.max_deceleration.value}
                disabled={disabled}
                ariaLabel={`${limit.resource_id} maximum deceleration`}
                onCommit={(value) => updateLimit(index, 'max_deceleration', value)}
              />
              <CommitNumberInput
                min={0}
                value={limit.settle_velocity_threshold.value}
                disabled={disabled}
                ariaLabel={`${limit.resource_id} settle velocity threshold`}
                onCommit={(value) => updateLimit(index, 'settle_velocity_threshold', value)}
              />
            </div>
          ))}
        </div>}
        {draft?.jerk_limits_enabled === true && (
          <details title="Empty uses the ManyForge acceleration/ramp preset. Explicit values are saved with this Deployment.">
            <summary>Joint jerk limits</summary>
            {draft.per_resource_limits.map((limit, index) => (
              <OptionalNumberDraft key={limit.resource_id}
                label={limit.resource_id.split('.').at(-1) ?? limit.resource_id}
                unit={limit.max_deceleration.coordinate_kind === 'angular_rad_s2' ? 'rad/s³' : 'm/s³'}
                ariaLabel={`${limit.resource_id} maximum jerk`}
                placeholder="ManyForge preset" value={limit.max_jerk?.value ?? null}
                disabled={disabled}
                onCommit={(value) => {
                  const next = structuredClone(draft);
                  if (value === null) delete next.per_resource_limits[index].max_jerk;
                  else next.per_resource_limits[index].max_jerk = {
                    coordinate_kind: limit.max_deceleration.coordinate_kind === 'angular_rad_s2' ? 'angular_rad_s3' : 'linear_m_s3',
                    value,
                  };
                  return commit(next);
                }}
              />
            ))}
          </details>
        )}
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
  // Feedback timeout = Deployment override of the gripper's `gripper_feedback`
  // stream budget; blank keeps the gripper row's `state_freshness_timeout_s`
  // (the one default the host also reads), shown as the placeholder.
  const policies = objectValue(controller.workspace?.draft.document?.policies) ?? {};
  const streamRows = (Array.isArray(policies.component_streams)
    ? policies.component_streams : []) as Array<Record<string, JsonValue>>;
  const isFeedbackRow = (row: Record<string, JsonValue>) => (
    row.controllable_component_id === descriptor?.controllableComponentId
    && row.stream_id === 'gripper_feedback'
  );
  const feedbackBudgetNs = numberValue(streamRows.find(isFeedbackRow)?.budget_ns);
  const updateFeedbackTimeout = async (timeoutMs: number | null) => {
    if (descriptor === null) return null;
    const rows = streamRows.filter((row) => !isFeedbackRow(row));
    const row = { ...(streamRows.find(isFeedbackRow) ?? {
      controllable_component_id: descriptor.controllableComponentId, stream_id: 'gripper_feedback',
    }) };
    if (timeoutMs === null) delete row.budget_ns;
    else row.budget_ns = Math.max(1, Math.round(timeoutMs * 1e6));
    // A row needs at least one override besides its identity.
    if (Object.keys(row).length > 2) rows.push(row);
    rows.sort((a, b) => (
      String(a.controllable_component_id).localeCompare(String(b.controllable_component_id))
      || String(a.stream_id).localeCompare(String(b.stream_id))
    ));
    const updated = { ...policies };
    if (rows.length) updated.component_streams = rows;
    else delete updated.component_streams;
    return controller.mutate(['policies'], 'set', updated);
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
            <OptionalNumberDraft
              label="Feedback timeout"
              value={feedbackBudgetNs === null ? null : feedbackBudgetNs / 1e6}
              unit="ms"
              placeholder={source === null
                ? 'gripper profile default'
                : `${format(source.stateFreshnessTimeoutS * 1000)} ms (profile default)`}
              disabled={disabled}
              onCommit={updateFeedbackTimeout}
            />
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
            <p className="field-help">The Deployment owns tool geometry, presets, motion time, and the feedback timeout (how long gripper feedback may stay silent before the gripper is reported stale; blank uses the gripper profile default). Transport, state authority, completion, and grasp confirmation above are exact addressed-owner facts and remain read-only.</p>
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
  programKeys = NO_PROGRAM_KEYS,
  stopChains = NO_STOP_CHAINS,
  executionGroupIds = null,
  poseSources = [],
}: DeploymentFocusedEditorProps) {
  const document = controller.workspace?.draft.document ?? null;
  const geometricContactEndpoints = useMemo(() => {
    const motionEndpoints = motionDescriptors.flatMap((descriptor) => descriptor.frames.map(
      (frame): GeometricContactEndpointOption => ({
        componentId: descriptor.controllableComponentId,
        targetId: descriptor.commandTargetId,
        frameRoleId: frame.frameRoleId,
      }),
    ));
    const retainedEndpoints = sourceModel.status === 'ready'
      ? sourceModel.data.targets.flatMap((target) => target.frames.map(
        (frame): GeometricContactEndpointOption => ({
          componentId: target.component_id,
          targetId: target.target_id,
          frameRoleId: frame.frame_role_id,
        }),
      )) : [];
    return [...new Map([...motionEndpoints, ...retainedEndpoints].map((endpoint) => [
      JSON.stringify([endpoint.componentId, endpoint.targetId, endpoint.frameRoleId]), endpoint,
    ])).values()];
  }, [motionDescriptors, sourceModel]);
  const runtimes = useMemo(() => runtimeConfigurations(document), [document]);
  // Model-free components (I/O devices, pose sources) have no model, motion,
  // gripper, frame or Stop inputs.
  const motionRuntimes = useMemo(
    () => runtimes.filter((runtime) => !runtime.ioProvider && !runtime.poseSource), [runtimes]);
  const limits = useMemo(() => motionTargetLimits(motionRuntimes), [motionRuntimes]);
  const controlledStop = useMemo(() => controlledStopTargets(document), [document]);
  const ioRuntimes = useMemo(() => runtimes.filter((runtime) => !runtime.poseSource).map((runtime) => ({
    index: runtime.index,
    runtimeUnitId: runtime.runtimeUnitId,
    configuration: runtime.configuration,
    configurationPath: runtime.configurationPath,
    intentComponent: runtime.intentTargetSettings !== null,
    ioProvider: runtime.ioProvider,
  })), [runtimes]);
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
      <ModelAndTargetSection
        controller={controller}
        sourceModel={sourceModel}
        motionDescriptors={motionDescriptors}
        limits={limits}
        runtimes={motionRuntimes}
      />
      <NativeRuntimeSection controller={controller} runtimes={motionRuntimes} />
      <ScenePoseSourcePolicySection controller={controller} sources={poseSources} />
      <ControlledStopSection controller={controller} targets={controlledStop} />
      <GripperSection controller={controller} descriptors={gripperDescriptors} runtimes={motionRuntimes} />
      {document.api_version === 'manyforge.deployment_intent.v2' && (
        <IoDeviceSection controller={controller} document={document}
          geometricContactEndpoints={geometricContactEndpoints} />
      )}
      <IoSignalConfigurationSection controller={controller} runtimes={ioRuntimes}
        geometricContactEndpoints={geometricContactEndpoints} />
      <PoseSourceComponentSection runtimes={runtimes} />
      {document.api_version === 'manyforge.deployment_intent.v2' && (
        <MotionTakeoverSection controller={controller} document={document} />
      )}
      {document.api_version === 'manyforge.deployment_intent.v2' && (
        <GlobalInterruptRulesSection controller={controller} document={document} programKeys={programKeys}
          stopChains={stopChains} executionGroupIds={executionGroupIds} />
      )}
    </section>
  );
}

const TAKEOVER_TOTAL_KEY = 'motion_takeover_preparation_budget_s';
const TAKEOVER_PLANNING_KEY = 'motion_takeover_planning_budget_s';
/** The backend's frozen derived default for an omitted planning budget. */
const TAKEOVER_DEFAULT_PLANNING_FRACTION = 0.8;

/**
 * Moving takeover timing in `policies`: the total preparation time (the
 * handoff horizon; absent = no takeover) and an optional planning budget
 * strictly below it (absent = derived, 80% of the total). The difference is
 * left for validation, the join and controller delivery.
 */
function MotionTakeoverSection({ controller, document }: {
  controller: DeploymentWorkspaceController;
  document: Record<string, JsonValue>;
}) {
  const policies = objectValue(document.policies);
  const total = numberValue(policies?.[TAKEOVER_TOTAL_KEY]);
  const planning = numberValue(policies?.[TAKEOVER_PLANNING_KEY]);
  const effectivePlanning = planning
    ?? (total === null ? null : total * TAKEOVER_DEFAULT_PLANNING_FRACTION);
  const [error, setError] = useState<string | null>(null);
  const disabled = !controller.mutationAvailable || controller.busy;
  const commit = async (key: string, value: number | null) => {
    const nextTotal = key === TAKEOVER_TOTAL_KEY ? value : total;
    const nextPlanning = key === TAKEOVER_PLANNING_KEY ? value : planning;
    if (nextPlanning !== null && nextTotal === null) {
      setError('Clear the planning budget before turning takeover off.');
      return null;
    }
    if (nextPlanning !== null && nextTotal !== null && nextPlanning >= nextTotal) {
      setError('The planning budget must be below the total preparation time.');
      return null;
    }
    const result = value === null
      ? await controller.mutate(['policies', key], 'remove')
      : policies === null
        ? await controller.mutate(['policies'], 'set', { [key]: value })
        : await controller.mutate(['policies', key], 'set', value);
    setError(result === null ? 'The Deployment owner rejected this timing change.' : null);
    return result;
  };
  return (
    <details className="dcard" open={total !== null}>
      <summary>Motion takeover timing</summary>
      <div className="deployment-focused-body">
        <p className="field-help">A moving takeover replaces the running motion with a newly accepted one without stopping first; if preparation does not finish in time, the takeover fallback applies. Both times count from the start of takeover preparation. The planning budget caps planning of the new motion (left empty, it is 80% of the total); the time left after it stays available for checking the result, joining it to the running motion and sending it to the controller, but does not guarantee delivery in time.</p>
        <OptionalNumberDraft label="Total preparation time (handoff horizon)" value={total} unit="s"
          placeholder="takeover off" ariaLabel="Takeover total preparation time" disabled={disabled}
          onCommit={(value) => commit(TAKEOVER_TOTAL_KEY, value)} />
        <OptionalNumberDraft label="Planning budget" value={planning} unit="s"
          placeholder={effectivePlanning === null ? 'takeover off' : `default ${format(effectivePlanning)}`}
          ariaLabel="Takeover planning budget" disabled={disabled || total === null}
          onCommit={(value) => commit(TAKEOVER_PLANNING_KEY, value)} />
        {total === null && <p className="muted">Takeover is off: a motion requested while another one runs is refused as busy.</p>}
        {total !== null && effectivePlanning !== null && (
          <p className="muted">
            {planning === null ? `Default planning budget: ${format(effectivePlanning)} s (80% of the total). ` : ''}
            Reserved after planning: {format(total - effectivePlanning)} s.
          </p>
        )}
        {error && <p className="deployment-field-error inline-message severity-error">{error}</p>}
      </div>
    </details>
  );
}

interface PoseSourceBinding {
  source: string;
  objectId: string;
  policyId: string;
  eligible: boolean | null;
  lifetimeNs: number | null;
}

/** The bindings of one pose_sources row: joint state, simulation entities, detections. */
function poseSourceBindings(row: Record<string, JsonValue>): PoseSourceBinding[] {
  const binding = (source: string, value: JsonValue | undefined): PoseSourceBinding[] => {
    const item = objectValue(value);
    return item === null ? [] : [{
      source,
      objectId: String(item.object_id ?? ''),
      policyId: String(item.policy_id ?? ''),
      eligible: typeof item.eligible === 'boolean' ? item.eligible : null,
      lifetimeNs: numberValue(item.lifetime_ns),
    }];
  };
  const list = (value: JsonValue | undefined) => (Array.isArray(value) ? value : []);
  return [
    ...binding('joint state', row.joint_state),
    ...list(row.simulation_entities).flatMap((item) => binding(
      String(objectValue(item)?.entity ?? 'simulation entity'), item)),
    ...list(row.detection3d_sources).flatMap((item) => binding('detection 3D', item)),
  ];
}

/**
 * Model-free pose-source components (pose_source_v0 / runtime_kind
 * pose_source): read-only. Their configuration is authored in the Deployment
 * file and kept unchanged by every Draft edit here (Check validates it).
 */
function PoseSourceComponentSection({ runtimes }: { runtimes: RuntimeConfiguration[] }) {
  const sources = runtimes.filter((runtime) => runtime.poseSource);
  if (sources.length === 0) return null;
  return (
    <details className="dcard">
      <summary>Pose sources</summary>
      <div className="deployment-focused-body">
        {sources.map((runtime) => {
          const runtimeConfiguration = objectValue(runtime.configuration.runtime_configuration);
          const poseSources = runtimeConfiguration?.pose_sources;
          const rows = Array.isArray(poseSources)
            ? poseSources.flatMap((value) => {
              const row = objectValue(value);
              return row === null ? [] : [row];
            })
            : [];
          return (
            <section className="deployment-pose-source" key={runtime.runtimeUnitId}
              aria-label={`${runtime.runtimeUnitId} pose source`}>
              <header><strong>{runtime.runtimeUnitId}</strong><small>Read-only</small></header>
              {rows.map((row, rowIndex) => (
                <div key={rowIndex}>
                  <p className="muted">Poll period: {numberValue(row.simulation_poll_period_ms) ?? 'not declared'} ms</p>
                  <table aria-label={`${runtime.runtimeUnitId} pose bindings`}>
                    <thead><tr><th>Object</th><th>Policy</th><th>Eligible</th><th>Lifetime</th><th>Source</th></tr></thead>
                    <tbody>
                      {poseSourceBindings(row).map((binding, index) => (
                        <tr key={index}>
                          <td>{binding.objectId}</td>
                          <td>{binding.policyId}</td>
                          <td>{binding.eligible === null ? '' : binding.eligible ? 'yes' : 'no'}</td>
                          <td>{binding.lifetimeNs === null ? '' : `${format(binding.lifetimeNs / 1e9)} s`}</td>
                          <td>{binding.source}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              ))}
              <details>
                <summary>Configuration</summary>
                <pre>{JSON.stringify(runtimeConfiguration, null, 2)}</pre>
              </details>
            </section>
          );
        })}
        <p className="field-help">A pose source is a model-free component that publishes object poses (and optionally the measured robot joint state) from the simulator or 3D detections. Its configuration is edited in the Deployment file; it has no model, pose, frames or Stop settings.</p>
      </div>
    </details>
  );
}

interface IoDeviceRow {
  index: number;
  componentId: string;
  runtimeConfiguration: Record<string, JsonValue> | null;
}

/**
 * Model-free I/O device components (io_device_v0 / runtime_kind io_provider):
 * add one (a single `components` append), edit its lifecycle command time
 * limit, or remove it. Its signals are edited in the I/O signals section. An
 * I/O device is not drawn in the 3D scene.
 */
function IoDeviceSection({ controller, document, geometricContactEndpoints }: {
  controller: DeploymentWorkspaceController;
  document: Record<string, JsonValue>;
  geometricContactEndpoints: readonly GeometricContactEndpointOption[];
}) {
  const components = Array.isArray(document.components) ? document.components : [];
  const componentIds = components.flatMap((value) => {
    const id = objectValue(value)?.component_id;
    return typeof id === 'string' ? [id] : [];
  });
  const devices: IoDeviceRow[] = components.flatMap((value, index) => {
    const component = objectValue(value);
    const componentId = component?.component_id;
    return isIoProviderComponent(value) && typeof componentId === 'string' ? [{
      index,
      componentId,
      runtimeConfiguration: objectValue(objectValue(component?.configuration)?.runtime_configuration),
    }] : [];
  });
  const [draft, setDraft] = useState<IoDeviceDraft | null>(null);
  const [error, setError] = useState<string | null>(null);
  const disabled = !controller.mutationAvailable || controller.busy;
  const update = (next: IoDeviceDraft) => {
    setDraft(next);
    setError(null);
  };
  const create = async () => {
    if (draft === null) return;
    const problem = validateIoDeviceDraft(draft, componentIds);
    if (problem !== null) {
      setError(problem);
      return;
    }
    const result = await controller.mutate(['components'], 'append', ioDeviceComponentValue(draft));
    if (result === null) setError('The Deployment owner rejected this I/O device.');
    else setDraft(null);
  };
  const signal = draft?.signal ?? null;
  return (
    <details className="dcard" open={devices.length > 0 || draft !== null}>
      <summary>I/O devices</summary>
      <div className="deployment-focused-body">
        {devices.length === 0 && draft === null && <p className="muted">No I/O device is declared.</p>}
        {devices.map((device) => (
          <IoDeviceEditor key={device.componentId} controller={controller} device={device} disabled={disabled} />
        ))}
        {draft === null ? (
          <div className="button-row">
            <button type="button" disabled={disabled}
              onClick={() => update(newIoDeviceDraft(componentIds))}>Add I/O device</button>
          </div>
        ) : (
          <fieldset className="deployment-gripper-editor">
            <legend>New I/O device</legend>
            <div className="deployment-focused-form-grid">
              <label className="deployment-focused-field">
                <span>Component id</span>
                <input aria-label="I/O device component id" value={draft.componentId} disabled={disabled}
                  onChange={(event) => update({
                    ...draft,
                    componentId: event.target.value,
                    signal: { ...draft.signal, draft: { ...draft.signal.draft, componentId: event.target.value } } as IoDeviceDraft['signal'],
                  })} />
              </label>
              <label className="deployment-focused-field">
                <span>Lifecycle command time limit</span>
                <span className="deployment-number-with-unit">
                  <input aria-label="I/O device lifecycle command time limit" inputMode="decimal"
                    value={draft.lifecycleTimeoutS} disabled={disabled}
                    title={`Enable/Disable of this device's driver must finish within this time (above 0, at most ${IO_DEVICE_MAXIMUM_LIFECYCLE_TIMEOUT_S} s).`}
                    onChange={(event) => update({ ...draft, lifecycleTimeoutS: event.target.value })} />
                  <small>s</small>
                </span>
              </label>
            </div>
            <p className="field-help">First signal (an I/O device needs at least one). Leave Resource role blank to use the signal name, and Source blank to use {draft.componentId}_source.</p>
            {signal?.kind === 'input' ? (
              <IoSignalInputFields index={0} draft={signal.draft} disabled={disabled} ioProvider
                geometricContactEndpoints={geometricContactEndpoints}
                onChange={(patch: Partial<IoSignalInputDraft>) => update({ ...draft, signal: { kind: 'input', draft: { ...signal.draft, ...patch } } })}
                onDirection={(direction) => update({ ...draft, signal: withIoDirection(signal, direction) as IoDeviceDraft['signal'] })} />
            ) : signal?.kind === 'output' ? (
              <IoSignalOutputFields index={0} draft={signal.draft} disabled={disabled} ioProvider
                onChange={(patch: Partial<IoSignalOutputDraft>) => update({ ...draft, signal: { kind: 'output', draft: { ...signal.draft, ...patch } } })}
                onDirection={(direction) => update({ ...draft, signal: withIoDirection(signal, direction) as IoDeviceDraft['signal'] })} />
            ) : null}
            {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
            <div className="button-row">
              <button type="button" className="primary-action" disabled={disabled}
                onClick={() => { void create(); }}>Create I/O device</button>
              <button type="button" disabled={controller.busy}
                onClick={() => { setDraft(null); setError(null); }}>Cancel</button>
            </div>
          </fieldset>
        )}
        <p className="field-help">An I/O device is a model-free component whose connection to its I/O (subscription or service client) is its own control domain: own Enable/Disable, time limit and re-sync recovery. Sources: Mock, generic ROS Bool / SetBool, Universal Robots io_states / set_io and ros2_control GPIO. I/O physically on a robot is declared here too: it shares the robot's driver without owning it, and the robot's Enable/Disable does not touch it. It has no model, pose, frames or Stop settings and is not drawn in the 3D scene.</p>
      </div>
    </details>
  );
}

function IoDeviceEditor({ controller, device, disabled }: {
  controller: DeploymentWorkspaceController;
  device: IoDeviceRow;
  disabled: boolean;
}) {
  const current = ioProviderDeviceTimeout(device.runtimeConfiguration, device.componentId);
  const rosOutputs = ioDeviceHasAcknowledgedOutput(device.runtimeConfiguration);
  const gpioOutputs = ioDeviceHasGpioOutput(device.runtimeConfiguration);
  const shownAck = current.outputAckTimeoutS ?? IO_DEVICE_PROVISIONAL_OUTPUT_ACK_TIMEOUT_S;
  const [text, setText] = useState(String(current.timeoutS));
  const [ackText, setAckText] = useState(String(shownAck));
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    setText(String(current.timeoutS));
    setError(null);
  }, [current.timeoutS]);
  useEffect(() => {
    setAckText(String(shownAck));
    setError(null);
  }, [shownAck]);
  // Writes the device's one row; the acknowledgement timeout is kept only
  // while the device has ROS SetBool / UR set_io outputs (contracts 68, 82).
  const commitRow = async (timeoutS: number, ackS: number | null) => {
    const result = await controller.mutate(
      ['components', device.index, 'configuration', 'runtime_configuration', 'io_provider_devices'],
      'set',
      [ioProviderDeviceValue(device.componentId, timeoutS, rosOutputs && ackS !== null ? ackS : undefined)],
    );
    setError(result === null ? 'The Deployment owner rejected this setting.' : null);
  };
  const commit = async () => {
    const problem = lifecycleTimeoutProblem(text);
    if (problem !== null) {
      setError(problem);
      return;
    }
    const value = Number(text.trim());
    if (current.exact && current.authored && value === current.timeoutS) return;
    await commitRow(value, current.outputAckTimeoutS);
  };
  const commitAck = async () => {
    const problem = outputAckTimeoutProblem(ackText);
    if (problem !== null) {
      setError(problem);
      return;
    }
    const value = Number(ackText.trim());
    if (current.exact && current.outputAckTimeoutS === value) return;
    if (current.outputAckTimeoutS === null && value === IO_DEVICE_PROVISIONAL_OUTPUT_ACK_TIMEOUT_S) return;
    await commitRow(current.timeoutS, value);
  };
  return (
    <section className="deployment-runtime-unit">
      <header><strong>{device.componentId}</strong><small>I/O device</small></header>
      {device.runtimeConfiguration === null ? (
        <p className="muted">This I/O device has no runtime configuration; its time limit cannot be edited here.</p>
      ) : (
        <div className="deployment-focused-form-grid">
          <label className={`deployment-focused-field${error !== null ? ' invalid' : ''}`}>
            <span>Lifecycle command time limit</span>
            <span className="deployment-number-with-unit">
              <input aria-label={`${device.componentId} lifecycle command time limit`} inputMode="decimal"
                value={text} disabled={disabled}
                onChange={(event) => { setText(event.target.value); setError(null); }}
                onBlur={() => { void commit(); }}
                onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} />
              <small>s</small>
            </span>
          </label>
          {rosOutputs && (
            <label className="deployment-focused-field">
              <span>Output acknowledgement timeout</span>
              <span className="deployment-number-with-unit">
                <input aria-label={`${device.componentId} output acknowledgement timeout`} inputMode="decimal"
                  value={ackText} disabled={disabled}
                  title="How long a ROS SetBool or UR set_io output write waits for the service's acceptance. Without it in time the write's effect is unknown and the output stays blocked until the device is re-synced."
                  onChange={(event) => { setAckText(event.target.value); setError(null); }}
                  onBlur={() => { void commitAck(); }}
                  onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} />
                <small>s</small>
              </span>
            </label>
          )}
        </div>
      )}
      {!current.authored && device.runtimeConfiguration !== null && (
        <p className="field-help">Not authored: the default {current.timeoutS} s applies.</p>
      )}
      {rosOutputs && current.outputAckTimeoutS === null && device.runtimeConfiguration !== null && (
        <p className="field-help">Output acknowledgement: provisional {IO_DEVICE_PROVISIONAL_OUTPUT_ACK_TIMEOUT_S} s (to be measured on the live lanes); Mock outputs are immediate.</p>
      )}
      {gpioOutputs && device.runtimeConfiguration !== null && (
        <p className="field-help">ros2_control GPIO outputs are submitted only: the gpio command controller gives no acknowledgement, so they have no acknowledgement timeout.</p>
      )}
      {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
      <div className="button-row">
        <button type="button" disabled={disabled}
          onClick={() => { void controller.mutate(['components', device.index], 'remove'); }}>Remove I/O device</button>
      </div>
    </section>
  );
}

const LATCH_RELEASE_HELP = 'A latch with no operation to Reset is cleared by the operator once its condition no longer holds (default), or automatically when the condition clears. A Program Reset never clears it.';
const IDLE_BEHAVIOUR_HELP = 'Default: latches while a Program Run is in flight and the robot is still; ignored when no Run exists. Moving motion is always stopped.';

/**
 * Global (Deployment) interrupt rules, `policies.interrupt_rules` (schema04
 * §3): each with its condition, scope, priority, idle behaviour and Stop
 * entry stage. A rule stops, latches and publishes the read-only key
 * `interrupt_state.<rule_id>`. The whole list is committed per change.
 */
export function GlobalInterruptRulesSection({
  controller, document, programKeys, stopChains = NO_STOP_CHAINS, executionGroupIds = null,
}: {
  controller: DeploymentWorkspaceController;
  document: Record<string, JsonValue>;
  /** Null when there is no Program: its user-key operands then wait for one (contract 106). */
  programKeys: readonly BlackboardKeyDeclaration[] | null;
  /** A global rule's entry stage must be qualified by every published robot chain. */
  stopChains?: readonly TargetStopChain[];
  /** The Applied Program's execution group ids; null when no Program is applied. */
  executionGroupIds?: readonly string[] | null;
}) {
  const rows = globalRulesOf(document);
  const stopStages = useMemo(() => commonStopStages(stopChains), [stopChains]);
  // Operands of a new rule; each rule's own list adds the keys it reads that no Program declares.
  const choices = useMemo(
    () => globalRuleChoices(document, programKeys, undefined),
    [document, programKeys],
  );
  const [error, setError] = useState<string | null>(null);
  const disabled = !controller.mutationAvailable || controller.busy;
  if (rows === null) {
    return (
      <details className="dcard">
        <summary>Global interrupt rules</summary>
        <p className="deployment-field-error inline-message severity-error">policies.interrupt_rules is not a list; it is kept unchanged and cannot be edited here.</p>
      </details>
    );
  }
  const ids = rows.map((row) => objectValue(row)?.rule_id);
  const commit = async (next: JsonValue[], index: number | null) => {
    if (index !== null) {
      const others = next.flatMap((row, at) => {
        const id = objectValue(row)?.rule_id;
        return at !== index && typeof id === 'string' ? [id] : [];
      });
      const problem = globalRuleProblem(next[index], others,
        globalRuleChoices(document, programKeys, objectValue(next[index])?.condition));
      if (problem !== null) {
        setError(problem);
        return;
      }
    }
    const result = next.length === 0
      ? await controller.mutate(['policies', 'interrupt_rules'], 'remove')
      : await controller.mutate(['policies', 'interrupt_rules'], 'set', next);
    setError(result === null ? 'The Deployment owner rejected this rule change.' : null);
  };
  const update = (index: number, patch: Record<string, JsonValue | undefined>) => {
    const current = objectValue(rows[index]) ?? {};
    const merged: Record<string, JsonValue> = { ...current };
    for (const [key, value] of Object.entries(patch)) {
      if (value === undefined) delete merged[key];
      else merged[key] = value;
    }
    void commit(rows.map((row, at) => (at === index ? merged : row)), index);
  };
  return (
    <details className="dcard" open={rows.length > 0}>
      <summary>Global interrupt rules</summary>
      <div className="deployment-focused-body">
        <p className="field-help">A global rule stops the motion in its scope when its condition holds, latches, and publishes the read-only key <code>interrupt_state.&lt;rule id&gt;</code> (clear, tripped or latched), always current. Program nodes react to a rule only through that key.</p>
        {rows.length === 0 && <p className="muted">No global interrupt rule is declared.</p>}
        {rows.map((row, index) => {
          const rule = objectValue(row) ?? {};
          const scopeKind = globalRuleScopeKind(rule.scope);
          const groups = globalRuleScopeGroups(rule.scope);
          // Applied groups first, then any stored id the Applied Program lacks (T2 refuses it at Apply).
          const groupChoices = [...new Set([...(executionGroupIds ?? []), ...groups])].sort();
          const groupsAvailable = executionGroupIds !== null && executionGroupIds.length > 0;
          const ruleId = typeof rule.rule_id === 'string' ? rule.rule_id : `rule ${index + 1}`;
          const ruleChoices = globalRuleChoices(document, programKeys, rule.condition);
          const keyNotes = globalRuleKeyNotes(rule.condition, programKeys);
          return (
            <fieldset className="deployment-gripper-editor" key={`${index}:${ruleId}`} aria-label={`Global rule ${ruleId}`}>
              <legend>{ruleId}</legend>
              <div className="deployment-focused-form-grid">
                <label className="deployment-focused-field">
                  <span>Rule id</span>
                  <DraftTextInput aria-label={`${ruleId} rule id`} value={typeof rule.rule_id === 'string' ? rule.rule_id : ''}
                    disabled={disabled}
                    validate={(text) => (/^[a-z][a-z0-9_.-]*$/.test(text.trim()) ? null : 'Use a portable lower-case id.')}
                    onCommit={(text) => update(index, { rule_id: text.trim() })} />
                </label>
                <label className="deployment-focused-field">
                  <span>Priority (0–100)</span>
                  <DraftTextInput aria-label={`${ruleId} priority`} inputMode="numeric"
                    value={typeof rule.priority === 'number' ? rule.priority : 0} disabled={disabled}
                    validate={(text) => (/^\d+$/.test(text.trim()) && Number(text) <= 100 ? null : 'Priority is an integer from 0 to 100.')}
                    onCommit={(text) => update(index, { priority: Number(text.trim()) })} />
                </label>
                <label className="deployment-focused-field">
                  <span>Scope</span>
                  <select aria-label={`${ruleId} scope`} value={scopeKind ?? ''} disabled={disabled}
                    onChange={(event) => {
                      const kind = event.target.value;
                      if (kind === 'execution_groups') {
                        const initial = groups.length > 0 ? groups : groupChoices.slice(0, 1);
                        if (initial.length > 0) update(index, { scope: canonicalScope(initial) as JsonValue });
                      } else if (kind === 'all_motion' || kind === 'everything') {
                        update(index, { scope: kind });
                      }
                    }}>
                    {scopeKind === null && <option value="">Unrecognized scope (kept)</option>}
                    <option value="all_motion">All motion</option>
                    <option value="execution_groups" disabled={!groupsAvailable && scopeKind !== 'execution_groups'}>
                      Execution groups{groupsAvailable ? '' : ' (apply a Program first)'}
                    </option>
                    <option value="everything">Everything (Full Stop)</option>
                  </select>
                </label>
                {scopeKind === 'execution_groups' && (
                  <fieldset className="deployment-focused-field" aria-label={`${ruleId} execution groups`}>
                    <legend>Execution groups (Applied Program)</legend>
                    {groupChoices.map((group) => {
                      const selected = groups.includes(group);
                      const known = executionGroupIds?.includes(group) ?? false;
                      return (
                        <label key={group}>
                          <input type="checkbox" aria-label={`${ruleId} group ${group}`} checked={selected}
                            disabled={disabled || (selected && groups.length === 1)}
                            onChange={() => update(index, {
                              scope: canonicalScope(selected
                                ? groups.filter((item) => item !== group) : [...groups, group]) as JsonValue,
                            })} />
                          {' '}{group}{known ? '' : ' (not in the Applied Program)'}
                        </label>
                      );
                    })}
                  </fieldset>
                )}
                <label className="deployment-focused-field" title={IDLE_BEHAVIOUR_HELP}>
                  <span>When nothing moves</span>
                  <select aria-label={`${ruleId} idle behaviour`} value={typeof rule.idle_behaviour === 'string' ? rule.idle_behaviour : ''}
                    disabled={disabled}
                    onChange={(event) => update(index, {
                      idle_behaviour: event.target.value === '' ? undefined : event.target.value,
                    })}>
                    <option value="">Default (latch during a Run, else ignore)</option>
                    <option value="latch">Latch</option>
                    <option value="ignore">Ignore (records one info row)</option>
                  </select>
                </label>
                <label className="deployment-focused-field" title={LATCH_RELEASE_HELP}>
                  <span>Latch release</span>
                  <select aria-label={`${ruleId} latch release`}
                    value={rule.latch_release === 'auto_on_clear' ? 'auto_on_clear' : ''}
                    disabled={disabled}
                    onChange={(event) => update(index, {
                      latch_release: event.target.value === '' ? undefined : event.target.value,
                    })}>
                    <option value="">Operator clears (default)</option>
                    <option value="auto_on_clear">Auto-clear when the condition clears</option>
                  </select>
                </label>
              </div>
              <div className="deployment-focused-field">
                <span>Condition</span>
                <InterruptConditionEditor label={`${ruleId} condition`} value={rule.condition} choices={ruleChoices}
                  disabled={disabled} onCommit={(condition) => update(index, { condition })} />
                {keyNotes.waiting.length > 0 && (
                  <p className="field-help" aria-label={`${ruleId} key waiting`}>
                    Waiting for a Program that declares {keyNotes.waiting.join(', ')}: the whole rule is inactive until then.
                  </p>
                )}
                {keyNotes.mixed && (
                  <p className="field-help" aria-label={`${ruleId} mixed operands`}>
                    This rule reads signals and keys, so it doesn&apos;t act while no Program is applied; use a separate signal-only rule if it must always act.
                  </p>
                )}
                {keyNotes.undeclared.length > 0 && (
                  <p className="deployment-field-error inline-message severity-error" aria-label={`${ruleId} key undeclared`}>
                    {GLOBAL_RULE_KEY_UNDECLARED}: the Program does not declare {keyNotes.undeclared.join(', ')}; Apply is refused until it does.
                  </p>
                )}
                {keyNotes.stateOperands.length > 0 && (
                  <p className="deployment-field-error inline-message severity-error" aria-label={`${ruleId} state operand`}>
                    {GLOBAL_RULE_STATE_OPERAND_UNSUPPORTED}: a global rule cannot read {keyNotes.stateOperands.join(', ')}; read rule states in the Program instead.
                  </p>
                )}
                {(() => {
                  const problem = interruptKeyInitialProblem(rule.condition, programKeys ?? []);
                  return problem === null ? null : (
                    <p className="deployment-field-error inline-message severity-error" aria-label={`${ruleId} key initial`}>{problem}</p>
                  );
                })()}
              </div>
              <div className="deployment-focused-field">
                <span>Stop entry stage</span>
                <StopStagePicker label={`${ruleId} stop entry stage`}
                  value={typeof rule.stop_entry_stage_id === 'string' ? rule.stop_entry_stage_id : undefined}
                  stages={stopStages} omittedLabel="First stage (default)" disabled={disabled}
                  onChange={(next) => update(index, { stop_entry_stage_id: next })} />
              </div>
              <div className="button-row">
                <button type="button" disabled={disabled}
                  onClick={() => { void commit(rows.filter((_, at) => at !== index), null); }}>Remove rule</button>
              </div>
            </fieldset>
          );
        })}
        {error !== null && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
        <div className="button-row">
          <button type="button" disabled={disabled || choices.length === 0}
            title={choices.length === 0 ? 'Declare an input signal or a Program key first.' : undefined}
            onClick={() => {
              const rule = newGlobalRule(ids.filter((id): id is string => typeof id === 'string'), choices[0]);
              void commit([...rows, rule as unknown as JsonValue], rows.length);
            }}>Add global rule</button>
        </div>
      </div>
    </details>
  );
}
