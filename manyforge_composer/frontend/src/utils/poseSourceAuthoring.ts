import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';

/**
 * Deployment authoring of pose sources: the `pose_source` components
 * (`components[].configuration.runtime_configuration`) and the per-object
 * links in `policies.scene_pose_sources` (one row per scene object).
 */

export type JsonObject = Record<string, JsonValue>;

export const POSE_APPLY_POLICIES = [
  ['observe_only', 'Observe only'],
  ['continuous_unattached', 'Continuous while unattached'],
  ['continuous', 'Continuous, including attached'],
  ['continuous_relative_to_attached', 'Continuous, relative to gripper when attached'],
  ['before_planning', 'Before planning'],
  ['between_nodes', 'Between motion nodes'],
] as const;

export type PoseKeyKind = 'simulation_entity' | 'detection3d';

/** One `policies.scene_pose_sources` row. Optional deadband members are absent when exact. */
export type PoseLink = {
  scene_resource_id: string;
  source_id: string;
  source_key: JsonObject;
  apply_policy: string;
  [field: string]: JsonValue;
};

export type Deadband = 'deadband_translation_m' | 'deadband_rotation_rad';
// Shown in mm and degrees; stored in m and rad. Absent means exact.
export const DEADBANDS: readonly [Deadband, string, number][] = [
  ['deadband_translation_m', 'mm', 1000],
  ['deadband_rotation_rad', '°', 180 / Math.PI],
];

export interface PoseSourceComponent {
  index: number;
  componentId: string;
  runtimeConfiguration: JsonObject;
}

export function jsonObject(value: JsonValue | undefined): JsonObject | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

export function poseSourceComponents(document: JsonObject | null): PoseSourceComponent[] {
  const components = Array.isArray(document?.components) ? document.components : [];
  return components.flatMap((value, index) => {
    const component = jsonObject(value);
    const configuration = jsonObject(component?.configuration);
    const componentId = component?.component_id;
    return configuration?.runtime_kind === 'pose_source' && typeof componentId === 'string' ? [{
      index,
      componentId,
      runtimeConfiguration: jsonObject(configuration.runtime_configuration) ?? {},
    }] : [];
  });
}

/** The key kinds a component offers: `simulation_entity` needs `simulation`, `detection3d` needs `detection3d`. */
export function offeredKeyKinds(component: PoseSourceComponent | undefined): PoseKeyKind[] {
  const configuration = component?.runtimeConfiguration;
  return [
    ...(jsonObject(configuration?.simulation) ? ['simulation_entity' as const] : []),
    ...(jsonObject(configuration?.detection3d) ? ['detection3d' as const] : []),
  ];
}

export function keyKind(link: PoseLink): PoseKeyKind | null {
  if (jsonObject(link.source_key.simulation_entity)) return 'simulation_entity';
  if (jsonObject(link.source_key.detection3d)) return 'detection3d';
  return null;
}

export function defaultSourceKey(kind: PoseKeyKind): JsonObject {
  return kind === 'simulation_entity'
    ? { simulation_entity: { entity: '', source_header_frame_id: '' } }
    : { detection3d: {
      class_id: '', detection_id: '', confidence_min: 0, max_position_variance_m2: 0.0025, selection: 'first',
    } };
}

export function poseLinks(document: JsonObject | null): PoseLink[] {
  const policies = jsonObject(document?.policies);
  return (Array.isArray(policies?.scene_pose_sources) ? policies.scene_pose_sources : [])
    .flatMap((value) => {
      const row = jsonObject(value);
      return row === null ? [] : [{ ...row, source_key: jsonObject(row.source_key) ?? {} } as PoseLink];
    });
}

/** Replaces (or with null removes) the one link of an object; writes `['policies']`. */
export async function savePoseLink(
  controller: DeploymentWorkspaceController,
  sceneResourceId: string,
  replacement: PoseLink | null,
) {
  const document = controller.workspace?.draft.document ?? null;
  const policies = jsonObject(document?.policies) ?? {};
  const rows = poseLinks(document).filter((row) => row.scene_resource_id !== sceneResourceId);
  if (replacement) rows.push(replacement);
  rows.sort((a, b) => a.source_id.localeCompare(b.source_id)
    || a.scene_resource_id.localeCompare(b.scene_resource_id));
  const updated = { ...policies };
  if (rows.length) updated.scene_pose_sources = rows;
  else delete updated.scene_pose_sources;
  return controller.mutate(['policies'], 'set', updated);
}

export function poseStreamId(sceneResourceId: string): string {
  return `poses/${sceneResourceId}`;
}

/** The object's pose lifetime: its `component_streams` budget, else the source's `stream_budget_ns`. */
export function effectiveStreamBudgetNs(
  document: JsonObject | null,
  component: PoseSourceComponent | undefined,
  sceneResourceId: string,
): number | null {
  const streams = jsonObject(document?.policies)?.component_streams;
  const row = (Array.isArray(streams) ? streams : []).map(jsonObject).find((item) => (
    item?.controllable_component_id === component?.componentId
    && item?.stream_id === poseStreamId(sceneResourceId)
    && typeof item?.budget_ns === 'number'
  ));
  const budget = row?.budget_ns ?? component?.runtimeConfiguration.stream_budget_ns;
  return typeof budget === 'number' ? budget : null;
}

/** The default header frame for an entity: its last path segment (stored explicitly). */
export function entityHeaderFrame(entity: string): string {
  return entity.split('/').filter(Boolean).at(-1) ?? '';
}

/** Sets an entity and, while the header frame is empty, prefills it from the entity. */
export function withEntity(value: JsonObject, entity: string): JsonObject {
  const header = typeof value.source_header_frame_id === 'string' ? value.source_header_frame_id : '';
  return { ...value, entity, source_header_frame_id: header || entityHeaderFrame(entity) };
}

export function keySummary(link: PoseLink): string {
  const simulation = jsonObject(link.source_key.simulation_entity);
  if (simulation) return String(simulation.entity ?? '');
  const detection = jsonObject(link.source_key.detection3d);
  if (detection) return `detection ${String(detection.class_id || 'any class')}`;
  return 'no key';
}
