import type { SceneUrdfJoint, SceneUrdfManifest } from '../types/api';

const ACTUATED_TYPES = new Set(['revolute', 'prismatic', 'continuous']);

/**
 * Resolve an explicit, qualified target-joint list against structural model
 * data. I6 owns the component/target mapping. Missing names, duplicates, fixed
 * joints, and implicit group fallbacks fail closed here.
 */
export function modelJointsForQualifiedTarget(
  manifest: SceneUrdfManifest | null,
  targetJointNames: readonly string[] | null,
): SceneUrdfJoint[] | null {
  if (manifest === null || targetJointNames === null || targetJointNames.length === 0) {
    return null;
  }
  const unique = new Set(targetJointNames);
  if (unique.size !== targetJointNames.length) return null;
  const byName = new Map(manifest.joints.map((joint) => [joint.name, joint]));
  const selected: SceneUrdfJoint[] = [];
  for (const name of targetJointNames) {
    const joint = byName.get(name);
    if (!joint || !ACTUATED_TYPES.has(joint.type)) return null;
    selected.push(joint);
  }
  return selected;
}
