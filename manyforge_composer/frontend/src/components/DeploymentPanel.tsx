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
  min,
  step,
}: {
  label: string;
  value: string | number;
  onChange: (v: string) => void;
  type?: string;
  disabled?: boolean;
  help?: string;
  min?: number;
  step?: number;
}) {
  return (
    <label className="block mb-2" title={help}>
      <span className="text-xs text-gray-400">
        {label}
        {help !== undefined && (
          <span
            className="ml-1 cursor-help text-gray-500"
            aria-label={help}
          >
            ⓘ
          </span>
        )}
      </span>
      <DraftTextInput
        type={type}
        value={value}
        onCommit={onChange}
        disabled={disabled}
        min={min}
        step={step}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
      />
    </label>
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
}: {
  label: string;
  values: string[];
  onChange: (values: string[]) => void;
  disabled?: boolean;
}) {
  return (
    <label className="block mb-2">
      <span className="text-xs text-gray-400">{label}</span>
      <DraftTextInput
        type="text"
        value={values.join(', ')}
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
}: {
  label: string;
  checked: boolean;
  onChange: (value: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <label className="mb-2 flex items-center gap-2 text-sm text-gray-200">
      <input
        type="checkbox"
        checked={checked}
        onChange={(event) => onChange(event.target.checked)}
        disabled={disabled}
      />
      <span>{label}</span>
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
    objects: {},
  };
}

function defaultPoseSourceObject(): PoseSourceObjectBindingConfig {
  return {
    entity: '',
    frameId: '',
    applyPolicy: 'observe_only',
    confidenceMin: null,
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
