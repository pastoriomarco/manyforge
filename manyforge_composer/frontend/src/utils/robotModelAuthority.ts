import type {
  SceneUrdfJoint,
  SceneUrdfManifest,
} from '../types/api';

const DRIVABLE_TYPES = new Set(['revolute', 'prismatic', 'continuous']);

function isDrivable(joint: SceneUrdfJoint): boolean {
  return DRIVABLE_TYPES.has(joint.type);
}

export function robotControlJointsForGroup(
  manifest: SceneUrdfManifest | null,
  groupId: string | null | undefined,
): SceneUrdfJoint[] {
  if (manifest?.joints === undefined) return [];
  const byName = new Map(manifest.joints.map((joint) => [joint.name, joint]));
  const authorityNames =
    groupId != null && groupId.length > 0
      ? manifest.authority?.group_commandable_joint_names?.[groupId]
      : undefined;
  if (authorityNames !== undefined && authorityNames.length > 0) {
    return authorityNames
      .map((name) => byName.get(name))
      .filter((joint): joint is SceneUrdfJoint => joint !== undefined)
      .filter(isDrivable);
  }

  // Legacy/mock fallback: no Robot Model Authority metadata is available.
  // Hide mimic followers and masters so gripper chains stay under the
  // dedicated gripper widget instead of leaking into arm jogging.
  return manifest.joints.filter(
    (joint) =>
      isDrivable(joint) &&
      !joint.mimic &&
      !joint.is_mimic_master,
  );
}

export function robotControlJointNamesForGroup(
  manifest: SceneUrdfManifest | null,
  groupId: string | null | undefined,
): string[] {
  return robotControlJointsForGroup(manifest, groupId).map((joint) => joint.name);
}
