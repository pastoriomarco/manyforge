import { parseSceneRuntimeProjection } from '../api/parsers';
import type {
  SceneRuntimeExpectedJointState,
  SceneRuntimePoseSource,
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
function rawExpectedJointState(payload: unknown): SceneRuntimeExpectedJointState | null {
  const root = recordAt(payload);
  const observer = recordAt(root?.cell_observer);
  const expected = recordAt(observer?.expected_joint_state);
  if (expected === null) return null;
  const { object_id: objectId, policy_id: policyId, topic } = expected;
  if (![objectId, policyId, topic].every((part) => (
    typeof part === 'string' && part.length > 0 && new TextEncoder().encode(part).byteLength <= 256
  ))) return null;
  return {
    object_id: objectId as string,
    policy_id: policyId as string,
    topic: topic as string,
  };
}

function unavailableNotice(
  expected: SceneRuntimeExpectedJointState,
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

function currentBindingRow(
  projection: SceneRuntimeProjection,
): SceneRuntimePoseSource | null {
  const observer = projection.cell_observer;
  const expected = observer?.expected_joint_state;
  if (observer === undefined || expected === undefined || expected === null) return null;
  return projection.state.pose_sources.find((row) => (
    row.object_id === expected.object_id
    && row.policy_id === expected.policy_id
    && row.source_id === observer.source_id
    && row.source_epoch === `${observer.source_epoch_base}.${row.source_epoch_order}`
  )) ?? null;
}

/**
 * A current warning about the configured Isaac JointState observer. The
 * passive Headless scene-runtime section is the sole input; this does not
 * infer whether the simulator is stopped or paused and starts no polling.
 */
export function isaacJointStreamNotices(
  section: HeadlessSourceSection | null,
): WorkbenchNotice[] {
  if (section === null || section.payload === null) return [];

  let projection: SceneRuntimeProjection;
  try {
    projection = parseSceneRuntimeProjection(section.payload);
  } catch {
    const expected = rawExpectedJointState(section.payload);
    return expected === null
      ? []
      : [unavailableNotice(expected, 'The scene-runtime projection is malformed.')];
  }

  const observer = projection.cell_observer;
  const expected = observer?.expected_joint_state;
  // Older owners omit the binding, and explicit null means no joint stream is
  // configured. Neither case supports an Isaac-specific warning.
  if (expected === undefined || expected === null) return [];

  const sectionCurrent = section.available
    && section.freshness === 'fresh'
    && section.knownGaps.length === 0
    && section.sourceGeneration !== null
    && section.sourceOwnerEpoch !== null
    && section.sourceOwnerId !== null;
  if (!sectionCurrent) {
    return [unavailableNotice(expected, 'The scene-runtime source is stale or unavailable.')];
  }

  const configured = projection.state.configured_pose_source;
  if (
    observer === undefined
    || observer.availability !== 'available'
    || observer.process_presence !== 'present'
    || !observer.observation_current
    || projection.state.state !== 'open'
    || configured === undefined
    || configured.source_id !== observer.source_id
    || configured.source_epoch_base !== observer.source_epoch_base
    || !configured.freshness_open
  ) {
    return [unavailableNotice(expected, 'The configured observer is not verifiably open and current.')];
  }

  const row = currentBindingRow(projection);
  if (row === null) {
    return [{
      id: 'isaac.joint-state',
      severity: 'warning',
      domain: 'runtime',
      title: 'Waiting for Isaac joint-state feedback',
      message: `No current joint-state sample has been received for ${expected.object_id} from ${expected.topic}. Motion readiness remains unavailable until fresh feedback arrives.`,
    }];
  }

  if (row.payload_quality === 'unknown') {
    const error = row.payload.source_error;
    return [unavailableNotice(
      expected,
      `${error.code}: ${error.detail} (source ${error.source_key}).`,
    )];
  }

  if (row.payload.kind !== 'joint_state' || row.payload.topic !== expected.topic) {
    return [unavailableNotice(
      expected,
      'The observer row does not match the configured joint-state topic.',
    )];
  }
  if (!row.fresh) {
    return [{
      id: 'isaac.joint-state',
      severity: 'warning',
      domain: 'runtime',
      title: 'Isaac joint-state feedback is stale',
      message: `The last joint-state sample for ${expected.object_id} from ${expected.topic} is stale. Motion readiness remains unavailable until fresh feedback returns.`,
    }];
  }
  return [];
}
