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
  motionDescriptor: ManualControlMotionDescriptor | null;
  gripperDescriptor: ManualControlGripperDescriptor | null;
}

interface RuntimeConfiguration {
  index: number;
  runtimeUnitId: string;
  configuration: Record<string, JsonValue>;
}

interface MotionTargetLimit {
  runtimeIndex: number;
  componentId: string;
  targetId: string;
  value: number;
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
  openStateRef: string;
  closedStateRef: string;
  commandDurationS: number;
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
  const graph = objectValue(document?.graph);
  const runtimeUnits = graph?.runtime_units;
  if (!Array.isArray(runtimeUnits)) return [];
  return runtimeUnits.flatMap((value, index) => {
    const runtime = objectValue(value);
    const configuration = objectValue(runtime?.configuration);
    const runtimeUnitId = runtime?.runtime_unit_id;
    return configuration !== null && typeof runtimeUnitId === 'string'
      ? [{ index, runtimeUnitId, configuration }]
      : [];
  });
}

function motionTargetLimits(runtimes: RuntimeConfiguration[]): MotionTargetLimit[] {
  return runtimes.flatMap((runtime) => {
    const rows = runtime.configuration.motion_target_limits;
    if (!Array.isArray(rows)) return [];
    return rows.flatMap((value) => {
      const row = objectValue(value);
      const componentId = row?.controllable_component_id;
      const targetId = row?.command_target_id;
      const limit = numberValue(row?.maximum_tcp_linear_speed_mps);
      return typeof componentId === 'string'
        && typeof targetId === 'string'
        && limit !== null
        ? [{ runtimeIndex: runtime.index, componentId, targetId, value: limit }]
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
      const openStateRef = row?.open_state_ref;
      const closedStateRef = row?.closed_state_ref;
      const commandDurationS = numberValue(row?.command_duration_s);
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
        || typeof openStateRef !== 'string'
        || typeof closedStateRef !== 'string'
        || commandDurationS === null
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
        openStateRef,
        closedStateRef,
        commandDurationS,
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
    open_state_ref: draft.openStateRef,
    closed_state_ref: draft.closedStateRef,
    command_duration_s: draft.commandDurationS,
    touch_links: [...draft.touchLinks].sort(),
    mount_frame: draft.mountFrame,
    attach_frame: draft.attachFrame,
    tcp_frame: draft.tcpFrame,
    auxiliary_frames: [...draft.auxiliaryFrames].sort(),
  };
}

function normalizedStopProfile(
  document: Record<string, JsonValue> | null,
): { profile: DeploymentControlledStopProfile; derivedRows: number } | null {
  const policies = objectValue(document?.policies);
  const rows = policies?.stop_stage_parameters;
  if (!Array.isArray(rows)) return null;
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
          }]
        : [];
    });
    if (normalizedLimits.length !== limits.length || normalizedLimits.length === 0) return [];
    return [{
      stop_mode: mode,
      deceleration_time_s: deceleration,
      allow_natural_completion_on_short_path: natural,
      per_resource_limits: normalizedLimits,
    } satisfies DeploymentControlledStopProfile];
  });
  if (profiles.length === 0) return null;
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

function NumberDraft({
  label,
  value,
  unit,
  min,
  max,
  step,
  disabled,
  onCommit,
}: {
  label: string;
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
    <label className={`deployment-focused-field${invalid ? ' invalid' : ''}`}>
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
      {invalid && <small className="deployment-field-error">Value was rejected.</small>}
    </label>
  );
}

function OptionalNumberDraft({
  label,
  value,
  unit,
  disabled,
  onCommit,
}: {
  label: string;
  value: number | null;
  unit: string;
  disabled: boolean;
  onCommit: (value: number | null) => Promise<unknown> | unknown;
}) {
  const [draft, setDraft] = useState(value === null ? '' : String(value));
  const [invalid, setInvalid] = useState(false);
  useEffect(() => {
    setDraft(value === null ? '' : String(value));
    setInvalid(false);
  }, [value]);
  const commit = async () => {
    const parsed = draft.trim() === '' ? null : Number(draft);
    if (parsed !== null && (!Number.isFinite(parsed) || parsed <= 0 || parsed > 1000)) {
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
          min="0.000001"
          max="1000"
          step="any"
          placeholder="model only"
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
      {invalid && <small className="deployment-field-error">Enter a positive value, or leave blank.</small>}
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
  const disabled = !controller.mutationAvailable || controller.busy;
  const commit = (
    sourceJointName: string,
    field: 'maxVelocityRadS' | 'maxAccelerationRadS2' | 'maxJerkRadS3',
    value: number | null,
  ) => controller.mutate(
    [
      'graph', 'runtime_units', runtime.index, 'configuration',
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
                  unit="rad/s"
                  disabled={disabled}
                  onCommit={(value) => commit(limit.sourceJointName, 'maxVelocityRadS', value)}
                />
                <OptionalNumberDraft
                  label="Maximum acceleration"
                  value={draft?.maxAccelerationRadS2 ?? null}
                  unit="rad/s²"
                  disabled={disabled}
                  onCommit={(value) => commit(limit.sourceJointName, 'maxAccelerationRadS2', value)}
                />
                <OptionalNumberDraft
                  label="Maximum jerk"
                  value={draft?.maxJerkRadS3 ?? null}
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
        Blank fields use the robot-model limit. Draft changes become effective only after Deployment Apply.
      </p>
    </details>
  );
}

function ModelAndTargetSection({
  controller,
  sourceModel,
  motionDescriptor,
  limits,
  runtimes,
}: Pick<DeploymentFocusedEditorProps, 'controller' | 'sourceModel' | 'motionDescriptor'> & {
  limits: MotionTargetLimit[];
  runtimes: RuntimeConfiguration[];
}) {
  const source = sourceModel.status === 'ready' ? sourceModel.data : null;
  const componentId = motionDescriptor?.controllableComponentId ?? limits[0]?.componentId;
  const occurrence = source?.occurrences.find((item) => item.component_id === componentId)
    ?? source?.occurrences[0]
    ?? null;
  const editingDisabled = !controller.mutationAvailable || controller.busy;
  return (
    <details className="deployment-focused-section" open>
      <summary>Robot model and target limits</summary>
      <div className="deployment-focused-body">
        <div className="deployment-resolved-grid">
          <span><small>Robot source</small><strong>{occurrence?.source_robot_name ?? 'Unavailable'}</strong></span>
          <span><small>Model</small><strong>{occurrence?.source_model_name ?? 'Unavailable'}</strong></span>
          <span title={occurrence?.model_revision}><small>Model revision</small><strong>{shortIdentity(occurrence?.model_revision)}</strong></span>
          <span><small>Planning groups</small><strong>{occurrence?.srdf_groups.map((group) => group.source_group_name).join(', ') || 'not declared'}</strong></span>
        </div>
        {sourceModel.status !== 'ready' && (
          <p className="muted">
            {sourceModel.status === 'loading' ? 'Reading source-model facts.' : sourceModel.message}
          </p>
        )}
        {motionDescriptor !== null && (
          <div className="deployment-limit-table" role="table" aria-label="Source model joint limits">
            <div className="deployment-limit-row heading" role="row">
              <span>Joint</span><span>Position</span><span>Velocity</span><span>Acceleration</span>
            </div>
            {motionDescriptor.joints.map((joint) => (
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
          {limits.length === 0 && <p className="muted">No Deployment-owned motion target cap is declared.</p>}
          {limits.map((limit) => {
            const matching = motionDescriptor !== null
              && motionDescriptor.controllableComponentId === limit.componentId
              && motionDescriptor.commandTargetId === limit.targetId
              ? motionDescriptor.operationalLimit
              : null;
            const runtime = runtimes.find((item) => item.index === limit.runtimeIndex);
            return (
              <div className="deployment-target-limit" key={`${limit.componentId}:${limit.targetId}`}>
                <header>
                  <strong>{limit.componentId} / {limit.targetId}</strong>
                  <small>Compiler-validated Deployment input</small>
                </header>
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
                {matching !== null && motionDescriptor !== null && (
                  <>
                    <div className="deployment-resolved-grid compact">
                      <span><small>Catalog policy</small><strong>{format(matching.catalogPolicyMaximumTcpLinearSpeedMps)} m/s</strong></span>
                      <span><small>Deployment cap</small><strong>{format(matching.deploymentMaximumTcpLinearSpeedMps)} m/s</strong></span>
                      <span><small>Effective minimum</small><strong>{format(matching.effectiveMaximumTcpLinearSpeedMps)} m/s</strong></span>
                    </div>
                    {runtime !== undefined && (
                      <DirectControlJointLimitsEditor
                        controller={controller}
                        runtime={runtime}
                        descriptor={motionDescriptor}
                      />
                    )}
                  </>
                )}
              </div>
            );
          })}
        </div>
        <p className="field-help">
          Physical joint limits remain source-model facts. Runtime uses the conservative minimum of model, catalog-policy, Deployment, Program, node, and live-scale limits.
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
    <details className="deployment-focused-section">
      <summary>Native runtime configuration</summary>
      <div className="deployment-focused-body">
        {runtimes.length === 0 && <p className="muted">No native runtime configuration is declared.</p>}
        {runtimes.map((runtime) => {
          const prefix: Array<string | number> = [
            'graph', 'runtime_units', runtime.index, 'configuration',
          ];
          const positions = runtime.configuration.initial_joint_positions_rad;
          return (
            <section className="deployment-runtime-unit" key={runtime.runtimeUnitId}>
              <header><strong>{runtime.runtimeUnitId}</strong><small>Named authored inputs</small></header>
              <div className="deployment-focused-form-grid">
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
  normalized,
}: {
  controller: DeploymentWorkspaceController;
  normalized: { profile: DeploymentControlledStopProfile; derivedRows: number } | null;
}) {
  const [draft, setDraft] = useState<DeploymentControlledStopProfile | null>(
    normalized?.profile ?? null,
  );
  useEffect(() => {
    setDraft(normalized?.profile ?? null);
  }, [normalized]);
  const disabled = !controller.mutationAvailable || controller.busy;
  if (draft === null) {
    return (
      <details className="deployment-focused-section">
        <summary>Controlled Stop</summary>
        <div className="deployment-focused-body"><p className="muted">No coherent editable Controlled Stop contract is declared.</p></div>
      </details>
    );
  }
  const changed = normalized !== null
    && JSON.stringify(draft) !== JSON.stringify(normalized.profile);
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
    <details className="deployment-focused-section">
      <summary>Controlled Stop</summary>
      <div className="deployment-focused-body">
        <p className="field-help">
          One normalized profile controls {normalized?.derivedRows ?? 0} compiler-expanded route rows atomically. It is independent of nominal motion acceleration.
        </p>
        <div className="deployment-focused-form-grid">
          <label className="deployment-focused-field">
            <span>Stop mode</span>
            <select
              value={draft.stop_mode}
              disabled={disabled}
              onChange={(event) => setDraft({
                ...draft,
                stop_mode: event.target.value as DeploymentControlledStopProfile['stop_mode'],
              })}
            >
              <option value="soft_on_path">Soft on path</option>
              <option value="spring_back_hold">Spring-back hold</option>
            </select>
          </label>
          <NumberDraft
            label="Deceleration time"
            value={draft.deceleration_time_s}
            unit="s"
            min={0.001}
            max={60}
            disabled={disabled}
            onCommit={(value) => {
              setDraft({ ...draft, deceleration_time_s: value });
              return true;
            }}
          />
          <label className="deployment-focused-checkbox">
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
        </div>
        <div className="deployment-stop-limits">
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
        </div>
        <div className="button-row">
          <button
            type="button"
            className="primary-action"
            disabled={disabled || !changed}
            onClick={() => { void controller.updateControlledStop(draft); }}
          >Update Controlled Stop Draft</button>
          <button
            type="button"
            disabled={disabled || !changed || normalized === null}
            onClick={() => setDraft(normalized?.profile ?? null)}
          >Discard Stop edits</button>
        </div>
        <p className="field-help">Maximum Stop time, lead-in, sample period, jerk, and provider fallback are not declared as editable inputs by this native provider. Supported modes and hard-stop floors remain provider-owned.</p>
      </div>
    </details>
  );
}

function GripperSection({
  controller,
  descriptor,
  runtimes,
}: {
  controller: DeploymentWorkspaceController;
  descriptor: ManualControlGripperDescriptor | null;
  runtimes: RuntimeConfiguration[];
}) {
  const authored = useMemo(() => gripperTargetConfigurations(runtimes), [runtimes]);
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
    if (
      value.description.trim() === ''
      || value.openStateRef === ''
      || value.closedStateRef === ''
      || !Number.isFinite(value.commandDurationS)
      || value.commandDurationS <= 0
      || value.commandDurationS > 60
      || value.touchLinks.length === 0
      || value.mountFrame === ''
      || value.attachFrame === ''
      || value.tcpFrame === ''
    ) return 'Complete every required field and use a motion time above 0 and at most 60 s.';
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
        'graph', 'runtime_units', normalizedDraft.runtimeIndex, 'configuration',
        'gripper_target_configurations', normalizedDraft.rowIndex,
      ],
      'set',
      gripperConfigurationValue(normalizedDraft),
    );
    setError(result === null ? 'The Deployment owner rejected this gripper configuration.' : null);
  };
  return (
    <details className="deployment-focused-section">
      <summary>Current gripper configuration</summary>
      <div className="deployment-focused-body">
        {descriptor === null ? <p className="muted">No current addressed gripper descriptor is available.</p> : (
          <>
            <div className="deployment-resolved-grid">
              <span><small>Target</small><strong>{descriptor.controllableComponentId} / {descriptor.commandTargetId}</strong></span>
              <span><small>Group / command joint</small><strong>{descriptor.sourceGroupName ?? 'not declared'} / {descriptor.sourceCoordinateName}</strong></span>
              <span><small>Range</small><strong>{format(descriptor.minPosition)} to {format(descriptor.maxPosition)} {descriptor.unit}</strong></span>
              <span><small>Command / actuation</small><strong>{descriptor.commandKind.replaceAll('_', ' ')} / {descriptor.actuationKind?.replaceAll('_', ' ') ?? 'not configured'}</strong></span>
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
                    <span>Motion time</span>
                    <span className="deployment-number-with-unit">
                      <input
                        type="number"
                        min="0.000001"
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
                {error !== null && <p className="deployment-field-error" role="alert">{error}</p>}
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
  motionDescriptor,
  gripperDescriptor,
}: DeploymentFocusedEditorProps) {
  const document = controller.workspace?.draft.document ?? null;
  const runtimes = useMemo(() => runtimeConfigurations(document), [document]);
  const limits = useMemo(() => motionTargetLimits(runtimes), [runtimes]);
  const controlledStop = useMemo(() => normalizedStopProfile(document), [document]);
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
        motionDescriptor={motionDescriptor}
        limits={limits}
        runtimes={runtimes}
      />
      <NativeRuntimeSection controller={controller} runtimes={runtimes} />
      <ControlledStopSection controller={controller} normalized={controlledStop} />
      <GripperSection controller={controller} descriptor={gripperDescriptor} runtimes={runtimes} />
    </section>
  );
}
