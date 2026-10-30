import { useEffect, useState } from 'react';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { CCLCStreamRecovery, CCLCStreamSeverity } from '../types/cclc';
import { streamLabel } from '../state/componentStreamHealthPresentation';
import type { FocusedEditorScope } from './DeploymentFocusedEditor';

const SEVERITIES: readonly CCLCStreamSeverity[] = ['fault', 'blocking', 'warning'];
const RECOVERIES: readonly CCLCStreamRecovery[] = ['ack', 'auto'];
/** Mirrors the host's pose-source stream declaration (one stream per binding). */
const POSE_SOURCE_STREAM_DEFAULTS = { severity: 'warning', recovery: 'auto' } as const;
/** Mirrors the host's AI model component stream declaration (`endpoint`, 10 s). */
const AI_MODEL_STREAM: DeclaredStream = {
  streamId: 'endpoint', budgetNs: 10_000_000_000, severity: 'warning', recovery: 'auto',
};

type Row = Record<string, JsonValue>;
type OverrideField = 'budget_ns' | 'severity' | 'recovery';

interface DeclaredStream {
  streamId: string;
  budgetNs: number;
  severity: CCLCStreamSeverity;
  recovery: CCLCStreamRecovery;
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
  const stream = (streamId: string) => ({ streamId, budgetNs, ...POSE_SOURCE_STREAM_DEFAULTS });
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

/** One component's declared streams: ROS `component_streams` rows, one per pose binding. */
function declaredStreams(runtime: Row, componentId: string, links: Row[] = []): DeclaredStream[] {
  const own = (row: Row) => row.controllable_component_id === componentId;
  const profile = rows(runtime.component_streams).filter(own).flatMap((row) => {
    const streamId = row.stream_id;
    const budgetNs = finite(row.budget_ns);
    return typeof streamId === 'string' && budgetNs !== null
      && SEVERITIES.includes(row.severity as CCLCStreamSeverity)
      && RECOVERIES.includes(row.recovery as CCLCStreamRecovery)
      ? [{ streamId, budgetNs, severity: row.severity as CCLCStreamSeverity,
        recovery: row.recovery as CCLCStreamRecovery }]
      : [];
  });
  const poses = rows(runtime.pose_sources).filter(own).flatMap(poseSourceBindings).flatMap(
    (binding) => (binding.lifetimeNs === null ? [] : [{
      streamId: poseStreamId(binding.objectId, binding.policyId),
      budgetNs: binding.lifetimeNs,
      ...POSE_SOURCE_STREAM_DEFAULTS,
    }]));
  const aiModels = rows(runtime.ai_models).filter(own).map(() => AI_MODEL_STREAM);
  return [...profile, ...poses, ...aiModels, ...authoredPoseStreams(runtime, componentId, links)];
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
 * CP9 component streams: each component's declared streams with their profile
 * defaults, and the Deployment `policies.component_streams` overrides. One
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
  const save = async (componentId: string, streamId: string, field: OverrideField, value: JsonValue | null) => {
    const isRow = (row: Row) => row.controllable_component_id === componentId && row.stream_id === streamId;
    const row: Row = { ...(overrides.find(isRow)
      ?? { controllable_component_id: componentId, stream_id: streamId }) };
    if (value === null) delete row[field];
    else row[field] = value;
    const next = overrides.filter((candidate) => !isRow(candidate));
    // A row needs at least one override besides its identity.
    if ('budget_ns' in row || 'severity' in row || 'recovery' in row) next.push(row);
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
              return (
                <fieldset key={stream.streamId} className="deployment-focused-form-grid">
                  <legend>{label}</legend>
                  <p className="muted">
                    {`Default: ${milliseconds(stream.budgetNs)} · ${stream.severity} · ${stream.recovery}`}
                  </p>
                  <BudgetOverride label={`${label} budget override for ${componentId}`}
                    value={budgetNs === null ? null : budgetNs / 1e6}
                    placeholder={`${milliseconds(stream.budgetNs)} (default)`} disabled={disabled}
                    onCommit={(ms) => save(componentId, stream.streamId, 'budget_ns',
                      ms === null ? null : Math.max(1, Math.round(ms * 1e6)))} />
                  <label className="deployment-focused-field">
                    <span>Severity</span>
                    <select aria-label={`${label} severity override for ${componentId}`} disabled={disabled}
                      value={typeof override?.severity === 'string' ? override.severity : ''}
                      onChange={(event) => { void save(componentId, stream.streamId, 'severity',
                        event.target.value || null); }}>
                      <option value="">{`Default (${stream.severity})`}</option>
                      {SEVERITIES.map((value) => <option key={value} value={value}>{value}</option>)}
                    </select>
                  </label>
                  <label className="deployment-focused-field">
                    <span>Recovery</span>
                    <select aria-label={`${label} recovery override for ${componentId}`} disabled={disabled}
                      value={typeof override?.recovery === 'string' ? override.recovery : ''}
                      onChange={(event) => { void save(componentId, stream.streamId, 'recovery',
                        event.target.value || null); }}>
                      <option value="">{`Default (${stream.recovery})`}</option>
                      {RECOVERIES.map((value) => <option key={value} value={value}>{value}</option>)}
                    </select>
                  </label>
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
              const values = [
                ...(budgetNs === null ? [] : [`budget ${milliseconds(budgetNs)}`]),
                ...(typeof row.severity === 'string' ? [`severity ${row.severity}`] : []),
                ...(typeof row.recovery === 'string' ? [`recovery ${row.recovery}`] : []),
              ];
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
        <p className="field-help">Defaults come from the component profile; overrides are Deployment rows (blank or Default keeps the default). Fault opens an incident (Full Stop), blocking holds new motion, warning only shows. Ack latches until Recover; auto clears when the stream is live again (a fault incident then closes by itself; nothing resumes).</p>
      </div>
    </details>
  );
}
