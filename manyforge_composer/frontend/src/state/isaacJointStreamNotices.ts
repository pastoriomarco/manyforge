import { parseSceneRuntimeProjection } from '../api/parsers';
import type {
  SceneRuntimeConfiguredJointState,
  SceneRuntimeProjection,
} from '../types/api';
import type { HeadlessSourceSection } from '../types/headless';
import type { WorkbenchNotice } from './workbenchNotices';

function recordAt(value: unknown): Record<string, unknown> | null {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

/** Recover only an exact-looking configured binding so malformed status can be called unavailable. */
function rawConfiguredJointState(payload: unknown): SceneRuntimeConfiguredJointState | null {
  const configured = recordAt(payload)?.configured_joint_states;
  const first = Array.isArray(configured) ? recordAt(configured[0]) : null;
  if (first === null) return null;
  const { source_id: sourceId, object_id: objectId, policy_id: policyId, topic } = first;
  if (![sourceId, objectId, policyId, topic].every((part) => (
    typeof part === 'string' && part.length > 0 && new TextEncoder().encode(part).byteLength <= 256
  ))) return null;
  return {
    source_id: sourceId as string,
    object_id: objectId as string,
    policy_id: policyId as string,
    topic: topic as string,
  };
}

function unavailableNotice(
  expected: SceneRuntimeConfiguredJointState,
  reason: string,
): WorkbenchNotice {
  return {
    id: 'isaac.joint-state',
    severity: 'warning',
    domain: 'runtime',
    title: 'Isaac joint-state stream status unavailable',
    message: `The current observer status for ${expected.object_id} on ${expected.topic} cannot be verified. ${reason}`,
  };
}

function jointStateNotice(
  projection: SceneRuntimeProjection,
  expected: SceneRuntimeConfiguredJointState,
): WorkbenchNotice | null {
  const row = projection.state.pose_sources.find((candidate) => (
    candidate.source_id === expected.source_id
    && candidate.object_id === expected.object_id
    && candidate.policy_id === expected.policy_id
  ));
  if (row === undefined) {
    return {
      id: 'isaac.joint-state',
      severity: 'warning',
      domain: 'runtime',
      title: 'Waiting for Isaac joint-state feedback',
      message: `No current joint-state sample has been received for ${expected.object_id} from ${expected.topic}. Motion readiness remains unavailable until fresh feedback arrives.`,
    };
  }

  if (row.payload_quality === 'unknown') {
    const error = row.payload.source_error;
    return unavailableNotice(
      expected,
      `${error.code}: ${error.detail} (source ${error.source_key}).`,
    );
  }

  if (row.payload.kind !== 'joint_state' || row.payload.topic !== expected.topic) {
    return unavailableNotice(
      expected,
      'The observer row does not match the configured joint-state topic.',
    );
  }
  if (!row.fresh) {
    return {
      id: 'isaac.joint-state',
      severity: 'warning',
      domain: 'runtime',
      title: 'Isaac joint-state feedback is stale',
      message: `The last joint-state sample for ${expected.object_id} from ${expected.topic} is stale. Motion readiness remains unavailable until fresh feedback returns.`,
    };
  }
  return null;
}

/**
 * A current warning about the configured Isaac JointState bindings of the
 * active bundle's pose sources. The passive Headless scene-runtime section is
 * the sole input; this does not infer whether the simulator is stopped or
 * paused and starts no polling. One notice (the first configured binding that
 * needs attention) keeps the notice id unique.
 */
export function isaacJointStreamNotices(
  section: HeadlessSourceSection | null,
): WorkbenchNotice[] {
  if (section === null || section.payload === null) return [];

  let projection: SceneRuntimeProjection;
  try {
    projection = parseSceneRuntimeProjection(section.payload);
  } catch {
    const expected = rawConfiguredJointState(section.payload);
    return expected === null
      ? []
      : [unavailableNotice(expected, 'The scene-runtime projection is malformed.')];
  }

  // Older owners omit the member; an empty array means no joint stream is
  // configured. Neither case supports an Isaac-specific warning.
  const configured = projection.configured_joint_states ?? [];
  if (configured.length === 0) return [];

  const sectionCurrent = section.available
    && section.freshness === 'fresh'
    && section.knownGaps.length === 0
    && section.sourceGeneration !== null
    && section.sourceOwnerEpoch !== null
    && section.sourceOwnerId !== null;
  if (!sectionCurrent) {
    return [unavailableNotice(configured[0]!, 'The scene-runtime source is stale or unavailable.')];
  }

  for (const expected of configured) {
    const notice = jointStateNotice(projection, expected);
    if (notice !== null) return [notice];
  }
  return [];
}
