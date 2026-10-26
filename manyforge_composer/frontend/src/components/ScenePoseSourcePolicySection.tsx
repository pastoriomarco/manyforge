import { useState } from 'react';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue, SceneRuntimePoseSource } from '../types/api';

const POLICIES = [
  ['observe_only', 'Observe only'],
  ['continuous_unattached', 'Continuous while unattached'],
  ['continuous', 'Continuous, including attached'],
  ['continuous_relative_to_attached', 'Continuous, relative to gripper when attached'],
  ['before_planning', 'Before planning'],
  ['between_nodes', 'Between motion nodes'],
] as const;

type Binding = { source_id: string; scene_resource_id: string; apply_policy: string };
const object = (value: JsonValue | undefined): Record<string, JsonValue> => (
  value !== null && typeof value === 'object' && !Array.isArray(value) ? value : {}
);

export function ScenePoseSourcePolicySection({ controller, sources }: {
  controller: DeploymentWorkspaceController;
  sources: readonly SceneRuntimePoseSource[];
}) {
  const document = controller.workspace?.draft.document;
  const policies = object(document?.policies);
  const configured = (Array.isArray(policies.scene_pose_sources)
    ? policies.scene_pose_sources : []) as Binding[];
  const available = new Map<string, { source: string; resource: string; inherited: string | null }>();
  const key = (source: string, resource: string) => JSON.stringify([source, resource]);
  for (const source of sources) {
    if (source.eligible) available.set(key(source.source_id, source.object_id), {
      source: source.source_id, resource: source.object_id, inherited: source.policy_id,
    });
  }
  for (const row of configured) {
    if (!available.has(key(row.source_id, row.scene_resource_id)))
      available.set(key(row.source_id, row.scene_resource_id), {
        source: row.source_id, resource: row.scene_resource_id, inherited: null,
      });
  }
  const [error, setError] = useState<string | null>(null);
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const change = async (source: string, resource: string, policy: string) => {
    const rows = configured.filter((row) => row.source_id !== source || row.scene_resource_id !== resource);
    if (policy) rows.push({ source_id: source, scene_resource_id: resource, apply_policy: policy });
    rows.sort((a, b) => a.source_id.localeCompare(b.source_id) || a.scene_resource_id.localeCompare(b.scene_resource_id));
    const updated = { ...policies };
    if (rows.length) updated.scene_pose_sources = rows;
    else delete updated.scene_pose_sources;
    const result = await controller.mutate(['policies'], 'set', updated);
    setError(result === null ? 'The Deployment owner rejected the pose-source policy.' : null);
  };
  return (
    <details className="dcard">
      <summary>Object pose sources</summary>
      <p className="field-help">Policies control when observations update the runtime Scene used for planning, collision checks and display. Save persists them; Apply activates them. Between-nodes updates apply to objects referenced by the next motion or pick; reset readiness updates the full configured set.</p>
      {[...available.values()].map((row) => (
        <label className="deployment-focused-field" key={key(row.source, row.resource)}>
          <span>{row.resource} · {row.source}</span>
          <select aria-label={`Pose policy for ${row.resource} from ${row.source}`} disabled={disabled}
            value={configured.find((item) => item.source_id === row.source && item.scene_resource_id === row.resource)?.apply_policy ?? ''}
            onChange={(event) => { void change(row.source, row.resource, event.target.value); }}>
            <option value="">Source default{row.inherited ? ` (${row.inherited})` : ''}</option>
            {POLICIES.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
          </select>
        </label>
      ))}
      {available.size === 0 && <p className="muted">No writable object pose sources are configured.</p>}
      {error && <p role="alert">{error}</p>}
    </details>
  );
}
