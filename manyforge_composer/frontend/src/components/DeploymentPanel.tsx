import { memo, useCallback, useEffect, useMemo, useState } from 'react';
import { useShallow } from 'zustand/react/shallow';
import { api } from '../api/client';
import type {
  AssistantConfig,
  FullDeployment,
  ModelEndpointCapabilities,
  ModelEndpointCatalog,
  ModelEndpointConfig,
  PoseSourceApplyPolicy,
  PoseSourceConfig,
  PoseSourceKind,
  PoseSourceObjectBindingConfig,
  SceneFrameState,
  SceneUrdfManifest,
  RobotControlJointLimit,
} from '../types/api';
import { useDeploymentStore } from '../store/deploymentStore';
import { useEditLocked } from '../store/cycleStore';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import {
  DEFAULT_CONTROLLED_STOP_DECELERATION_TIME_S,
  DEFAULT_SOFT_ON_PATH_DECELERATION_LIMIT_RAD_S2,
  DEFAULT_SOFT_ON_PATH_LEAD_IN_S,
  DEFAULT_SOFT_ON_PATH_SAMPLE_PERIOD_S,
  FAULT_STOP_SLOT,
  KNOWN_STOP_MODES,
  MAX_CONTROLLED_STOP_DECELERATION_TIME_S,
  OPERATOR_STOP_SLOT,
  buildStopProfileView,
  hardStopLevelLabel,
  stopModeLabel,
} from '../store/stopProfile';
import { DraftTextarea, DraftTextInput } from './common/DraftInputs';

function signature(value: unknown): string {
  return JSON.stringify(value ?? null);
}

function FieldInput({
  label,
  value,
  onChange,
  type = 'text',
  disabled = false,
  help,
  placeholder,
  min,
  step,
}: {
  label: string;
  value: string | number;
  onChange: (v: string) => void;
  type?: string;
  disabled?: boolean;
  help?: string;
  placeholder?: string;
  min?: number;
  step?: number;
}) {
  return (
    <label className="block mb-2" title={help}>
      <FieldLabel label={label} help={help} />
      <DraftTextInput
        type={type}
        value={value}
        onCommit={onChange}
        disabled={disabled}
        min={min}
        step={step}
        placeholder={placeholder}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
      />
    </label>
  );
}

function FieldLabel({
  label,
  help,
  className = 'text-xs text-gray-400',
}: {
  label: string;
  help?: string;
  className?: string;
}) {
  return (
    <span className={className}>
      {label}
      {help !== undefined && (
        <span
          className="ml-1 cursor-help text-gray-500"
          aria-label={help}
          title={help}
        >
          ⓘ
        </span>
      )}
    </span>
  );
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div>
      <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wide mb-1">
        {title}
      </h3>
      <div className="space-y-1">{children}</div>
    </div>
  );
}

function ConfigTextarea({
  label,
  value,
  onChange,
  disabled = false,
}: {
  label: string;
  value: Record<string, unknown>;
  onChange: (v: Record<string, unknown>) => void;
  disabled?: boolean;
}) {
  const [error, setError] = useState<string | null>(null);
  const textValue = useMemo(() => JSON.stringify(value, null, 2), [value]);

  return (
    <label className="block mb-2">
      <span className="text-xs text-gray-400">{label}</span>
      <DraftTextarea
        rows={3}
        value={textValue}
        disabled={disabled}
        onCommit={(nextValue) => {
          try {
            onChange(JSON.parse(nextValue));
            setError(null);
          } catch {
            setError('Invalid JSON');
          }
        }}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-xs text-gray-200 font-mono focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
      />
      {error && <span className="mt-1 block text-[11px] text-red-400">{error}</span>}
    </label>
  );
}

function FrameSelectInput({
  label,
  value,
  options,
  onChange,
  disabled = false,
}: {
  label: string;
  value: string;
  options: string[];
  onChange: (value: string) => void;
  disabled?: boolean;
}) {
  const selectOptions = Array.from(new Set([value, ...options].filter(Boolean))).sort();

  if (selectOptions.length === 0) {
    return (
      <FieldInput
        label={label}
        value={value}
        onChange={onChange}
        disabled={disabled}
      />
    );
  }

  return (
    <label className="block mb-2">
      <span className="text-xs text-gray-400">{label}</span>
      <select
        value={value}
        onChange={(e) => onChange(e.target.value)}
        disabled={disabled}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
      >
        <option value="">Select frame</option>
        {selectOptions.map((frameName) => (
          <option key={frameName} value={frameName}>
            {frameName}
          </option>
        ))}
      </select>
    </label>
  );
}

function StringListInput({
  label,
  values,
  onChange,
  disabled = false,
  help,
  placeholder,
}: {
  label: string;
  values: string[];
  onChange: (values: string[]) => void;
  disabled?: boolean;
  help?: string;
  placeholder?: string;
}) {
  return (
    <label className="block mb-2" title={help}>
      <FieldLabel label={label} help={help} />
      <DraftTextInput
        type="text"
        value={values.join(', ')}
        placeholder={placeholder}
        onCommit={(nextValue) => {
          onChange(
            nextValue
              .split(',')
              .map((value) => value.trim())
              .filter(Boolean),
          );
        }}
        disabled={disabled}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
      />
    </label>
  );
}

function CheckboxInput({
  label,
  checked,
  onChange,
  disabled = false,
  help,
}: {
  label: string;
  checked: boolean;
  onChange: (value: boolean) => void;
  disabled?: boolean;
  help?: string;
}) {
  return (
    <label
      className="mb-2 flex items-center gap-2 text-sm text-gray-200"
      title={help}
    >
      <input
        type="checkbox"
        checked={checked}
        onChange={(event) => onChange(event.target.checked)}
        disabled={disabled}
      />
      <FieldLabel label={label} help={help} className="text-sm text-gray-200" />
    </label>
  );
}

const DEFAULT_MODEL_ENDPOINT_CAPABILITIES: ModelEndpointCapabilities = {
  supportsResponseTools: false,
  supportsJsonObject: false,
  supportsImageInput: false,
  supportedImageTransports: [],
  toolCallFamily: '',
  latencyBudgetMs: null,
};

const POSE_SOURCE_KINDS: PoseSourceKind[] = [
  'simulation_interfaces',
  'pose_stamped_topic',
  'pose_array_topic',
  'detection3d_array_topic',
  'tf',
];

const POSE_SOURCE_APPLY_POLICIES: PoseSourceApplyPolicy[] = [
  'observe_only',
  'when_idle',
  'continuous',
  'continuous_unattached',
  'before_planning',
  'between_nodes',
];

function modelEndpointCapabilities(
  endpoint: ModelEndpointConfig,
): ModelEndpointCapabilities {
  return {
    ...DEFAULT_MODEL_ENDPOINT_CAPABILITIES,
    ...(endpoint.capabilities ?? {}),
  };
}

function deploymentWarnings(deployment: FullDeployment): string[] {
  const warnings: string[] = [];
  const endpoints = deployment.modelEndpoints ?? {};
  const endpointValues = Object.values(endpoints);
  const anyResponseTools = endpointValues.some(
    (endpoint) => modelEndpointCapabilities(endpoint).supportsResponseTools,
  );
  const anyJsonObject = endpointValues.some(
    (endpoint) => modelEndpointCapabilities(endpoint).supportsJsonObject,
  );

  const templates = Object.values(deployment.promptTemplates ?? {});
  if (templates.some((template) => template.responseTool) && !anyResponseTools) {
    warnings.push('A prompt template declares a response tool but no model endpoint declares response-tool support.');
  }
  if (templates.some((template) => template.outputSchema) && !anyJsonObject) {
    warnings.push('A prompt template declares JSON schema output but no model endpoint declares JSON-object support.');
  }
  for (const [alias, endpoint] of Object.entries(endpoints)) {
    const capabilities = modelEndpointCapabilities(endpoint);
    if (endpoint.imageFetchBaseUrl && !capabilities.supportsImageInput) {
      warnings.push(`Endpoint ${alias} has an image fetch base URL but does not declare image input support.`);
    }
    if (
      capabilities.supportsImageInput
      && capabilities.supportedImageTransports.length === 0
    ) {
      warnings.push(`Endpoint ${alias} declares image input support but no supported image transports.`);
    }
  }

  const assistant = deployment.assistant;
  if (assistant?.enabled && ['nemoclaw', 'openclaw'].includes(assistant.provider)) {
    const endpoint = endpoints[assistant.endpointAlias];
    if (!assistant.endpointAlias) {
      warnings.push(`Assistant provider ${assistant.provider} requires an endpoint alias.`);
    } else if (!endpoint) {
      warnings.push(`Assistant endpoint alias ${assistant.endpointAlias} is not configured.`);
    } else {
      const capabilities = modelEndpointCapabilities(endpoint);
      if (!endpoint.baseUrl) {
        warnings.push(`Assistant endpoint ${assistant.endpointAlias} needs a base URL.`);
      }
      if (!capabilities.supportsResponseTools) {
        warnings.push(`Assistant endpoint ${assistant.endpointAlias} should declare response-tool support.`);
      }
    }
  }

  return warnings;
}

function nextPoseSourceId(sources: PoseSourceConfig[]): string {
  const existing = new Set(sources.map((source) => source.id));
  let index = 1;
  while (existing.has(`pose_source_${index}`)) {
    index += 1;
  }
  return `pose_source_${index}`;
}

function defaultPoseSource(id: string): PoseSourceConfig {
  return {
    id,
    kind: 'simulation_interfaces',
    enabled: true,
    maxUpdateHz: 4,
    staleAfterS: 1,
    frameId: 'universe',
    servicePrefix: 'auto',
    topic: '',
    description: '',
    measuredTcp: null,
    measuredRobot: null,
    objects: {},
  };
}

function defaultPoseSourceObject(): PoseSourceObjectBindingConfig {
  return {
    entity: '',
    frameId: '',
    applyPolicy: 'observe_only',
    confidenceMin: null,
    measuredStateGhost: {
      enabled: false,
      robotTcp: true,
      object: true,
      opacity: 0.5,
    },
    measuredAttachmentBaseline: {
      capture: 'on_attach_or_first_paired_sample',
      requireSameTimeRobotTcp: true,
    },
    attachedDriftMonitor: {
      enabled: false,
      translationThresholdM: 0.02,
      rotationThresholdRad: 0.2,
      debounceS: 0.1,
      staleBehavior: 'fault',
      action: 'interrupt',
      signalId: '',
      sampleTimeToleranceS: 0.05,
    },
    metadata: {},
  };
}

function FrameMultiSelect({
  label,
  options,
  selected,
  onChange,
  disabled = false,
}: {
  label: string;
  options: string[];
  selected: string[];
  onChange: (values: string[]) => void;
  disabled?: boolean;
}) {
  const knownOptions = Array.from(new Set([...selected, ...options].filter(Boolean))).sort();

  if (knownOptions.length === 0) {
    return (
      <StringListInput
        label={label}
        values={selected}
        onChange={onChange}
        disabled={disabled}
      />
    );
  }

  return (
    <div className="mb-2">
      <div className="text-xs text-gray-400">{label}</div>
      <div className="mt-1 rounded border border-gray-700 bg-gray-900/40 p-2 space-y-1">
        {knownOptions.map((frameName) => {
          const checked = selected.includes(frameName);
          return (
            <label key={frameName} className="flex items-center gap-2 text-sm text-gray-200">
              <input
                type="checkbox"
                checked={checked}
                disabled={disabled}
                onChange={(event) => {
                  const next = event.target.checked
                    ? [...selected, frameName]
                    : selected.filter((value) => value !== frameName);
                  onChange(Array.from(new Set(next)).sort());
                }}
              />
              <span>{frameName}</span>
            </label>
          );
        })}
      </div>
    </div>
  );
}

/**
 * Per-joint validated Robot Control velocity/acceleration limits editor
 * (spec/430 §3.2 robot-control-joint-limits).
 *
 * Renders one row per drivable joint with two number inputs
 * (max velocity rad/s, max acceleration rad/s²). Empty input =
 * "use engine default" (0.5 rad/s velocity, 2.0 rad/s² acceleration);
 * the placeholder reminds the operator what the default is.
 *
 * Persistence: the editor writes to
 * `deployment.robotConfig.robotControlJointLimits` (Record<joint, limit>);
 * joints with EVERY field empty are omitted from the saved YAML so a
 * "no overrides" deployment stays minimal.
 */

/**
 * Pure reducer for one per-joint cap edit (Phase 4-P5.1K). Returns the next
 * limits record. Invalid numeric input (non-finite or <= 0) is rejected and the
 * SAME `limits` reference is returned unchanged (the caller can skip onChange).
 * A joint is dropped only when velocity, acceleration AND jerk are all empty, so
 * a jerk-only joint is never silently dropped on a vel/accel edit. Exported for
 * unit testing.
 */
export function applyRobotControlJointLimitEdit(
  limits: Record<string, RobotControlJointLimit>,
  joint: string,
  field: 'maxVelocityRadS' | 'maxAccelerationRadS2' | 'maxJerkRadS3',
  raw: string,
): Record<string, RobotControlJointLimit> {
  const trimmed = raw.trim();
  const next: Record<string, RobotControlJointLimit> = { ...limits };
  const current: RobotControlJointLimit = { ...(next[joint] ?? {}) };
  if (trimmed === '') {
    current[field] = null;
  } else {
    const v = Number(trimmed);
    if (!Number.isFinite(v) || v <= 0) return limits;
    current[field] = v;
  }
  if (
    current.maxVelocityRadS == null &&
    current.maxAccelerationRadS2 == null &&
    current.maxJerkRadS3 == null
  ) {
    delete next[joint];
  } else {
    next[joint] = current;
  }
  return next;
}

function RobotControlJointLimitsEditor({
  drivableJointNames,
  limits,
  onChange,
  disabled,
}: {
  drivableJointNames: string[];
  limits: Record<string, RobotControlJointLimit>;
  onChange: (next: Record<string, RobotControlJointLimit>) => void;
  disabled: boolean;
}) {
  if (drivableJointNames.length === 0) {
    return (
      <div className="text-xs italic text-gray-500">
        URDF manifest not loaded yet (or no drivable joints found). Load a
        program with a robot to edit per-joint Robot Control limits.
      </div>
    );
  }

  function commit(
    joint: string,
    field: 'maxVelocityRadS' | 'maxAccelerationRadS2' | 'maxJerkRadS3',
    raw: string,
  ): void {
    const next = applyRobotControlJointLimitEdit(limits, joint, field, raw);
    if (next !== limits) onChange(next);
  }

  return (
    <div className="space-y-1">
      <div className="text-[10px] text-gray-500">
        Per-joint caps bounding operator-authored Robot Control targets.
        Leave blank to use engine defaults (0.5 rad/s, 2.0 rad/s²). Velocity
        is scaled by the operator's velocity slider; acceleration is an
        absolute physical limit. Jerk is carried through to the kernel but is
        only enforced where the active planner supports it (the discrete path
        rejects a jerk cap rather than silently ignoring it).
      </div>
      <div className="grid grid-cols-[1fr_5rem_5rem_5rem] items-center gap-2 border-b border-gray-700/40 pb-1 text-[10px] uppercase tracking-wide text-gray-500">
        <span>Joint</span>
        <span className="text-right">vel (rad/s)</span>
        <span className="text-right">accel (rad/s²)</span>
        <span className="text-right">jerk (rad/s³)</span>
      </div>
      {drivableJointNames.map((name) => {
        const limit = limits[name] ?? {};
        return (
          <div key={name} className="grid grid-cols-[1fr_5rem_5rem_5rem] items-center gap-2">
            <span className="truncate font-mono text-[11px] text-gray-200">{name}</span>
            <DraftTextInput
              value={limit.maxVelocityRadS == null ? '' : String(limit.maxVelocityRadS)}
              onCommit={(v) => commit(name, 'maxVelocityRadS', v)}
              placeholder="0.5"
              disabled={disabled}
              className="w-full rounded bg-gray-800 px-1 py-0.5 text-right font-mono text-[10px] text-gray-200"
              inputMode="decimal"
            />
            <DraftTextInput
              value={limit.maxAccelerationRadS2 == null ? '' : String(limit.maxAccelerationRadS2)}
              onCommit={(v) => commit(name, 'maxAccelerationRadS2', v)}
              placeholder="2.0"
              disabled={disabled}
              className="w-full rounded bg-gray-800 px-1 py-0.5 text-right font-mono text-[10px] text-gray-200"
              inputMode="decimal"
            />
            <DraftTextInput
              value={limit.maxJerkRadS3 == null ? '' : String(limit.maxJerkRadS3)}
              onCommit={(v) => commit(name, 'maxJerkRadS3', v)}
              placeholder="—"
              disabled={disabled}
              className="w-full rounded bg-gray-800 px-1 py-0.5 text-right font-mono text-[10px] text-gray-200"
              inputMode="decimal"
            />
          </div>
        );
      })}
    </div>
  );
}

type ControlledStopProfileSlot = typeof OPERATOR_STOP_SLOT | typeof FAULT_STOP_SLOT;

const CONTROLLED_STOP_PROFILE_SLOTS: Array<{
  slot: ControlledStopProfileSlot;
  label: string;
}> = [
  { slot: OPERATOR_STOP_SLOT, label: 'Operator stop (Stop button)' },
  { slot: FAULT_STOP_SLOT, label: 'Fault stop' },
];

const DEFAULT_ROS2_CONTROL_STOP_COMPLETION_TIMEOUT_S = 0.5;
const DEFAULT_SETTLE_VELOCITY_THRESHOLD_RAD_S = 0.01;

export const CONTROLLED_STOP_FIELD_HELP = {
  mode:
    'Selects the controlled-stop mechanism for this slot. Unsupported modes are disabled. Executable deployments must configure a real operator stop; an absent profile is shown only as a disabled placeholder.',
  decelerationTime:
    `Requested wall-time stop duration. Must be finite, greater than 0, and <= ${MAX_CONTROLLED_STOP_DECELERATION_TIME_S.toFixed(1)} s. If left unset, ManyForge uses ${DEFAULT_CONTROLLED_STOP_DECELERATION_TIME_S.toFixed(1)} s.`,
  maxStopTime:
    'Optional watchdog deadline for the whole stop, including measured-settle confirmation. Leave blank to use the auto deadline shown in the placeholder; if the robot is not stopped by then, ManyForge reports a stop failure or escalation.',
  stopCompletionTimeout:
    'ros2_control auto-deadline margin stored as stop_completion_timeout_s. The input shows the effective auto max stop time and must be greater than the largest requested stop duration; ManyForge stores only the extra margin.',
  settleVelocity:
    `Optional measured-rest velocity threshold in rad/s. Leave blank to use the default shown in the placeholder (${DEFAULT_SETTLE_VELOCITY_THRESHOLD_RAD_S} rad/s). If measured velocity stays above this threshold before max stop time, the stop is not certified as settled.`,
  naturalCompletion:
    'Soft on-path only. If the accepted trajectory will naturally stop before the requested stop window, ManyForge keeps Stopping active and lets that move finish instead of injecting a new stop trajectory.',
  maxDeceleration:
    `Per-joint acceleration/deceleration limit for soft on-path and spring-back planning. Leave blank to use ${DEFAULT_SOFT_ON_PATH_DECELERATION_LIMIT_RAD_S2} rad/s². Missing or too-low values can force natural completion, fallback, or hard-stop escalation.`,
  leadIn:
    `Command-frame lead-in before deceleration. Must be positive and shorter than the stop time. Leave blank to use ${DEFAULT_SOFT_ON_PATH_LEAD_IN_S} s. Too large a lead-in leaves too little decel time and can force hard-stop disposition.`,
  samplePeriod:
    `Waypoint spacing for generated stop trajectories. Leave blank to use ${DEFAULT_SOFT_ON_PATH_SAMPLE_PERIOD_S} s. Smaller values give finer validation but more waypoints; invalid values are rejected.`,
  pathRamp:
    'Enables backend-authored timed replacement trajectories for soft on-path. Turning it off removes the timed on-path stop guarantee and may cause the backend not to advertise soft_on_path.',
  maxJerk:
    'Optional per-joint jerk limit for soft on-path and spring-back planning. Leave blank to derive a limit from deceleration and stop duration. Too-low jerk can make exact-time stopping infeasible and trigger fallback or hard-stop disposition.',
} as const;

export function controlledStopFieldHelp(
  field: keyof typeof CONTROLLED_STOP_FIELD_HELP,
): string {
  return CONTROLLED_STOP_FIELD_HELP[field];
}

function formatControlNumber(value: number): string {
  return value.toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
}

function finiteNonNegative(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0
    ? value
    : null;
}

export function controlledStopCompletionToleranceS(
  executionConfig: Record<string, unknown>,
  executionAdapterId: string | null | undefined,
): number {
  if (executionAdapterId === 'execution.ros2_control_follow_joint_trajectory') {
    const configuredRosTolerance = finiteNonNegative(executionConfig.stop_completion_timeout_s);
    if (configuredRosTolerance != null) {
      return configuredRosTolerance;
    }
    return DEFAULT_ROS2_CONTROL_STOP_COMPLETION_TIMEOUT_S;
  }
  return finitePositive(executionConfig.soft_on_path_sample_period_s) ??
    DEFAULT_SOFT_ON_PATH_SAMPLE_PERIOD_S;
}

export function controlledStopAutoMaxStopTimeS(
  executionConfig: Record<string, unknown>,
  executionAdapterId: string | null | undefined,
  decelerationTimeS: number,
): number {
  return decelerationTimeS +
    controlledStopCompletionToleranceS(executionConfig, executionAdapterId);
}

export function controlledStopMaxRequestedStopTimeS(
  executionConfig: Record<string, unknown>,
): number {
  return Math.max(
    ...CONTROLLED_STOP_PROFILE_SLOTS.map(({ slot }) =>
      stopDecelerationTime(stopProfileFromConfig(executionConfig, slot)),
    ),
  );
}

export function applyControlledStopAutoDeadlineEdit(
  executionConfig: Record<string, unknown>,
  raw: string,
): Record<string, unknown> {
  const parsed = Number(raw);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return executionConfig;
  }
  const requested = controlledStopMaxRequestedStopTimeS(executionConfig);
  if (parsed <= requested) {
    return executionConfig;
  }
  return {
    ...executionConfig,
    stop_completion_timeout_s: parsed - requested,
  };
}

function asRecord(value: unknown): Record<string, unknown> {
  if (value == null || typeof value !== 'object' || Array.isArray(value)) {
    return {};
  }
  return { ...(value as Record<string, unknown>) };
}

function finitePositive(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) && value > 0
    ? value
    : null;
}

function finitePositiveRecord(value: unknown): Record<string, number> {
  const raw = asRecord(value);
  const result: Record<string, number> = {};
  for (const [key, item] of Object.entries(raw)) {
    const parsed = finitePositive(item);
    if (key.length > 0 && parsed != null) {
      result[key] = parsed;
    }
  }
  return result;
}

function profileIdForSlot(slot: ControlledStopProfileSlot, mode: string): string {
  return `${slot === OPERATOR_STOP_SLOT ? 'operator' : 'fault'}_${mode}`;
}

function stopProfileFromConfig(
  executionConfig: Record<string, unknown>,
  slot: ControlledStopProfileSlot,
): Record<string, unknown> {
  return asRecord(executionConfig[slot]);
}

function configuredStopMode(
  profile: Record<string, unknown>,
  fallback: string,
): string {
  return typeof profile.stop_mode === 'string' && profile.stop_mode.length > 0
    ? profile.stop_mode
    : fallback;
}

function configuredStopModeOrNull(
  profile: Record<string, unknown>,
): string | null {
  return typeof profile.stop_mode === 'string' && profile.stop_mode.length > 0
    ? profile.stop_mode
    : null;
}

function stopDecelerationTime(profile: Record<string, unknown>): number {
  return finitePositive(profile.deceleration_time_s) ??
    DEFAULT_CONTROLLED_STOP_DECELERATION_TIME_S;
}

export function normalizeControlledStopProfileForMode(
  profile: Record<string, unknown>,
  slot: ControlledStopProfileSlot,
  mode: string,
  jointNames: string[],
): Record<string, unknown> {
  const decelerationTime = Math.min(
    MAX_CONTROLLED_STOP_DECELERATION_TIME_S,
    Math.max(0.05, stopDecelerationTime(profile)),
  );
  const next: Record<string, unknown> = {
    ...profile,
    profile_id:
      typeof profile.profile_id === 'string' && profile.profile_id.length > 0
        ? profile.profile_id
        : profileIdForSlot(slot, mode),
    stop_mode: mode,
    deceleration_time_s: decelerationTime,
    require_backend_confirmation:
      typeof profile.require_backend_confirmation === 'boolean'
        ? profile.require_backend_confirmation
        : true,
  };
  delete next.allowed_fallback_modes;

  if (finitePositive(next.max_stop_time_s) != null) {
    next.max_stop_time_s = Math.max(decelerationTime, finitePositive(next.max_stop_time_s) ?? decelerationTime);
  }

  if (mode === 'soft_on_path' || mode === 'spring_back_hold') {
    const decelerationLimits = finitePositiveRecord(
      profile.per_joint_max_deceleration_rad_s2,
    );
    for (const jointName of jointNames) {
      if (decelerationLimits[jointName] == null) {
        decelerationLimits[jointName] =
          DEFAULT_SOFT_ON_PATH_DECELERATION_LIMIT_RAD_S2;
      }
    }
    next.per_joint_max_deceleration_rad_s2 = decelerationLimits;
  } else {
    delete next.per_joint_max_deceleration_rad_s2;
  }

  if (mode === 'soft_on_path') {
    next.allow_natural_completion_on_short_path =
      typeof profile.allow_natural_completion_on_short_path === 'boolean'
        ? profile.allow_natural_completion_on_short_path
        : true;
  } else {
    delete next.allow_natural_completion_on_short_path;
  }

  return next;
}

export function applyControlledStopProfileModeEdit(
  executionConfig: Record<string, unknown>,
  slot: ControlledStopProfileSlot,
  mode: string,
  jointNames: string[],
): Record<string, unknown> {
  if (mode.length === 0) {
    const next = { ...executionConfig };
    delete next[slot];
    return next;
  }
  const profile = {
    ...stopProfileFromConfig(executionConfig, slot),
    profile_id: profileIdForSlot(slot, mode),
  };
  return {
    ...executionConfig,
    [slot]: normalizeControlledStopProfileForMode(profile, slot, mode, jointNames),
  };
}

export function applyControlledStopNumberRecordEdit(
  record: Record<string, number>,
  joint: string,
  raw: string,
): Record<string, number> {
  const trimmed = raw.trim();
  const next = { ...record };
  if (trimmed === '') {
    delete next[joint];
    return next;
  }
  const parsed = Number(trimmed);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return record;
  }
  next[joint] = parsed;
  return next;
}

function clampStopDeceleration(raw: string, fallback: number): number {
  const parsed = Number(raw);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return fallback;
  }
  return Math.min(
    MAX_CONTROLLED_STOP_DECELERATION_TIME_S,
    Math.max(0.05, parsed),
  );
}

function ControlledStopNumberRecordEditor({
  label,
  joints,
  values,
  placeholder,
  disabled,
  onChange,
  help,
}: {
  label: string;
  joints: string[];
  values: Record<string, number>;
  placeholder: string;
  disabled: boolean;
  onChange: (next: Record<string, number>) => void;
  help?: string;
}) {
  const names = joints.length > 0
    ? joints
    : Object.keys(values).sort();
  if (names.length === 0) {
    return (
      <div className="rounded border border-gray-700 bg-gray-900/40 p-2 text-xs text-gray-500">
        No drivable joints loaded.
      </div>
    );
  }
  return (
    <div className="mb-2" title={help}>
      <div className="mb-1">
        <FieldLabel label={label} help={help} />
      </div>
      <div className="space-y-1">
        {names.map((joint) => (
          <div key={joint} className="grid grid-cols-[1fr_6rem] items-center gap-2">
            <span className="truncate font-mono text-[11px] text-gray-200">{joint}</span>
            <DraftTextInput
              value={values[joint] == null ? '' : String(values[joint])}
              onCommit={(raw) => {
                const next = applyControlledStopNumberRecordEdit(values, joint, raw);
                if (next !== values) onChange(next);
              }}
              placeholder={placeholder}
              disabled={disabled}
              className="w-full rounded bg-gray-800 px-1 py-0.5 text-right font-mono text-[10px] text-gray-200 disabled:opacity-50"
              inputMode="decimal"
            />
          </div>
        ))}
      </div>
    </div>
  );
}

function ControlledStopProfileEditor({
  label,
  slot,
  executionConfig,
  executionAdapterId,
  appliedMode,
  stopProfileOptions,
  drivableJointNames,
  disabled,
  onChangeExecutionConfig,
}: {
  label: string;
  slot: ControlledStopProfileSlot;
  executionConfig: Record<string, unknown>;
  executionAdapterId: string | null | undefined;
  appliedMode: string | null;
  stopProfileOptions: ReturnType<typeof buildStopProfileView>['options'];
  drivableJointNames: string[];
  disabled: boolean;
  onChangeExecutionConfig: (next: Record<string, unknown>) => void;
}) {
  const rawProfile = stopProfileFromConfig(executionConfig, slot);
  const mode = configuredStopModeOrNull(rawProfile);
  const profile = mode
    ? normalizeControlledStopProfileForMode(
        rawProfile,
        slot,
        mode,
        drivableJointNames,
      )
    : {};
  const optionByMode = new Map(stopProfileOptions.map((option) => [option.mode, option]));
  const modes = Array.from(new Set([
    ...KNOWN_STOP_MODES,
    mode ?? '',
    ...stopProfileOptions.map((option) => option.mode),
  ].filter((candidate) => candidate.length > 0)));
  const decelerationTime = stopDecelerationTime(profile);
  const maxStopTime = finitePositive(profile.max_stop_time_s);
  const settleVelocityThreshold = finitePositive(profile.settle_velocity_threshold_rad_s);
  const decelerationLimits = finitePositiveRecord(
    profile.per_joint_max_deceleration_rad_s2,
  );
  const maxStopTimePlaceholder = `auto ${formatControlNumber(
    controlledStopAutoMaxStopTimeS(
      executionConfig,
      executionAdapterId,
      decelerationTime,
    ),
  )}`;

  const updateProfile = (nextProfile: Record<string, unknown>) => {
    const fallbackMode =
      mode ?? (slot === OPERATOR_STOP_SLOT ? 'soft_on_path' : 'spring_back_hold');
    onChangeExecutionConfig({
      ...executionConfig,
      [slot]: normalizeControlledStopProfileForMode(
        nextProfile,
        slot,
        configuredStopMode(nextProfile, fallbackMode),
        drivableJointNames,
      ),
    });
  };

  return (
    <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
      <div className="mb-2 flex items-start justify-between gap-2">
        <div className="text-xs font-semibold uppercase tracking-wide text-gray-300">
          {label}
        </div>
        <div
          className={`rounded px-1.5 py-0.5 text-[10px] font-mono uppercase ${
            appliedMode === mode
              ? 'bg-emerald-950/70 text-emerald-300'
              : 'bg-amber-950/70 text-amber-300'
          }`}
          title="Currently applied backend stop mode for this slot"
        >
          Applied: {appliedMode ? stopModeLabel(appliedMode) : 'none'}
        </div>
      </div>
      {slot === OPERATOR_STOP_SLOT && appliedMode == null && (
        <div className="mb-2 rounded border border-amber-800 bg-amber-950/40 p-2 text-xs text-amber-100">
          The Stop button has no applied controlled-stop profile.
        </div>
      )}
      {appliedMode !== mode && (
        <div className="mb-2 rounded border border-blue-800 bg-blue-950/30 p-2 text-xs text-blue-100">
          Draft differs from applied runtime. Apply the deployment to activate this stop profile.
        </div>
      )}
      <div className="space-y-2">
        <label className="block mb-2" title={controlledStopFieldHelp('mode')}>
          <FieldLabel label="Mode" help={controlledStopFieldHelp('mode')} />
          <select
            value={mode ?? ''}
            onChange={(event) => {
              onChangeExecutionConfig(
                applyControlledStopProfileModeEdit(
                  executionConfig,
                  slot,
                  event.target.value,
                  drivableJointNames,
                ),
              );
            }}
            disabled={disabled}
            className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
          >
            <option value="" disabled>
              Not configured
            </option>
            {modes.map((candidate) => {
              const option = optionByMode.get(candidate);
              return (
                <option
                  key={candidate}
                  value={candidate}
                  disabled={option?.disabled ?? false}
                  title={option?.reason ?? undefined}
                >
                  {stopModeLabel(candidate)}
                  {option?.disabled ? ' (unsupported)' : ''}
                </option>
              );
            })}
          </select>
        </label>
        {mode && (
          <>
            <FieldInput
              label="Deceleration Time (s)"
              help={controlledStopFieldHelp('decelerationTime')}
              value={decelerationTime}
              type="number"
              min={0.05}
              step={0.05}
              disabled={disabled}
              onChange={(raw) => {
                const next = clampStopDeceleration(raw, decelerationTime);
                updateProfile({
                  ...profile,
                  deceleration_time_s: next,
                  max_stop_time_s:
                    maxStopTime != null && maxStopTime < next ? next : profile.max_stop_time_s,
                });
              }}
            />
            <FieldInput
              label="Max Stop Time (s)"
              help={controlledStopFieldHelp('maxStopTime')}
              value={maxStopTime ?? ''}
              placeholder={maxStopTime == null ? maxStopTimePlaceholder : undefined}
              type="number"
              min={0.05}
              step={0.05}
              disabled={disabled}
              onChange={(raw) => {
                const trimmed = raw.trim();
                const nextProfile = { ...profile };
                if (trimmed === '') {
                  delete nextProfile.max_stop_time_s;
                } else {
                  const parsed = Number(trimmed);
                  if (Number.isFinite(parsed) && parsed > 0) {
                    nextProfile.max_stop_time_s = Math.max(decelerationTime, parsed);
                  }
                }
                updateProfile(nextProfile);
              }}
            />
            <FieldInput
              label="Settle Velocity (rad/s)"
              help={controlledStopFieldHelp('settleVelocity')}
              value={settleVelocityThreshold ?? ''}
              placeholder={
                settleVelocityThreshold == null
                  ? `default ${formatControlNumber(DEFAULT_SETTLE_VELOCITY_THRESHOLD_RAD_S)}`
                  : undefined
              }
              type="number"
              min={0.0001}
              step={0.001}
              disabled={disabled}
              onChange={(raw) => {
                const trimmed = raw.trim();
                const nextProfile = { ...profile };
                if (trimmed === '') {
                  delete nextProfile.settle_velocity_threshold_rad_s;
                } else {
                  const parsed = Number(trimmed);
                  if (Number.isFinite(parsed) && parsed > 0) {
                    nextProfile.settle_velocity_threshold_rad_s = parsed;
                  }
                }
                updateProfile(nextProfile);
              }}
            />
          </>
        )}
      </div>
      {(mode === 'soft_on_path' || mode === 'spring_back_hold') && (
        <div className="mt-2 border-t border-gray-700/60 pt-2">
          {mode === 'soft_on_path' && (
            <CheckboxInput
              label="Allow Natural Completion"
              help={controlledStopFieldHelp('naturalCompletion')}
              checked={profile.allow_natural_completion_on_short_path !== false}
              onChange={(value) => updateProfile({
                ...profile,
                allow_natural_completion_on_short_path: value,
              })}
              disabled={disabled}
            />
          )}
          <ControlledStopNumberRecordEditor
            label="Max Deceleration (rad/s²)"
            help={controlledStopFieldHelp('maxDeceleration')}
            joints={drivableJointNames}
            values={decelerationLimits}
            placeholder={String(DEFAULT_SOFT_ON_PATH_DECELERATION_LIMIT_RAD_S2)}
            disabled={disabled}
            onChange={(next) => updateProfile({
              ...profile,
              per_joint_max_deceleration_rad_s2: next,
            })}
          />
        </div>
      )}
    </div>
  );
}

function ControlledStopEditor({
  executionConfig,
  executionAdapterId,
  stopProfileView,
  hardStopLevel,
  drivableJointNames,
  disabled,
  onChangeExecutionConfig,
}: {
  executionConfig: Record<string, unknown>;
  executionAdapterId: string | null | undefined;
  stopProfileView: ReturnType<typeof buildStopProfileView>;
  hardStopLevel: string | null | undefined;
  drivableJointNames: string[];
  disabled: boolean;
  onChangeExecutionConfig: (next: Record<string, unknown>) => void;
}) {
  const leadIn = finitePositive(executionConfig.soft_on_path_stop_traj_delta_s) ??
    DEFAULT_SOFT_ON_PATH_LEAD_IN_S;
  const samplePeriod = finitePositive(executionConfig.soft_on_path_sample_period_s) ??
    DEFAULT_SOFT_ON_PATH_SAMPLE_PERIOD_S;
  const jerkLimits = finitePositiveRecord(executionConfig.soft_on_path_max_jerk_rad_s3);
  const enablePathRamp = executionConfig.enable_soft_on_path_path_ramp !== false;
  const showRos2ControlCompletionTimeout =
    executionAdapterId === 'execution.ros2_control_follow_joint_trajectory';

  const updateEngineField = (field: string, value: unknown) => {
    onChangeExecutionConfig({
      ...executionConfig,
      [field]: value,
    });
  };

  const minProfileDeceleration = Math.min(
    ...CONTROLLED_STOP_PROFILE_SLOTS.map(({ slot }) =>
      stopDecelerationTime(stopProfileFromConfig(executionConfig, slot)),
    ),
  );
  const maxLeadIn = Math.max(0.001, minProfileDeceleration - 0.001);
  const maxRequestedStopTime =
    controlledStopMaxRequestedStopTimeS(executionConfig);
  const autoMaxStopTime = maxRequestedStopTime +
    controlledStopCompletionToleranceS(executionConfig, executionAdapterId);

  return (
    <div className="space-y-3">
      {stopProfileView.queryFailed && (
        <div className="rounded border border-red-800 bg-red-950/40 p-2 text-xs text-red-200">
          Stop capability query failed. Apply is still available, but Run remains blocked until the backend reports capability.
        </div>
      )}
      {stopProfileView.unsupported && !stopProfileView.queryFailed && (
        <div className="rounded border border-amber-800 bg-amber-950/40 p-2 text-xs text-amber-100">
          The loaded deployment contains a stop mode unsupported by the active backend.
        </div>
      )}
      <div
        className="rounded border border-gray-700 bg-gray-900/40 p-2 text-xs text-gray-300"
        title={
          'Read-only backend capability. Profiled hard deceleration means JTC ' +
          'decelerate-on-cancel is configured and proven. Immediate cancel/hold ' +
          'is an unprofiled hard-stop floor used only when timed/proven stops ' +
          'cannot be trusted.'
        }
      >
        <FieldLabel
          label="Hard-stop fallback floor"
          help={
            'Not an operator-selected profile. This is the final hard-stop floor ' +
            'ManyForge will use for refused or unconfirmed controlled stops.'
          }
          className="text-[11px] text-gray-500"
        />
        <div className="mt-0.5 font-mono text-[11px] text-gray-100">
          {hardStopLevelLabel(hardStopLevel)}
        </div>
      </div>
      <div className="space-y-3">
        {CONTROLLED_STOP_PROFILE_SLOTS.map(({ slot, label }) => (
          <ControlledStopProfileEditor
            key={slot}
            label={label}
            slot={slot}
            executionConfig={executionConfig}
            executionAdapterId={executionAdapterId}
            appliedMode={
              slot === OPERATOR_STOP_SLOT
                ? stopProfileView.operatorMode
                : stopProfileView.faultMode
            }
            stopProfileOptions={stopProfileView.options}
            drivableJointNames={drivableJointNames}
            disabled={disabled}
            onChangeExecutionConfig={onChangeExecutionConfig}
          />
        ))}
      </div>
      <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
        <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-300">
          Soft On-Path Engine
        </div>
        <div className="space-y-2">
          <FieldInput
            label="Lead-In (s)"
            help={controlledStopFieldHelp('leadIn')}
            value={leadIn}
            type="number"
            min={0.001}
            step={0.01}
            disabled={disabled}
            onChange={(raw) => {
              const parsed = Number(raw);
              if (Number.isFinite(parsed) && parsed > 0) {
                updateEngineField('soft_on_path_stop_traj_delta_s', Math.min(parsed, maxLeadIn));
              }
            }}
          />
          <FieldInput
            label="Sample Period (s)"
            help={controlledStopFieldHelp('samplePeriod')}
            value={samplePeriod}
            type="number"
            min={0.001}
            step={0.001}
            disabled={disabled}
            onChange={(raw) => {
              const parsed = Number(raw);
              if (Number.isFinite(parsed) && parsed > 0) {
                updateEngineField('soft_on_path_sample_period_s', parsed);
              }
            }}
          />
          {showRos2ControlCompletionTimeout && (
            <FieldInput
              label="Auto Max Stop Time (s)"
              help={controlledStopFieldHelp('stopCompletionTimeout')}
              value={formatControlNumber(autoMaxStopTime)}
              type="number"
              min={maxRequestedStopTime + 0.001}
              step={0.05}
              disabled={disabled}
              onChange={(raw) => {
                const next = applyControlledStopAutoDeadlineEdit(
                  executionConfig,
                  raw,
                );
                if (next !== executionConfig) {
                  onChangeExecutionConfig(next);
                }
              }}
            />
          )}
          <div className="flex items-end">
            <CheckboxInput
              label="Path Ramp"
              help={controlledStopFieldHelp('pathRamp')}
              checked={enablePathRamp}
              onChange={(value) => updateEngineField('enable_soft_on_path_path_ramp', value)}
              disabled={disabled}
            />
          </div>
        </div>
        <ControlledStopNumberRecordEditor
          label="Max Jerk (rad/s³)"
          help={controlledStopFieldHelp('maxJerk')}
          joints={drivableJointNames}
          values={jerkLimits}
          placeholder="derived"
          disabled={disabled}
          onChange={(next) => updateEngineField('soft_on_path_max_jerk_rad_s3', next)}
        />
      </div>
    </div>
  );
}

function PoseSourcesEditor({
  sources,
  onChange,
  disabled,
}: {
  sources: PoseSourceConfig[];
  onChange: (next: PoseSourceConfig[]) => void;
  disabled: boolean;
}) {
  const updateSource = useCallback((index: number, patch: Partial<PoseSourceConfig>) => {
    onChange(sources.map((source, i) => (i === index ? { ...source, ...patch } : source)));
  }, [onChange, sources]);

  const removeSource = useCallback((index: number) => {
    onChange(sources.filter((_, i) => i !== index));
  }, [onChange, sources]);

  const addSource = useCallback(() => {
    onChange([...sources, defaultPoseSource(nextPoseSourceId(sources))]);
  }, [onChange, sources]);

  const updateObject = useCallback((
    sourceIndex: number,
    objectId: string,
    patch: Partial<PoseSourceObjectBindingConfig>,
  ) => {
    const source = sources[sourceIndex];
    if (!source) return;
    updateSource(sourceIndex, {
      objects: {
        ...(source.objects ?? {}),
        [objectId]: {
          ...defaultPoseSourceObject(),
          ...(source.objects?.[objectId] ?? {}),
          ...patch,
        },
      },
    });
  }, [sources, updateSource]);

  const renameObject = useCallback((sourceIndex: number, currentId: string, nextIdRaw: string) => {
    const nextId = nextIdRaw.trim();
    const source = sources[sourceIndex];
    if (!source || !nextId || nextId === currentId || source.objects?.[nextId]) return;
    const nextObjects: Record<string, PoseSourceObjectBindingConfig> = {};
    for (const [objectId, binding] of Object.entries(source.objects ?? {})) {
      nextObjects[objectId === currentId ? nextId : objectId] = binding;
    }
    updateSource(sourceIndex, { objects: nextObjects });
  }, [sources, updateSource]);

  const addObject = useCallback((sourceIndex: number) => {
    const source = sources[sourceIndex];
    if (!source) return;
    const existing = new Set(Object.keys(source.objects ?? {}));
    let index = 1;
    while (existing.has(`scene_object_${index}`)) {
      index += 1;
    }
    updateSource(sourceIndex, {
      objects: {
        ...(source.objects ?? {}),
        [`scene_object_${index}`]: defaultPoseSourceObject(),
      },
    });
  }, [sources, updateSource]);

  const removeObject = useCallback((sourceIndex: number, objectId: string) => {
    const source = sources[sourceIndex];
    if (!source) return;
    const nextObjects = { ...(source.objects ?? {}) };
    delete nextObjects[objectId];
    updateSource(sourceIndex, { objects: nextObjects });
  }, [sources, updateSource]);

  return (
    <div className="space-y-2">
      <div className="rounded border border-blue-900/50 bg-blue-950/20 p-2 text-[11px] leading-relaxed text-blue-100/90">
        Pose sources continuously observe external object poses up to the configured
        max update rate. Apply policy controls when those observations may mutate
        ManyForge's planning scene: never during an active trajectory.
      </div>
      {sources.length === 0 && (
        <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
          No pose sources configured.
        </div>
      )}
      {sources.map((source, sourceIndex) => {
        const objects = source.objects ?? {};
        return (
          <div key={`${source.id}:${sourceIndex}`} className="rounded border border-gray-700 bg-gray-900/40 p-3">
            <div className="mb-2 flex items-center justify-between gap-2">
              <div className="min-w-0">
                <div className="truncate text-xs font-semibold uppercase tracking-wide text-gray-300">
                  {source.id || `pose source ${sourceIndex + 1}`}
                </div>
                <div className="text-[10px] text-gray-500">
                  Continuous observation at max {source.maxUpdateHz || 4} Hz; stale after {source.staleAfterS || 1}s.
                </div>
              </div>
              <button
                type="button"
                onClick={() => removeSource(sourceIndex)}
                disabled={disabled}
                className="rounded border border-red-900 px-2 py-1 text-[11px] text-red-200 hover:bg-red-950 disabled:cursor-not-allowed disabled:opacity-50"
              >
                Remove
              </button>
            </div>
            <div className="grid gap-2 md:grid-cols-2">
              <FieldInput
                label="Source ID"
                value={source.id}
                onChange={(value) => updateSource(sourceIndex, { id: value.trim() || source.id })}
                disabled={disabled}
              />
              <label className="block mb-2">
                <span className="text-xs text-gray-400">Kind</span>
                <select
                  value={source.kind}
                  onChange={(event) => updateSource(sourceIndex, { kind: event.target.value as PoseSourceKind })}
                  disabled={disabled}
                  className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
                >
                  {POSE_SOURCE_KINDS.map((kind) => (
                    <option key={kind} value={kind}>{kind}</option>
                  ))}
                </select>
              </label>
              <FieldInput
                label="Max Update Hz"
                value={source.maxUpdateHz}
                type="number"
                min={0.1}
                step={0.5}
                onChange={(value) => updateSource(sourceIndex, { maxUpdateHz: Math.max(0.1, Number(value) || 4) })}
                disabled={disabled}
              />
              <FieldInput
                label="Stale After (s)"
                value={source.staleAfterS}
                type="number"
                min={0.1}
                step={0.1}
                onChange={(value) => updateSource(sourceIndex, { staleAfterS: Math.max(0.1, Number(value) || 1) })}
                disabled={disabled}
              />
              <FieldInput
                label="Frame ID"
                value={source.frameId ?? ''}
                onChange={(value) => updateSource(sourceIndex, { frameId: value })}
                disabled={disabled}
              />
              <FieldInput
                label="Service Prefix"
                value={source.servicePrefix ?? 'auto'}
                onChange={(value) => updateSource(sourceIndex, { servicePrefix: value || 'auto' })}
                disabled={disabled}
                help="simulation_interfaces service namespace. Use auto for discovery."
              />
              <FieldInput
                label="Measured TCP Entity"
                value={source.measuredTcp?.entity ?? ''}
                onChange={(value) => updateSource(sourceIndex, {
                  measuredTcp: value.trim()
                    ? {
                        ...(source.measuredTcp ?? {}),
                        entity: value.trim(),
                        tcpFrameId: source.measuredTcp?.tcpFrameId || 'tool0',
                        frameId: source.measuredTcp?.frameId || source.frameId || '',
                      }
                    : null,
                })}
                disabled={disabled}
                help="Optional simulation entity for the robot TCP sampled with object poses. Required for measured grasp drift monitoring."
              />
              <FieldInput
                label="Measured TCP Frame"
                value={source.measuredTcp?.tcpFrameId ?? ''}
                onChange={(value) => updateSource(sourceIndex, {
                  measuredTcp: {
                    ...(source.measuredTcp ?? {}),
                    entity: source.measuredTcp?.entity ?? '',
                    tcpFrameId: value.trim(),
                    frameId: source.measuredTcp?.frameId || source.frameId || '',
                  },
                })}
                disabled={disabled || !source.measuredTcp}
                help="Frame name for the measured TCP sample, for example tool0."
              />
              <FieldInput
                label="Measured Robot Joint State Topic"
                value={source.measuredRobot?.jointStateTopic ?? ''}
                onChange={(value) => updateSource(sourceIndex, {
                  measuredRobot: value.trim()
                    ? {
                        jointStateTopic: value.trim(),
                      }
                    : null,
                })}
                disabled={disabled}
                help="Optional ROS JointState topic used to render the measured robot ghost overlay, for example /joint_states."
              />
              <FieldInput
                label="Topic"
                value={source.topic ?? ''}
                onChange={(value) => updateSource(sourceIndex, { topic: value })}
                disabled={disabled}
                help="Required for topic-based or TF source adapters."
              />
              <CheckboxInput
                label="Enabled"
                checked={source.enabled}
                onChange={(value) => updateSource(sourceIndex, { enabled: value })}
                disabled={disabled}
              />
            </div>
            <FieldInput
              label="Description"
              value={source.description ?? ''}
              onChange={(value) => updateSource(sourceIndex, { description: value })}
              disabled={disabled}
            />
            <div className="mt-2 space-y-2">
              <div className="flex items-center justify-between">
                <div className="text-[10px] font-semibold uppercase tracking-wide text-gray-500">Object bindings</div>
                <button
                  type="button"
                  onClick={() => addObject(sourceIndex)}
                  disabled={disabled}
                  className="rounded border border-gray-600 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
                >
                  Add Object
                </button>
              </div>
              {Object.keys(objects).length === 0 && (
                <div className="rounded border border-gray-700 bg-gray-950/50 px-2 py-1 text-[11px] text-gray-500">
                  Add one binding per ManyForge scene object that this source observes.
                </div>
              )}
              {Object.entries(objects).map(([objectId, binding]) => (
                <div key={objectId} className="rounded border border-gray-800 bg-gray-950/50 p-2">
                  <div className="grid gap-2 md:grid-cols-2">
                    <FieldInput
                      label="ManyForge Object ID"
                      value={objectId}
                      onChange={(value) => renameObject(sourceIndex, objectId, value)}
                      disabled={disabled}
                    />
                    <FieldInput
                      label="Source Entity"
                      value={binding.entity ?? ''}
                      onChange={(value) => updateObject(sourceIndex, objectId, { entity: value })}
                      disabled={disabled}
                      help="For Isaac simulation_interfaces, this is the USD/entity path."
                    />
                    <label className="block mb-2">
                      <span className="text-xs text-gray-400">Apply Policy</span>
                      <select
                        value={binding.applyPolicy ?? 'observe_only'}
                        onChange={(event) => updateObject(sourceIndex, objectId, { applyPolicy: event.target.value as PoseSourceApplyPolicy })}
                        disabled={disabled}
                        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
                      >
                        {POSE_SOURCE_APPLY_POLICIES.map((policy) => (
                          <option key={policy} value={policy}>{policy}</option>
                        ))}
                      </select>
                    </label>
                    <FieldInput
                      label="Object Frame ID"
                      value={binding.frameId ?? ''}
                      onChange={(value) => updateObject(sourceIndex, objectId, { frameId: value })}
                      disabled={disabled}
                    />
                    <FieldInput
                      label="Min Confidence"
                      value={binding.confidenceMin ?? ''}
                      type="number"
                      min={0}
                      step={0.05}
                      onChange={(value) => {
                        const trimmed = value.trim();
                        updateObject(sourceIndex, objectId, {
                          confidenceMin: trimmed === '' ? null : Math.min(1, Math.max(0, Number(trimmed) || 0)),
                        });
                      }}
                      disabled={disabled}
                      help="Optional threshold for perception-driven sources."
                    />
                    <div className="md:col-span-2 rounded border border-cyan-900/60 bg-cyan-950/10 p-2">
                      <div className="mb-2 text-[10px] font-semibold uppercase tracking-wide text-cyan-200">
                        Measured Grasp Integrity
                      </div>
                      <div className="grid gap-2 md:grid-cols-2">
                        <CheckboxInput
                          label="Measured-state ghosts"
                          checked={Boolean(binding.measuredStateGhost?.enabled)}
                          onChange={(value) => updateObject(sourceIndex, objectId, {
                            measuredStateGhost: {
                              ...defaultPoseSourceObject().measuredStateGhost,
                              ...(binding.measuredStateGhost ?? {}),
                              enabled: value,
                            },
                          })}
                          disabled={disabled}
                          help="Shows the measured robot joint-state overlay when configured, plus external object samples and the measured TCP marker. These are diagnostics; Composer's planning scene remains authoritative."
                        />
                        <CheckboxInput
                          label="Drift monitor enabled"
                          checked={Boolean(binding.attachedDriftMonitor?.enabled)}
                          onChange={(value) => updateObject(sourceIndex, objectId, {
                            attachedDriftMonitor: {
                              ...defaultPoseSourceObject().attachedDriftMonitor,
                              ...(binding.attachedDriftMonitor ?? {}),
                              enabled: value,
                              signalId: binding.attachedDriftMonitor?.signalId || `attached_object_drift/${objectId}`,
                            },
                          })}
                          disabled={disabled}
                          help="When enabled, same-time measured TCP/object samples arm a baseline at grasp and detect object slip."
                        />
                        <FieldInput
                          label="Translation threshold (m)"
                          value={binding.attachedDriftMonitor?.translationThresholdM ?? ''}
                          type="number"
                          min={0.001}
                          step={0.001}
                          onChange={(value) => {
                            const trimmed = value.trim();
                            updateObject(sourceIndex, objectId, {
                              attachedDriftMonitor: {
                                ...defaultPoseSourceObject().attachedDriftMonitor,
                                ...(binding.attachedDriftMonitor ?? {}),
                                translationThresholdM: trimmed === '' ? null : Math.max(0.001, Number(trimmed) || 0.02),
                                signalId: binding.attachedDriftMonitor?.signalId || `attached_object_drift/${objectId}`,
                              },
                            });
                          }}
                          disabled={disabled}
                          help="Object slip relative to measured TCP above this distance raises the configured action after debounce."
                        />
                        <FieldInput
                          label="Rotation threshold (rad)"
                          value={binding.attachedDriftMonitor?.rotationThresholdRad ?? ''}
                          type="number"
                          min={0.001}
                          step={0.01}
                          onChange={(value) => {
                            const trimmed = value.trim();
                            updateObject(sourceIndex, objectId, {
                              attachedDriftMonitor: {
                                ...defaultPoseSourceObject().attachedDriftMonitor,
                                ...(binding.attachedDriftMonitor ?? {}),
                                rotationThresholdRad: trimmed === '' ? null : Math.max(0.001, Number(trimmed) || 0.2),
                                signalId: binding.attachedDriftMonitor?.signalId || `attached_object_drift/${objectId}`,
                              },
                            });
                          }}
                          disabled={disabled}
                          help="Angular slip relative to measured TCP above this angle raises the configured action after debounce."
                        />
                        <FieldInput
                          label="Debounce (s)"
                          value={binding.attachedDriftMonitor?.debounceS ?? 0.1}
                          type="number"
                          min={0.0}
                          step={0.01}
                          onChange={(value) => updateObject(sourceIndex, objectId, {
                            attachedDriftMonitor: {
                              ...defaultPoseSourceObject().attachedDriftMonitor,
                              ...(binding.attachedDriftMonitor ?? {}),
                              debounceS: Math.max(0.001, Number(value) || 0.1),
                              signalId: binding.attachedDriftMonitor?.signalId || `attached_object_drift/${objectId}`,
                            },
                          })}
                          disabled={disabled}
                          help="Threshold must remain exceeded for this time before the condition is raised."
                        />
                        <label className="block mb-2">
                          <span className="text-xs text-gray-400">Action</span>
                          <select
                            value={binding.attachedDriftMonitor?.action ?? 'interrupt'}
                            onChange={(event) => updateObject(sourceIndex, objectId, {
                              attachedDriftMonitor: {
                                ...defaultPoseSourceObject().attachedDriftMonitor,
                                ...(binding.attachedDriftMonitor ?? {}),
                                action: event.target.value,
                                signalId: binding.attachedDriftMonitor?.signalId || `attached_object_drift/${objectId}`,
                              },
                            })}
                            disabled={disabled}
                            className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
                            title="interrupt stops the cycle when drift is confirmed; diagnostic_only uses the same monitor but does not stop execution."
                          >
                            <option value="interrupt">Interrupt</option>
                            <option value="diagnostic_only">Diagnostic only</option>
                          </select>
                        </label>
                      </div>
                    </div>
                    <div className="flex items-end">
                      <button
                        type="button"
                        onClick={() => removeObject(sourceIndex, objectId)}
                        disabled={disabled}
                        className="w-full rounded border border-red-900 px-2 py-1 text-[11px] text-red-200 hover:bg-red-950 disabled:cursor-not-allowed disabled:opacity-50"
                      >
                        Remove Binding
                      </button>
                    </div>
                  </div>
                </div>
              ))}
            </div>
          </div>
        );
      })}
      <button
        type="button"
        onClick={addSource}
        disabled={disabled}
        className="w-full rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
      >
        Add Pose Source
      </button>
    </div>
  );
}

function nextEndpointAlias(existingAliases: string[]): string {
  let index = existingAliases.length + 1;
  while (existingAliases.includes(`model_${index}`)) {
    index += 1;
  }
  return `model_${index}`;
}

// Wrapped in React.memo at the bottom of the function definition so
// editing one endpoint's fields doesn't re-render every sibling card.
// With the draft-input pattern, the parent panel only commits on
// blur; combined with memoized children, a single field commit
// re-renders only the card that owns it. Callers should pass stable
// `onUpdate` callbacks (useCallback) or accept that updates will
// trigger card re-renders.
function ModelEndpointCardImpl({
  alias,
  endpoint,
  availableModelIds,
  probeStatus,
  probeError,
  disabled,
  onAliasChange,
  onFieldChange,
  onRemove,
}: {
  alias: string;
  endpoint: ModelEndpointConfig;
  availableModelIds: string[];
  probeStatus?: string;
  probeError?: string | null;
  disabled: boolean;
  onAliasChange: (nextAlias: string) => void;
  onFieldChange: <K extends keyof ModelEndpointConfig>(key: K, value: ModelEndpointConfig[K]) => void;
  onRemove: () => void;
}) {
  const capabilities = modelEndpointCapabilities(endpoint);
  const updateCapabilities = (
    nextCapabilities: Partial<ModelEndpointCapabilities>,
  ) => {
    onFieldChange('capabilities', {
      ...capabilities,
      ...nextCapabilities,
    });
  };

  return (
    <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
      <div className="mb-2 flex items-center justify-between">
        <div className="text-xs font-medium uppercase tracking-wide text-gray-400">Endpoint Alias</div>
        <button
          type="button"
          onClick={onRemove}
          disabled={disabled}
          className="rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Remove
        </button>
      </div>
      <FieldInput
        label="Alias"
        value={alias}
        onChange={onAliasChange}
        disabled={disabled}
      />
      <FieldInput
        label="Profile Ref"
        value={endpoint.profileRef ?? ''}
        onChange={(value) => onFieldChange('profileRef', value)}
        disabled={disabled}
      />
      <FieldInput
        label="Provider"
        value={endpoint.provider}
        onChange={(value) => onFieldChange('provider', value)}
        disabled={disabled}
      />
      <FieldInput
        label="Configured Model ID"
        value={endpoint.modelId}
        onChange={(value) => onFieldChange('modelId', value)}
        disabled={disabled}
      />
      {availableModelIds.length > 0 && (
        <label className="block mb-2">
          <span className="text-xs text-gray-400">Advertised models</span>
          <select
            value={availableModelIds.includes(endpoint.modelId) ? endpoint.modelId : ''}
            onChange={(e) => {
              if (e.target.value) {
                onFieldChange('modelId', e.target.value);
              }
            }}
            disabled={disabled}
            className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
          >
            <option value="">Select advertised model</option>
            {availableModelIds.map((modelId) => (
              <option key={modelId} value={modelId}>
                {modelId}
              </option>
            ))}
          </select>
        </label>
      )}
      <FieldInput
        label="Base URL"
        value={endpoint.baseUrl ?? ''}
        onChange={(value) => onFieldChange('baseUrl', value)}
        disabled={disabled}
      />
      <FieldInput
        label="Image Fetch Base URL"
        value={endpoint.imageFetchBaseUrl ?? ''}
        onChange={(value) => onFieldChange('imageFetchBaseUrl', value)}
        disabled={disabled}
      />
      <FieldInput
        label="API Key Env"
        value={endpoint.apiKeyEnv ?? ''}
        onChange={(value) => onFieldChange('apiKeyEnv', value)}
        disabled={disabled}
      />
      <FieldInput
        label="Default Timeout (s)"
        value={endpoint.defaultTimeoutS}
        onChange={(value) => onFieldChange('defaultTimeoutS', Number(value) || 0)}
        type="number"
        disabled={disabled}
      />
      <FieldInput
        label="Max Tokens"
        value={endpoint.maxTokens}
        onChange={(value) => onFieldChange('maxTokens', Math.max(0, Math.trunc(Number(value) || 0)))}
        type="number"
        disabled={disabled}
      />
      <div className="mt-3 border-t border-gray-700 pt-3">
        <div className="mb-2 text-xs font-medium uppercase tracking-wide text-gray-400">
          Declared Capabilities
        </div>
        <CheckboxInput
          label="Response tools"
          checked={capabilities.supportsResponseTools}
          onChange={(value) => updateCapabilities({ supportsResponseTools: value })}
          disabled={disabled}
        />
        <CheckboxInput
          label="JSON object output"
          checked={capabilities.supportsJsonObject}
          onChange={(value) => updateCapabilities({ supportsJsonObject: value })}
          disabled={disabled}
        />
        <CheckboxInput
          label="Image input"
          checked={capabilities.supportsImageInput}
          onChange={(value) => updateCapabilities({ supportsImageInput: value })}
          disabled={disabled}
        />
        <StringListInput
          label="Image Transports"
          values={capabilities.supportedImageTransports}
          onChange={(values) => updateCapabilities({ supportedImageTransports: values })}
          disabled={disabled}
        />
        <FieldInput
          label="Tool Call Family"
          value={capabilities.toolCallFamily}
          onChange={(value) => updateCapabilities({ toolCallFamily: value })}
          disabled={disabled}
        />
        <FieldInput
          label="Latency Budget (ms)"
          value={capabilities.latencyBudgetMs ?? ''}
          onChange={(value) => updateCapabilities({
            latencyBudgetMs: value ? Math.max(1, Math.trunc(Number(value) || 0)) : null,
          })}
          type="number"
          disabled={disabled}
        />
      </div>
      <div className="mt-1 text-[11px] text-gray-500">
        Live probe: {probeStatus ?? 'not loaded'}
      </div>
      {probeError && (
        <div className="mt-1 text-[11px] text-red-400">{probeError}</div>
      )}
    </div>
  );
}

const ModelEndpointCard = memo(ModelEndpointCardImpl);

export function DeploymentPanel() {
  // Tightened selector via useShallow: previously this was
  // `useDeploymentStore()` with no selector at all, which subscribed
  // the panel to every field of the store and re-rendered on any
  // unrelated change (e.g., another component editing a different
  // slice). useShallow does a shallow comparison on the returned
  // object's keys, so the panel now only re-renders when one of
  // these specific seven fields changes. Action references
  // (saveDeployment / setField / setNestedField) are stable across
  // renders so they don't trigger pointless re-renders.
  const {
    deployment,
    appliedDeployment,
    savedDeployment,
    sourcePath,
    saveDeployment,
    setField,
    setNestedField,
  } = useDeploymentStore(
    useShallow((state) => ({
      deployment: state.deployment,
      appliedDeployment: state.appliedDeployment,
      savedDeployment: state.savedDeployment,
      sourcePath: state.sourcePath,
      saveDeployment: state.saveDeployment,
      setField: state.setField,
      setNestedField: state.setNestedField,
    })),
  );
  const editLocked = useEditLocked();
  useStatusPolling();
  const runtime = useStatusStore((state) => state.runtime);
  const stopProfileView = buildStopProfileView(runtime);
  const [modelEndpointCatalog, setModelEndpointCatalog] = useState<ModelEndpointCatalog | null>(null);
  const [sceneFrames, setSceneFrames] = useState<SceneFrameState[]>([]);
  const [urdfManifest, setUrdfManifest] = useState<SceneUrdfManifest | null>(null);
  useEffect(() => {
    let cancelled = false;
    void api.urdf.manifest()
      .then((m) => {
        if (!cancelled) setUrdfManifest(m);
      })
      .catch(() => {
        if (!cancelled) setUrdfManifest(null);
      });
    return () => {
      cancelled = true;
    };
  }, [deployment?.sourcePath]);
  const drivableJointNames = useMemo<string[]>(() => {
    if (!urdfManifest?.joints) return [];
    const DRIVABLE = new Set(['revolute', 'prismatic', 'continuous']);
    return urdfManifest.joints.filter((j) => DRIVABLE.has(j.type)).map((j) => j.name);
  }, [urdfManifest]);
  const runtimeApplied = signature(deployment) === signature(appliedDeployment);
  const diskSaved = signature(deployment) === signature(savedDeployment);
  const endpointAliases = useMemo(
    () => Object.keys(deployment?.modelEndpoints ?? {}).sort(),
    [deployment?.modelEndpoints],
  );
  const warnings = useMemo(
    () => (deployment ? deploymentWarnings(deployment) : []),
    [deployment],
  );
  const executionConfig = deployment?.adapters.execution?.config ?? {};

  useEffect(() => {
    if (!deployment || Object.keys(deployment.modelEndpoints ?? {}).length === 0) {
      setModelEndpointCatalog(null);
      return;
    }
    let cancelled = false;
    void api.infra.modelEndpoints()
      .then((result) => {
        if (!cancelled) {
          setModelEndpointCatalog(result);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setModelEndpointCatalog(null);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [deployment]);

  useEffect(() => {
    let cancelled = false;
    void api.scene.frames()
      .then((result) => {
        if (!cancelled) {
          setSceneFrames(result.frames);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setSceneFrames([]);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [deployment?.sourcePath, deployment?.robotConfig?.urdfPath]);

  const frameOptions = useMemo(
    () => sceneFrames.map((frame) => frame.name).filter((frameName) => frameName !== 'universe'),
    [sceneFrames],
  );

  const setModelEndpoints = useCallback((nextModelEndpoints: Record<string, ModelEndpointConfig>) => {
    setField('modelEndpoints', nextModelEndpoints);
  }, [setField]);

  const setExecutionConfig = useCallback((nextConfig: Record<string, unknown>) => {
    if (!deployment?.adapters.execution) {
      return;
    }
    setNestedField('adapters.execution.config', nextConfig);
  }, [deployment?.adapters.execution, setNestedField]);

  const updateModelEndpointAlias = useCallback((currentAlias: string, nextAliasRaw: string) => {
    if (!deployment) {
      return;
    }
    const nextAlias = nextAliasRaw.trim();
    if (!nextAlias || nextAlias === currentAlias) {
      return;
    }
    if (Object.prototype.hasOwnProperty.call(deployment.modelEndpoints ?? {}, nextAlias)) {
      return;
    }
    const nextModelEndpoints: Record<string, ModelEndpointConfig> = {};
    for (const [alias, endpoint] of Object.entries(deployment.modelEndpoints ?? {})) {
      if (alias === currentAlias) {
        nextModelEndpoints[nextAlias] = endpoint;
      } else {
        nextModelEndpoints[alias] = endpoint;
      }
    }
    setModelEndpoints(nextModelEndpoints);
  }, [deployment, setModelEndpoints]);

  const updateModelEndpointField = useCallback(<K extends keyof ModelEndpointConfig>(
    alias: string,
    key: K,
    value: ModelEndpointConfig[K],
  ) => {
    if (!deployment) {
      return;
    }
    const currentEndpoint = deployment.modelEndpoints?.[alias];
    if (!currentEndpoint) {
      return;
    }
    setModelEndpoints({
      ...deployment.modelEndpoints,
      [alias]: {
        ...currentEndpoint,
        [key]: value,
      },
    });
  }, [deployment, setModelEndpoints]);

  const removeModelEndpoint = useCallback((alias: string) => {
    if (!deployment) {
      return;
    }
    const nextModelEndpoints = { ...deployment.modelEndpoints };
    delete nextModelEndpoints[alias];
    setModelEndpoints(nextModelEndpoints);
  }, [deployment, setModelEndpoints]);

  const addModelEndpoint = useCallback(() => {
    if (!deployment) {
      return;
    }
    const aliases = Object.keys(deployment.modelEndpoints ?? {});
    const alias = nextEndpointAlias(aliases);
    setModelEndpoints({
      ...deployment.modelEndpoints,
      [alias]: {
        profileRef: '',
        provider: 'openai',
        modelId: '',
        baseUrl: '',
        imageFetchBaseUrl: '',
        apiKeyEnv: '',
        defaultTimeoutS: 30,
        maxTokens: 4096,
        capabilities: { ...DEFAULT_MODEL_ENDPOINT_CAPABILITIES },
      },
    });
  }, [deployment, setModelEndpoints]);

  const updateAssistantField = useCallback(<K extends keyof AssistantConfig>(
    key: K,
    value: AssistantConfig[K],
  ) => {
    if (!deployment) {
      return;
    }
    setField('assistant', {
      provider: 'deterministic-scene-authoring',
      endpointAlias: '',
      enabled: true,
      ...(deployment.assistant ?? {}),
      [key]: value,
    });
  }, [deployment, setField]);

  const enableAssistantConfig = useCallback(() => {
    setField('assistant', {
      provider: 'deterministic-scene-authoring',
      endpointAlias: '',
      enabled: true,
    });
  }, [setField]);

  if (!deployment) {
    return (
      <div className="p-4 text-gray-500 text-sm">
        No deployment artifact loaded into Composer.
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-y-auto p-3 space-y-4">
      <div className="rounded border border-gray-700 bg-gray-900/60 p-3 text-xs text-gray-300 space-y-1">
        <div>
          <span className="text-gray-500">Loaded from:</span>{' '}
          <span className="font-mono">{sourcePath || '<unspecified>'}</span>
        </div>
        <div className="flex gap-2 pt-1">
          <span className={`rounded px-2 py-1 font-mono ${runtimeApplied ? 'bg-emerald-700 text-white' : 'bg-orange-700 text-white'}`}>
            {runtimeApplied ? 'APPLIED' : 'UNAPPLIED'}
          </span>
          <span className={`rounded px-2 py-1 font-mono ${diskSaved ? 'bg-slate-700 text-white' : 'bg-amber-700 text-white'}`}>
            {diskSaved ? 'SAVED' : 'UNSAVED'}
          </span>
          {editLocked && (
            <span className="rounded bg-yellow-800 px-2 py-1 font-mono text-yellow-100">
              READ ONLY WHILE RUNNING
            </span>
          )}
        </div>
      </div>

      {warnings.length > 0 && (
        <div className="rounded border border-amber-800 bg-amber-950/40 p-3 text-xs text-amber-100 space-y-1">
          {warnings.map((warning) => (
            <div key={warning}>{warning}</div>
          ))}
        </div>
      )}

      <Section title="General">
        <FieldInput
          label="Name"
          value={deployment.name}
          onChange={(v) => setField('name', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="Description"
          value={deployment.description}
          onChange={(v) => setField('description', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="Program Profile"
          value={deployment.programProfile}
          onChange={(v) => setField('programProfile', v)}
          disabled={editLocked}
        />
      </Section>

      <Section title="Robot Config">
        <FieldInput
          label="Robot ID"
          value={deployment.robotConfig.robotId}
          onChange={(v) => setNestedField('robotConfig.robotId', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="Group ID"
          value={deployment.robotConfig.groupId}
          onChange={(v) => setNestedField('robotConfig.groupId', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="URDF Path"
          value={deployment.robotConfig.urdfPath}
          onChange={(v) => setNestedField('robotConfig.urdfPath', v)}
          disabled={editLocked}
        />
        <StringListInput
          label="Package Paths"
          values={deployment.robotConfig.packagePaths}
          onChange={(values) => setNestedField('robotConfig.packagePaths', values)}
          disabled={editLocked}
        />
        <FieldInput
          label="Global Max TCP Linear Speed (m/s)"
          value={deployment.robotConfig.globalMaxTcpLinearSpeedMps}
          onChange={(v) =>
            setNestedField('robotConfig.globalMaxTcpLinearSpeedMps', parseFloat(v) || 0)
          }
          type="number"
          min={0}
          step={0.05}
          disabled={editLocked}
          help={
            'Deployment-wide cap on TCP (end-effector) Cartesian speed in m/s. ' +
            'Required and MUST be > 0 — a value of 0 or less is rejected at ' +
            'load. Enforced for ALL moves: planned trajectories clamp ' +
            'max_tcp_linear_speed_mps against this (min wins); validated ' +
            'Robot Control targets use it to bound the authored motion so ' +
            'the robot tracks at ≤ this speed regardless of operator input. ' +
            'See ADR-0015.'
          }
        />
      </Section>

      <Section title="Robot Control Joint Limits">
        <RobotControlJointLimitsEditor
          drivableJointNames={drivableJointNames}
          limits={deployment.robotConfig.robotControlJointLimits ?? {}}
          onChange={(next) => setNestedField('robotConfig.robotControlJointLimits', next)}
          disabled={editLocked}
        />
      </Section>

      <Section title="Controlled Stop">
        {deployment.adapters.execution ? (
          <ControlledStopEditor
            executionConfig={executionConfig}
            executionAdapterId={deployment.adapters.execution.adapterId}
            stopProfileView={stopProfileView}
            hardStopLevel={runtime?.hardStopLevel}
            drivableJointNames={drivableJointNames}
            disabled={editLocked}
            onChangeExecutionConfig={setExecutionConfig}
          />
        ) : (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            No execution adapter configured.
          </div>
        )}
      </Section>

      <Section title="Object Pose Sources">
        <PoseSourcesEditor
          sources={deployment.poseSources ?? []}
          onChange={(next) => setField('poseSources', next)}
          disabled={editLocked}
        />
      </Section>

      {Object.entries(deployment.grippers ?? {}).map(([gripperId, gripper]) => (
        <Section key={gripperId} title={`Gripper: ${gripperId}`}>
          <FieldInput
            label="Description"
            value={gripper.description}
            onChange={(value) => setNestedField(`grippers.${gripperId}.description`, value)}
            disabled={editLocked}
          />
          <FieldInput
            label="Command Mode"
            value={gripper.commandMode}
            onChange={(value) => setNestedField(`grippers.${gripperId}.commandMode`, value)}
            disabled={editLocked}
          />
          <FieldInput
            label="Group ID"
            value={gripper.groupId}
            onChange={(value) => setNestedField(`grippers.${gripperId}.groupId`, value)}
            disabled={editLocked}
          />
          <FieldInput
            label="Joint Name"
            value={gripper.jointName}
            onChange={(value) => setNestedField(`grippers.${gripperId}.jointName`, value)}
            disabled={editLocked}
          />
          <FieldInput
            label="Motion Time (s)"
            value={gripper.motionTimeS}
            onChange={(value) => setNestedField(`grippers.${gripperId}.motionTimeS`, Number(value) || 0)}
            type="number"
            disabled={editLocked}
          />
          <div>
            <label className="block text-xs font-semibold text-gray-400 mb-1">
              SRDF Presets
            </label>
            <div className="rounded border border-gray-700 bg-gray-900/60 px-2 py-1 text-xs text-gray-300">
              {Object.entries(gripper.srdfPresets ?? {}).length > 0 ? (
                Object.entries(gripper.srdfPresets)
                  .sort(([left], [right]) => left.localeCompare(right))
                  .map(([name, value]) => (
                    <div key={name} className="flex justify-between gap-3">
                      <span>{name}</span>
                      <span className="font-mono">{value}</span>
                    </div>
                  ))
              ) : (
                <span className="text-gray-500">No group_state presets found for this gripper.</span>
              )}
            </div>
          </div>
          <StringListInput
            label="Touch Links"
            values={gripper.touchLinks}
            onChange={(values) => setNestedField(`grippers.${gripperId}.touchLinks`, values)}
            disabled={editLocked}
          />
          <FrameSelectInput
            label="Mount Frame"
            value={gripper.mountFrame}
            options={frameOptions}
            onChange={(value) => setNestedField(`grippers.${gripperId}.mountFrame`, value)}
            disabled={editLocked}
          />
          <FrameSelectInput
            label="Attach Frame"
            value={gripper.attachFrame}
            options={frameOptions}
            onChange={(value) => setNestedField(`grippers.${gripperId}.attachFrame`, value)}
            disabled={editLocked}
          />
          <FrameSelectInput
            label="TCP Frame"
            value={gripper.tcpFrame}
            options={frameOptions}
            onChange={(value) => setNestedField(`grippers.${gripperId}.tcpFrame`, value)}
            disabled={editLocked}
          />
          <FrameMultiSelect
            label="Auxiliary Frames"
            options={frameOptions}
            selected={gripper.auxiliaryFrames}
            onChange={(values) => setNestedField(`grippers.${gripperId}.auxiliaryFrames`, values)}
            disabled={editLocked}
          />
        </Section>
      ))}

      {Object.entries(deployment.adapters).map(([slot, adapter]) => (
        <Section key={slot} title={`Adapter: ${slot}`}>
          <FieldInput
            label="Adapter ID"
            value={adapter.adapterId}
            onChange={(v) => setNestedField(`adapters.${slot}.adapterId`, v)}
            disabled={editLocked}
          />
          <FieldInput
            label="Version"
            value={adapter.adapterVersion}
            onChange={(v) => setNestedField(`adapters.${slot}.adapterVersion`, v)}
            disabled={editLocked}
          />
          <ConfigTextarea
            label="Config"
            value={adapter.config}
            onChange={(v) => setNestedField(`adapters.${slot}.config`, v)}
            disabled={editLocked}
          />
        </Section>
      ))}

      <Section title="Orchestrator">
        <FieldInput
          label="Engine"
          value={deployment.orchestrator.engine}
          onChange={(v) => setNestedField('orchestrator.engine', v)}
          disabled={editLocked}
        />
        <ConfigTextarea
          label="Config"
          value={deployment.orchestrator.config}
          onChange={(v) => setNestedField('orchestrator.config', v)}
          disabled={editLocked}
        />
      </Section>

      <Section title="Launch">
        <FieldInput
          label="Runner Module"
          value={deployment.launch.runner.module}
          onChange={(v) => setNestedField('launch.runner.module', v)}
          disabled={editLocked}
        />
        {deployment.launch.uiBackend && (
          <>
            <FieldInput
              label="UI Backend Module"
              value={deployment.launch.uiBackend.module}
              onChange={(v) => setNestedField('launch.uiBackend.module', v)}
              disabled={editLocked}
            />
          </>
        )}
      </Section>

      <Section title="Model Endpoints">
        <div className="mb-2 text-xs text-gray-500">
          Deployment aliases are what nodes reference. They should describe the task role, not the raw model family.
        </div>
        <FieldInput
          label="Profile Registry"
          value={deployment.modelEndpointProfileRegistry ?? ''}
          onChange={(value) => setField('modelEndpointProfileRegistry', value || null)}
          disabled={editLocked}
        />
        {Object.entries(deployment.modelEndpoints ?? {}).length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            No model endpoints configured.
          </div>
        )}
        {Object.entries(deployment.modelEndpoints ?? {}).map(([alias, endpoint]) => {
          const probe = modelEndpointCatalog?.endpoints?.[alias];
          return (
            <ModelEndpointCard
              key={alias}
              alias={alias}
              endpoint={endpoint}
              availableModelIds={probe?.availableModelIds ?? []}
              probeStatus={probe?.probeStatus}
              probeError={probe?.error}
              disabled={editLocked}
              onAliasChange={(nextAlias) => updateModelEndpointAlias(alias, nextAlias)}
              onFieldChange={(key, value) => updateModelEndpointField(alias, key, value)}
              onRemove={() => removeModelEndpoint(alias)}
            />
          );
        })}
        <button
          type="button"
          onClick={addModelEndpoint}
          disabled={editLocked}
          className="w-full rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Add Model Endpoint Alias
        </button>
      </Section>

      <Section title="Assistant">
        {!deployment.assistant ? (
          <button
            type="button"
            onClick={enableAssistantConfig}
            disabled={editLocked}
            className="w-full rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
          >
            Add Assistant Config
          </button>
        ) : (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
            <CheckboxInput
              label="Enabled"
              checked={deployment.assistant.enabled}
              onChange={(value) => updateAssistantField('enabled', value)}
              disabled={editLocked}
            />
            <label className="block mb-2">
              <span className="text-xs text-gray-400">Provider</span>
              <select
                value={deployment.assistant.provider}
                onChange={(event) => updateAssistantField('provider', event.target.value)}
                disabled={editLocked}
                className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
              >
                <option value="mock">mock</option>
                <option value="deterministic-scene-authoring">deterministic-scene-authoring</option>
                <option value="nemoclaw">nemoclaw</option>
                <option value="openclaw">openclaw</option>
              </select>
            </label>
            <label className="block mb-2">
              <span className="text-xs text-gray-400">Endpoint Alias</span>
              <select
                value={deployment.assistant.endpointAlias}
                onChange={(event) => updateAssistantField('endpointAlias', event.target.value)}
                disabled={editLocked || endpointAliases.length === 0}
                className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
              >
                <option value="">No endpoint</option>
                {endpointAliases.map((alias) => (
                  <option key={alias} value={alias}>
                    {alias}
                  </option>
                ))}
              </select>
            </label>
          </div>
        )}
      </Section>

      <button
        onClick={() => sourcePath && saveDeployment(sourcePath)}
        disabled={diskSaved || editLocked || !sourcePath}
        className="w-full px-3 py-2 bg-amber-600 hover:bg-amber-500 disabled:bg-gray-600 disabled:cursor-not-allowed text-white text-sm font-medium rounded"
      >
        Save Deployment Artifact
      </button>
    </div>
  );
}
