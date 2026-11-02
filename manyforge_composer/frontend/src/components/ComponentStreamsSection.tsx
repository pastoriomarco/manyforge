import { useEffect, useState } from 'react';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { streamLabel } from '../state/componentPresentation';
import type { JsonValue } from '../types/api';
import type { FocusedEditorScope } from './DeploymentFocusedEditor';

/** Mirrors the host's AI model component stream declaration (`endpoint`, 10 s). */
const AI_MODEL_STREAM: DeclaredStream = { streamId: 'endpoint', budgetNs: 10_000_000_000 };
/** IO-1: an I/O device's ROS inputs; declared by the host only with a budget row. */
const IO_INPUTS_STREAM: DeclaredStream = { streamId: 'inputs', budgetNs: null };
const ROS_INPUT_PROFILES = new Set(['ros_bool', 'ros2_control_gpio']);

type Row = Record<string, JsonValue>;

interface DeclaredStream {
  streamId: string;
  /** Null: no stream unless a Deployment row sets its budget. */
  budgetNs: number | null;
}

interface ComponentStreams {
  componentId: string;
  streams: DeclaredStream[];
}

export interface PoseSourceBinding {
  source: string;
  objectId: string;
  policyId: string;
  eligible: boolean | null;
  lifetimeNs: number | null;
}

const object = (value: JsonValue | undefined): Row | null => (
  value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null
);
const rows = (value: JsonValue | undefined): Row[] => (
  Array.isArray(value) ? value.flatMap((item) => {
    const row = object(item);
    return row === null ? [] : [row];
  }) : []
);
const finite = (value: JsonValue | undefined): number | null => (
  typeof value === 'number' && Number.isFinite(value) ? value : null
);
const compare = (a: string, b: string) => (a < b ? -1 : a > b ? 1 : 0);
const milliseconds = (ns: number) => `${Number((ns / 1e6).toFixed(3))} ms`;

/**
 * The bindings of one compiled pose_sources unit row: joint state, simulation
 * entities, detections (the host's binding and stream order).
 */
export function poseSourceBindings(row: Row): PoseSourceBinding[] {
  const binding = (source: string, value: JsonValue | undefined): PoseSourceBinding[] => {
    const item = object(value);
    return item === null ? [] : [{
      source,
      objectId: String(item.object_id ?? ''),
      policyId: String(item.policy_id ?? ''),
      eligible: typeof item.eligible === 'boolean' ? item.eligible : null,
      lifetimeNs: finite(item.lifetime_ns),
    }];
  };
  const list = (value: JsonValue | undefined) => (Array.isArray(value) ? value : []);
  return [
    ...binding('joint state', row.joint_state),
    ...list(row.simulation_entities).flatMap((item) => binding(
      String(object(item)?.entity ?? 'simulation entity'), item)),
    ...list(row.detection3d_sources).flatMap((item) => binding('detection 3D', item)),
  ];
}

/** The host's stream id of one pose binding (same rule as `PoseSourceStreamId`). */
const poseStreamId = (objectId: string, policyId: string) => (
  policyId === 'scene' ? `poses/${objectId}` : `poses/${objectId}/${policyId}`);

/**
 * Authored pose-source component (`pose_source.v2`): one stream per object link
 * naming it plus the robot overlays, each with the source's `stream_budget_ns`.
 */
function authoredPoseStreams(runtime: Row, componentId: string, links: Row[]): DeclaredStream[] {
  const budgetNs = finite(runtime.stream_budget_ns);
  if (budgetNs === null) return [];
  const stream = (streamId: string) => ({ streamId, budgetNs });
  const objects = links.filter((link) => link.source_id === componentId)
    .flatMap((link) => (typeof link.scene_resource_id === 'string'
      ? [stream(poseStreamId(link.scene_resource_id, 'scene'))] : []));
  const overlay = object(runtime.robot_overlay);
  const robot = overlay?.robot_component_id;
  const overlays = typeof robot === 'string' ? [
    ...(object(overlay?.joint_state) === null ? [] : [stream(poseStreamId(robot, 'viewer.measured_robot'))]),
    ...(object(overlay?.tcp_entity) === null ? [] : [stream(poseStreamId(robot, 'viewer.measured_tcp'))]),
  ] : [];
  return [...objects, ...overlays];
}

/**
 * One component's declared streams: ROS `component_streams` rows, one per pose
 * binding, and an I/O device's `inputs` when it has ROS inputs.
 */
function declaredStreams(runtime: Row, componentId: string, links: Row[] = []): DeclaredStream[] {
  const own = (row: Row) => row.controllable_component_id === componentId;
  const profile = rows(runtime.component_streams).filter(own).flatMap((row) => {
    const streamId = row.stream_id;
    const budgetNs = finite(row.budget_ns);
    return typeof streamId === 'string' && budgetNs !== null ? [{ streamId, budgetNs }] : [];
  });
  const poses = rows(runtime.pose_sources).filter(own).flatMap(poseSourceBindings).flatMap(
    (binding) => (binding.lifetimeNs === null ? [] : [{
      streamId: poseStreamId(binding.objectId, binding.policyId),
      budgetNs: binding.lifetimeNs,
    }]));
  const aiModels = rows(runtime.ai_models).filter(own).map(() => AI_MODEL_STREAM);
  const ioInputs = rows(runtime.io_signals).some((row) => own(row) && row.direction === 'input'
    && ROS_INPUT_PROFILES.has(String(object(row.provider)?.profile)));
  return [...profile, ...poses, ...aiModels, ...authoredPoseStreams(runtime, componentId, links),
    ...(ioInputs ? [IO_INPUTS_STREAM] : [])];
}

/** Every Deployment component (intent) or every component id found in unit rows (graph). */
function componentStreams(document: Row | null): ComponentStreams[] {
  if (document?.api_version === 'manyforge.deployment_intent.v2') {
    const links = rows(object(document.policies)?.scene_pose_sources);
    return rows(document.components).flatMap((component) => {
      const componentId = component.component_id;
      if (typeof componentId !== 'string') return [];
      const configuration = object(component.configuration);
      const runtime: Row = object(configuration?.runtime_configuration) ?? {};
      const streams = declaredStreams(runtime, componentId, links);
      return [{
        componentId,
        streams: configuration?.runtime_kind === 'ai_model' ? [...streams, AI_MODEL_STREAM] : streams,
      }];
    });
  }
  const found = new Map<string, ComponentStreams>();
  for (const unit of rows(object(document?.graph)?.runtime_units)) {
    const configuration: Row = object(unit.configuration) ?? {};
    const runtime = object(configuration.runtime_configuration) ?? configuration;
    for (const value of Object.values(runtime)) {
      for (const row of rows(value)) {
        const componentId = row.controllable_component_id;
        if (typeof componentId === 'string' && !found.has(componentId)) {
          found.set(componentId, { componentId, streams: declaredStreams(runtime, componentId) });
        }
      }
    }
  }
  return [...found.values()];
}

/** Optional positive budget in ms: commits on Enter/blur, Escape restores, blank clears. */
function BudgetOverride({ label, value, placeholder, disabled, onCommit }: {
  label: string;
  value: number | null;
  placeholder: string;
  disabled: boolean;
  onCommit: (value: number | null) => Promise<unknown>;
}) {
  const text = value === null ? '' : String(value);
  const [draft, setDraft] = useState(text);
  const [invalid, setInvalid] = useState(false);
  useEffect(() => { setDraft(text); setInvalid(false); }, [text]);
  const commit = async () => {
    const parsed = draft.trim() === '' ? null : Number(draft);
    if (parsed !== null && (!Number.isFinite(parsed) || parsed <= 0)) {
      setInvalid(true);
      return;
    }
    if (parsed === value) return;
    await onCommit(parsed);
  };
  return (
    <label className={`deployment-focused-field${invalid ? ' invalid' : ''}`}>
      <span>Budget</span>
      <span className="deployment-number-with-unit">
        <input type="number" min={0.000001} step="any" value={draft} placeholder={placeholder}
          aria-label={label} disabled={disabled}
          onChange={(event) => { setDraft(event.target.value); setInvalid(false); }}
          onBlur={() => { void commit(); }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              setDraft(text);
              setInvalid(false);
              event.currentTarget.blur();
            }
          }} />
        <small>ms</small>
      </span>
      {invalid && <small className="deployment-field-error inline-message severity-error">
        Enter a positive value, or leave blank.
      </small>}
    </label>
  );
}

/**
 * Component streams: each component's declared streams with their freshness
 * budget, and the Deployment `policies.component_streams` budget overrides. One
 * component's page shows only its streams; the cell settings only the
 * overrides whose stream no component declares.
 */
export function ComponentStreamsSection({ controller, scope = 'all' }: {
  controller: DeploymentWorkspaceController;
  scope?: FocusedEditorScope;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const policies = object(document?.policies) ?? {};
  const overrides = rows(policies.component_streams);
  const declared = componentStreams(document);
  // Override rows whose stream no component declares.
  const orphans = typeof scope === 'object' ? [] : overrides.filter((row) => !declared.some(({ componentId, streams }) => (
    row.controllable_component_id === componentId
    && streams.some((stream) => stream.streamId === row.stream_id))));
  const components = scope === 'cell' ? [] : declared.filter(({ componentId }) => (
    typeof scope !== 'object' || componentId === scope.componentId));
  if (components.length === 0 && orphans.length === 0) return null;
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const key = (componentId: string, streamId: string) => JSON.stringify([componentId, streamId]);
  // A refusal shows once, in the yellow Deployment notice row.
  const write = async (next: Row[]) => {
    const updated = { ...policies };
    if (next.length) updated.component_streams = next;
    else delete updated.component_streams;
    return controller.mutate(['policies'], 'set', updated);
  };
  const save = async (componentId: string, streamId: string, value: number | null) => {
    const isRow = (row: Row) => row.controllable_component_id === componentId && row.stream_id === streamId;
    const row: Row = { ...(overrides.find(isRow)
      ?? { controllable_component_id: componentId, stream_id: streamId }) };
    if (value === null) delete row.budget_ns;
    else row.budget_ns = value;
    const next = overrides.filter((candidate) => !isRow(candidate));
    // A row needs its budget besides its identity.
    if ('budget_ns' in row) next.push(row);
    next.sort((a, b) => (
      compare(String(a.controllable_component_id), String(b.controllable_component_id))
      || compare(String(a.stream_id), String(b.stream_id))
    ));
    return write(next);
  };
  return (
    <details className="dcard">
      <summary>Component streams</summary>
      <div className="deployment-focused-body">
        {components.map(({ componentId, streams }) => (
          <section key={componentId} aria-label={`${componentId} streams`}>
            <header><strong>{componentId}</strong></header>
            {streams.length === 0 && <p className="muted">No streams declared</p>}
            {streams.map((stream) => {
              const label = streamLabel(stream.streamId);
              const override = overrides.find((row) => (
                row.controllable_component_id === componentId && row.stream_id === stream.streamId));
              const budgetNs = finite(override?.budget_ns);
              const fallback = stream.budgetNs === null ? 'off' : milliseconds(stream.budgetNs);
              return (
                <fieldset key={stream.streamId} className="deployment-focused-form-grid">
                  <legend>{label}</legend>
                  <p className="muted">
                    {`Default: ${fallback}`}
                  </p>
                  <BudgetOverride label={`${label} budget override for ${componentId}`}
                    value={budgetNs === null ? null : budgetNs / 1e6}
                    placeholder={`${fallback} (default)`} disabled={disabled}
                    onCommit={(ms) => save(componentId, stream.streamId,
                      ms === null ? null : Math.max(1, Math.round(ms * 1e6)))} />
                </fieldset>
              );
            })}
          </section>
        ))}
        {orphans.length > 0 && (
          <section aria-label="Overrides without a declared stream">
            <header><strong>Overrides without a declared stream</strong></header>
            {orphans.map((row) => {
              const componentId = String(row.controllable_component_id);
              const streamId = String(row.stream_id);
              const budgetNs = finite(row.budget_ns);
              const values = budgetNs === null ? [] : [`budget ${milliseconds(budgetNs)}`];
              return (
                <div key={key(componentId, streamId)}>
                  <p>{`${componentId} · ${streamId}: ${values.join(' · ') || 'no values'}`}</p>
                  <div className="button-row">
                    <button type="button" disabled={disabled}
                      aria-label={`Remove ${streamId} override for ${componentId}`}
                      onClick={() => { void write(overrides.filter((candidate) => candidate !== row)); }}>Remove</button>
                  </div>
                </div>
              );
            })}
          </section>
        )}
        <p className="field-help">Defaults come from the component profile; an override is a Deployment row (blank keeps the default). A stream older than its budget is stale. What that means depends on the component: a robot's or an I/O device's own stream going stale is a fault (an I/O device's Inputs have no budget unless you set one, since change-only inputs may stay silent; losing every sender of its inputs is always its fault); a pose source's, camera's or AI model's only shows as stale and keeps Program conditions from reading it. In a simulator lane a simulated robot's or tool's stale stream is not a fault: the component keeps its state (an enabled robot stays enabled) with the reason "Simulation not playing", and a new command on it stops the run (as the operator's Stop) until the simulation plays and the operator presses Run.</p>
      </div>
    </details>
  );
}
